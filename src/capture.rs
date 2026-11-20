//! Window composition and CPU/CUDA capture; GL resources stay on the compositor thread.
use crate::{
    color,
    media_budget::{self, Budget, Cost, Lease},
    metrics,
    network::Peer,
    video,
};
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

pub struct Target {
    pub buffer: GlesRenderbuffer,
    // Buffer must be destroyed before its ticket is returned.
    _lease: Lease,
    pub touched: Instant,
}

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
    target: &mut Option<Target>,
    target_size: &mut (i32, i32),
    peer: Arc<Peer>,
    gpu_device: Option<&Rc<waybridge_codec::GpuDevice>>,
    gpu_source: &mut Option<waybridge_codec::GpuSource>,
    color_programs: Option<&color::Programs>,
    hdr_codec: Option<waybridge_codec::Codec>,
    budget: &Budget,
    video_codec: Option<waybridge_codec::Codec>,
) -> Result<Option<video::Frame>> {
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
        || target.as_ref().and_then(|target| target.buffer.format()) != Some(capture_format)
    {
        // Release the old registration and attachment before reserving the new
        // size; old frame/codec pool references keep their own native tickets.
        renderer.with_context(|_| {
            drop(gpu_source.take());
            drop(target.take());
        })?;
        *target_size = (0, 0);
    }
    let count = pixels.w as usize * pixels.h as usize;
    let gpu_device = gpu_device.filter(|_| {
        pixels.w >= 48
            && pixels.h >= 48
            && pixels.w <= 4096
            && pixels.h <= 2304
            && peer.gpu_allowed(id)
    });
    let mut gpu_device = gpu_device;
    let selected_codec = if linear_scene {
        hdr_codec.or(video_codec)
    } else {
        video_codec
    };
    let mut budget_video = selected_codec.is_some_and(|codec| peer.supports(codec, id))
        && pixels.w >= 48
        && pixels.h >= 48
        && pixels.w <= 4096
        && pixels.h <= 2304;
    let minimum = Cost {
        cpu: count * (if linear_scene { 24 } else { 16 }) + 65536,
        gpu: count * (if linear_scene { 8 } else { 4 }),
        encoders: 0,
    };
    if budget_video {
        let pool = if gpu_device.is_some() {
            media_budget::cuda_pool(pixels.w as u32, pixels.h as u32, linear_scene)
        } else {
            Cost::default()
        };
        let codec = selected_codec.unwrap();
        let working_set = minimum
            .add(pool)
            .and_then(|c| {
                c.add(media_budget::encoder(
                    pixels.w as u32,
                    pixels.h as u32,
                    codec.hdr(),
                    gpu_device.is_some(),
                ))
            })
            .unwrap();
        if !budget.fits_limits(working_set) {
            // A stream's minimum working set cannot fit even on an empty budget.
            // Use temporary CPU/PNG for this capture, without poisoning capability.
            budget_video = false;
            gpu_device = None;
            renderer.with_context(|_| drop(gpu_source.take()))?;
            metrics::increment("media_video_workset_exceeds_limit");
        }
    }
    let frame_cost = Cost::cpu(count * 16 + 65536);
    let scratch_cost = Cost::cpu(if linear_scene { count * 8 } else { 0 });
    let target_cost = Cost::gpu(if target.is_none() {
        count * if linear_scene { 8 } else { 4 }
    } else {
        0
    });
    let pool_cost = if gpu_device.is_some() && gpu_source.is_none() {
        media_budget::cuda_pool(pixels.w as u32, pixels.h as u32, linear_scene)
    } else {
        Cost::default()
    };
    // Reserve the whole capture atomically. Failed partial admission must not
    // leave misleading pressure after rolling back its temporary reservations.
    let total = frame_cost
        .add(scratch_cost)
        .and_then(|c| c.add(target_cost))
        .and_then(|c| c.add(pool_cost))
        .unwrap();
    let Some(mut frame_lease) = budget.acquire(id, total) else {
        return Ok(None);
    };
    let _scratch = frame_lease.split(scratch_cost);
    let pool_owner = if pool_cost != Cost::default() {
        Some(Arc::new(frame_lease.split(pool_cost)))
    } else {
        None
    };
    if target.is_none() {
        let lease = frame_lease.split(target_cost);
        *target = Some(Target {
            buffer: renderer.create_buffer(capture_format, (pixels.w, pixels.h).into())?,
            _lease: lease,
            touched: Instant::now(),
        });
        *target_size = (pixels.w, pixels.h);
    }
    target.as_mut().unwrap().touched = Instant::now();
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
    let mut bound = renderer.bind(&mut target.as_mut().unwrap().buffer)?;
    let damage = Rectangle::from_size(pixels);
    let mut source_object = (0i32, 0i32);
    let mut fp16 = vec![];
    {
        let mut frame = renderer.render(&mut bound, pixels, Transform::Flipped180)?;
        frame.clear(Color32F::new(0., 0., 0., 0.), &[damage])?;
        if linear_scene {
            let colored = color::color_elements(elements, &color_ids, color_programs.unwrap());
            draw_render_elements(&mut frame, scale, &colored, &[damage])?;
            if gpu_device.is_none() {
                fp16.resize(count * 8, 0);
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
                *gpu_source = Some(waybridge_codec::GpuSource::new_mode_with_owner(
                    device.clone(),
                    source_object.1 as u32,
                    pixels.w as u32,
                    pixels.h as u32,
                    linear_scene,
                    pool_owner
                        .clone()
                        .expect("new source reserved before rendering"),
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
                return Ok(Some(video::Frame {
                    id,
                    width: pixels.w as u32,
                    height: pixels.h as u32,
                    sequence,
                    geometry: [size.w, size.h, bbox.loc.x, bbox.loc.y],
                    rgba: vec![],
                    gpu: Some(gpu),
                    straight: true,
                    peer,
                    codec: video_codec,
                    video_eligible: false,
                    budget_video,
                    enqueued: Instant::now(),
                    captured,
                    _lease: frame_lease,
                }));
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
            fp16.resize(count * 8, 0);
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
        return Ok(Some(video::Frame {
            id,
            width: pixels.w as u32,
            height: pixels.h as u32,
            sequence,
            geometry: [size.w, size.h, bbox.loc.x, bbox.loc.y],
            rgba,
            gpu: None,
            straight: true,
            peer,
            codec: video_codec,
            video_eligible: false,
            budget_video,
            enqueued: Instant::now(),
            captured,
            _lease: frame_lease,
        }));
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
    Ok(Some(video::Frame {
        id,
        width: pixels.w as u32,
        height: pixels.h as u32,
        sequence,
        geometry: [size.w, size.h, bbox.loc.x, bbox.loc.y],
        rgba,
        gpu: None,
        straight: false,
        peer,
        codec: video_codec,
        video_eligible: false,
        budget_video,
        enqueued: Instant::now(),
        captured,
        _lease: frame_lease,
    }))
}
