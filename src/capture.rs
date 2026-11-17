//! Window composition and CPU/CUDA capture; GL resources stay on the compositor thread.
use crate::{color, metrics, network::Peer, video};
use anyhow::{bail, Result};
use smithay::{
    backend::{
        allocator::Fourcc,
        renderer::{
            element::{
                surface::{render_elements_from_surface_tree, WaylandSurfaceRenderElement},
                Kind,
            },
            gles::{GlesRenderbuffer, GlesRenderer},
            utils::draw_render_elements,
            Bind, Color32F, ExportMem, Frame, Offscreen, Renderer, TextureMapping,
        },
    },
    reexports::wayland_server::protocol::wl_surface::WlSurface,
    utils::{Logical, Point, Rectangle, Transform},
};
use std::{rc::Rc, sync::Arc, time::Instant};

/// Inspect downstream capacity without touching any GL resource. False leaves
/// the caller dirty; frame callbacks and control messages must still run.
pub fn ready(
    encoder: &video::Encoder,
    peer: &Peer,
    id: u32,
    bbox: Rectangle<i32, Logical>,
    scale: f64,
) -> bool {
    if !peer.video_ready(id) {
        metrics::increment("capture_wait_credit");
        return false;
    }
    let pixels: smithay::utils::Size<i32, smithay::utils::Physical> =
        bbox.size.to_physical_precise_round(scale);
    let estimate = (pixels.w.max(0) as usize)
        .saturating_mul(pixels.h.max(0) as usize)
        .saturating_mul(4);
    encoder.capture_ready(id, estimate)
}

#[allow(clippy::too_many_arguments)]
pub fn surface(
    renderer: &mut GlesRenderer,
    surface: &WlSurface,
    id: u32,
    sequence: u64,
    bbox: Rectangle<i32, Logical>,
    scale: f64,
    target: &mut Option<GlesRenderbuffer>,
    target_size: &mut (i32, i32),
    peer: Arc<Peer>,
    gpu_device: Option<&Rc<waybridge_codec::GpuDevice>>,
    gpu_source: &mut Option<waybridge_codec::GpuSource>,
    color_programs: Option<&color::Programs>,
    hdr_codec: Option<waybridge_codec::Codec>,
) -> Result<video::Frame> {
    let captured = Instant::now();
    let color_ids = color_programs
        .map(|_| color::scrgb_ids(surface))
        .unwrap_or_default();
    let linear_scene = !color_ids.is_empty();
    let capture_format = if linear_scene {
        Fourcc::Abgr16161616f
    } else {
        Fourcc::Abgr8888
    };
    // FP16 goes through a color-aware path; the original CUDA RGBA8 kernel must
    // never silently reinterpret half-float samples as eight-bit data.
    let gpu_device = gpu_device.filter(|_| {
        !linear_scene || (peer.hdr_allowed(id) && hdr_codec.is_some_and(|c| peer.supports(c, id)))
    });
    let size = bbox.size;
    let pixels = size.to_physical_precise_round(scale);
    if pixels.w <= 0
        || pixels.h <= 0
        || pixels.w > 8192
        || pixels.h > 8192
        || pixels.w as i64 * pixels.h as i64 > 16_777_216
    {
        bail!("surface {id} exceeds render allocation limit");
    }
    if gpu_device.is_none() && gpu_source.is_some() {
        renderer.with_context(|_| drop(gpu_source.take()))?;
    }
    if *target_size != (pixels.w, pixels.h)
        || target.as_ref().and_then(|target| target.format()) != Some(capture_format)
    {
        renderer.with_context(|_| drop(gpu_source.take()))?;
        *target = Some(renderer.create_buffer(capture_format, (pixels.w, pixels.h).into())?);
        *target_size = (pixels.w, pixels.h);
    }
    let started = Instant::now();
    let elements: Vec<WaylandSurfaceRenderElement<GlesRenderer>> =
        render_elements_from_surface_tree(
            renderer,
            surface,
            Point::from((-bbox.loc.x, -bbox.loc.y)).to_physical_precise_round(scale),
            scale,
            1.0,
            Kind::Unspecified,
        );
    let mut bound = renderer.bind(target.as_mut().unwrap())?;
    let damage = Rectangle::from_size(pixels);
    let mut source_object = (0i32, 0i32);
    let mut fp16 = if linear_scene {
        vec![0u8; pixels.w as usize * pixels.h as usize * 8]
    } else {
        vec![]
    };
    {
        let mut frame = renderer.render(&mut bound, pixels, Transform::Flipped180)?;
        frame.clear(Color32F::new(0., 0., 0., 0.), &[damage])?;
        if linear_scene {
            let colored = color::color_elements(elements, &color_ids, color_programs.unwrap());
            draw_render_elements(&mut frame, scale, &colored, &[damage])?;
            if gpu_device.is_none() {
                frame.with_context(|gl| unsafe {
                    use smithay::backend::renderer::gles::ffi;
                    let mut pack = 0;
                    gl.GetIntegerv(ffi::PIXEL_PACK_BUFFER_BINDING, &mut pack);
                    gl.BindBuffer(ffi::PIXEL_PACK_BUFFER, 0);
                    gl.ReadPixels(
                        0,
                        0,
                        pixels.w,
                        pixels.h,
                        ffi::RGBA,
                        ffi::HALF_FLOAT,
                        fp16.as_mut_ptr().cast(),
                    );
                    gl.BindBuffer(ffi::PIXEL_PACK_BUFFER, pack as u32);
                })?;
            }
        } else {
            draw_render_elements(&mut frame, scale, &elements, &[damage])?;
        }
        if gpu_device.is_some() {
            frame.with_context(|gl| unsafe {
                use smithay::backend::renderer::gles::ffi;
                gl.GetFramebufferAttachmentParameteriv(
                    ffi::FRAMEBUFFER,
                    ffi::COLOR_ATTACHMENT0,
                    ffi::FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                    &mut source_object.0,
                );
                gl.GetFramebufferAttachmentParameteriv(
                    ffi::FRAMEBUFFER,
                    ffi::COLOR_ATTACHMENT0,
                    ffi::FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                    &mut source_object.1,
                );
            })?;
        }
        frame.finish()?.wait()?;
    }
    metrics::record("composite", started.elapsed());
    if let Some(device) = gpu_device.filter(|_| {
        pixels.w >= 48
            && pixels.h >= 48
            && pixels.w <= 4096
            && pixels.h <= 2304
            && peer.gpu_allowed(id)
    }) {
        let started = Instant::now();
        let result = renderer.with_context(|_| -> std::result::Result<_, String> {
            if source_object.0 != smithay::backend::renderer::gles::ffi::RENDERBUFFER as i32
                || source_object.1 <= 0
            {
                return Err("expected a public GL renderbuffer framebuffer attachment".into());
            }
            if gpu_source.is_none() {
                *gpu_source = Some(waybridge_codec::GpuSource::new_mode(
                    device.clone(),
                    source_object.1 as u32,
                    pixels.w as u32,
                    pixels.h as u32,
                    linear_scene,
                )?);
                tracing::info!(
                    id,
                    width = pixels.w,
                    height = pixels.h,
                    "CUDA GL color input registered; independent alpha may still read back"
                );
            }
            gpu_source.as_mut().unwrap().capture(true)
        })?;
        match result {
            Ok(gpu) => {
                metrics::record("gpu_capture", started.elapsed());
                metrics::record("capture", captured.elapsed());
                return Ok(video::Frame {
                    id,
                    width: pixels.w as u32,
                    height: pixels.h as u32,
                    sequence,
                    geometry: [size.w, size.h, bbox.loc.x, bbox.loc.y],
                    rgba: vec![],
                    gpu: Some(gpu),
                    straight: true,
                    peer,
                    video_eligible: false,
                    enqueued: Instant::now(),
                    captured,
                });
            }
            Err(error) => {
                peer.disable_gpu(id);
                renderer.with_context(|_| drop(gpu_source.take()))?;
                tracing::warn!(id,%error,"CUDA GL capture failed; using CPU color path for this viewer/window");
            }
        }
    }
    if linear_scene {
        if gpu_device.is_some() {
            let mut frame = renderer.render(&mut bound, pixels, Transform::Flipped180)?;
            frame.with_context(|gl| unsafe {
                use smithay::backend::renderer::gles::ffi;
                let mut pack = 0;
                gl.GetIntegerv(ffi::PIXEL_PACK_BUFFER_BINDING, &mut pack);
                gl.BindBuffer(ffi::PIXEL_PACK_BUFFER, 0);
                gl.ReadPixels(
                    0,
                    0,
                    pixels.w,
                    pixels.h,
                    ffi::RGBA,
                    ffi::HALF_FLOAT,
                    fp16.as_mut_ptr().cast(),
                );
                gl.BindBuffer(ffi::PIXEL_PACK_BUFFER, pack as u32);
            })?;
            frame.finish()?.wait()?;
        }
        let started = Instant::now();
        let rgba = color::tonemap_fp16(&fp16, pixels.w as usize, pixels.h as usize, true);
        metrics::record("hdr_tonemap_cpu", started.elapsed());
        metrics::record("capture", captured.elapsed());
        return Ok(video::Frame {
            id,
            width: pixels.w as u32,
            height: pixels.h as u32,
            sequence,
            geometry: [size.w, size.h, bbox.loc.x, bbox.loc.y],
            rgba,
            gpu: None,
            straight: true,
            peer,
            video_eligible: false,
            enqueued: Instant::now(),
            captured,
        });
    }
    let started = Instant::now();
    let mapping = renderer.copy_framebuffer(
        &bound,
        Rectangle::from_size((pixels.w, pixels.h).into()),
        Fourcc::Abgr8888,
    )?;
    let flipped = mapping.flipped();
    let bytes = renderer.map_texture(&mapping)?;
    let stride = pixels.w as usize * 4;
    let mut rgba = vec![0; stride * pixels.h as usize];
    for row in 0..pixels.h as usize {
        let source = if flipped {
            pixels.h as usize - 1 - row
        } else {
            row
        };
        rgba[row * stride..(row + 1) * stride]
            .copy_from_slice(&bytes[source * stride..(source + 1) * stride]);
    }
    metrics::record("readback", started.elapsed());
    metrics::record("capture", captured.elapsed());
    Ok(video::Frame {
        id,
        width: pixels.w as u32,
        height: pixels.h as u32,
        sequence,
        geometry: [size.w, size.h, bbox.loc.x, bbox.loc.y],
        rgba,
        gpu: None,
        straight: false,
        peer,
        video_eligible: false,
        enqueued: Instant::now(),
        captured,
    })
}
