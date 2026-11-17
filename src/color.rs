//! Precision preflight. Hardware capabilities are distinct from implemented HDR.
use super::*;
use smithay::backend::renderer::{
    element::{surface::WaylandSurfaceRenderElement, Kind},
    Bind, Color32F, ExportMem, Frame, ImportMem, Offscreen, Renderer, Texture, TextureFilter,
};
use std::collections::HashSet;

pub const HIGH_PRECISION_TEXTURE: &str = r#"#version 100
//_DEFINES_
#if defined(EXTERNAL)
#extension GL_OES_EGL_image_external : require
#endif
precision highp float;
#if defined(EXTERNAL)
uniform highp samplerExternalOES tex;
#else
uniform highp sampler2D tex;
#endif
uniform float alpha;
varying vec2 v_coords;
#if defined(DEBUG_FLAGS)
uniform float tint;
#endif
void main() {
    vec4 color=texture2D(tex,v_coords);
#if defined(NO_ALPHA)
    color=vec4(color.rgb,1.0);
#endif
    gl_FragColor=color*alpha;
}
"#;
fn f16(value: f32) -> u16 {
    // Probe values are positive normal powers-of-two/rational values; no NaNs.
    let bits = value.to_bits();
    (((bits >> 23) & 255).saturating_sub(112) as u16) << 10 | ((bits >> 13) & 1023) as u16
}
pub fn f16_to_f32(value: u16) -> f32 {
    let sign = (u32::from(value) & 0x8000) << 16;
    let exponent = (value >> 10) & 31;
    let mantissa = u32::from(value & 1023);
    if exponent == 0 {
        let magnitude = (mantissa as f32) * 2f32.powi(-24);
        if sign == 0 {
            magnitude
        } else {
            -magnitude
        }
    } else if exponent == 31 {
        f32::from_bits(sign | 0x7f800000 | (mantissa << 13))
    } else {
        f32::from_bits(sign | ((u32::from(exponent) + 112) << 23) | (mantissa << 13))
    }
}
fn measure(renderer: &mut GlesRenderer, format: Fourcc) -> Result<Value> {
    renderer.downscale_filter(TextureFilter::Nearest)?;
    renderer.upscale_filter(TextureFilter::Nearest)?;
    let width = 1024;
    let mut bytes = vec![];
    for x in 0..width {
        if format == Fourcc::Abgr2101010 {
            bytes.extend((x as u32 | (1023u32 << 10) | (3u32 << 30)).to_ne_bytes());
        } else {
            for value in [0.25 + x as f32 / 4096., 2., 0., 1.] {
                bytes.extend(f16(value).to_ne_bytes());
            }
        }
    }
    let program = renderer.compile_custom_texture_shader(HIGH_PRECISION_TEXTURE, &[])?;
    let texture = renderer.import_memory(&bytes, format, (width, 1).into(), false)?;
    let mut target: GlesRenderbuffer = renderer.create_buffer(format, (width, 1).into())?;
    let mut bound = renderer.bind(&mut target)?;
    let damage = Rectangle::from_size((width, 1).into());
    let mut direct = vec![0u8; bytes.len()];
    {
        let mut frame = renderer.render(&mut bound, (width, 1).into(), Transform::Normal)?;
        frame.override_default_tex_program(program, vec![]);
        frame.clear(Color32F::TRANSPARENT, &[damage])?;
        frame.render_texture_at(
            &texture,
            (0, 0).into(),
            1,
            1.,
            Transform::Normal,
            &[damage],
            &[],
            1.,
        )?;
        if format == Fourcc::Abgr16161616f {
            frame.with_context(|gl| unsafe {
                use smithay::backend::renderer::gles::ffi;
                let mut pack = 0;
                gl.GetIntegerv(ffi::PIXEL_PACK_BUFFER_BINDING, &mut pack);
                gl.BindBuffer(ffi::PIXEL_PACK_BUFFER, 0);
                gl.ReadPixels(
                    0,
                    0,
                    width,
                    1,
                    ffi::RGBA,
                    ffi::HALF_FLOAT,
                    direct.as_mut_ptr().cast(),
                );
                gl.BindBuffer(ffi::PIXEL_PACK_BUFFER, pack as u32);
            })?;
        }
        frame.finish()?.wait()?;
    }
    // Smithay 0.7 map_texture fixes its length at four bytes/pixel. Use the
    // public GL callback for FP16's eight bytes rather than touching its internals.
    let data = if format == Fourcc::Abgr16161616f {
        direct
    } else {
        let mapping =
            renderer.copy_framebuffer(&bound, Rectangle::from_size((width, 1).into()), format)?;
        renderer.map_texture(&mapping)?.to_vec()
    };
    if format == Fourcc::Abgr2101010 {
        let values: HashSet<_> = data
            .chunks_exact(4)
            .map(|p| u32::from_ne_bytes(p.try_into().unwrap()) & 1023)
            .collect();
        Ok(
            json!({"format":"rgb10_a2","distinct_red_values":values.len(),"preserves_10bit":values.len()>256,"texture_width":texture.width()}),
        )
    } else {
        let values: HashSet<_> = data
            .chunks_exact(8)
            .map(|p| u16::from_ne_bytes(p[..2].try_into().unwrap()))
            .collect();
        let green = data
            .chunks_exact(8)
            .map(|p| f16_to_f32(u16::from_ne_bytes(p[2..4].try_into().unwrap())))
            .fold(0f32, f32::max);
        Ok(
            json!({"format":"rgba16f","distinct_red_values":values.len(),"max_green":green,"preserves_more_than_8bit":values.len()>256,"preserves_above_sdr_white":green>1.}),
        )
    }
}
pub fn validate_fp16(renderer: &mut GlesRenderer) -> Result<()> {
    let measured = measure(renderer, Fourcc::Abgr16161616f);
    renderer.downscale_filter(TextureFilter::Linear)?;
    renderer.upscale_filter(TextureFilter::Linear)?;
    let report = measured?;
    anyhow::ensure!(
        report["preserves_more_than_8bit"] == true && report["preserves_above_sdr_white"] == true,
        "FP16 renderer loses HDR precision/range"
    );
    Ok(())
}
pub fn preflight(renderer: &mut GlesRenderer) -> Value {
    let probe = |result: Result<Value>| {
        result.unwrap_or_else(|error| json!({"available":false,"error":error.to_string()}))
    };
    let rgb10 = probe(measure(renderer, Fourcc::Abgr2101010));
    let fp16 = probe(measure(renderer, Fourcc::Abgr16161616f));
    let codec = |kind| match waybridge_codec::probe_10bit(kind) {
        Ok((depth, distinct, peak)) => {
            json!({"verified":true,"decoded_bits":depth,"distinct_luma_values":distinct,"peak_code":peak})
        }
        Err(error) => json!({"verified":false,"error":error}),
    };
    json!({"streaming_hdr":false,"status":"partial-experimental-scrgb","wire_color_spaces":["bt709_sdr"],"current_capture_bits":8,
        "gl_rgb10":rgb10,"gl_fp16":fp16,"nvenc_hevc_main10":codec(waybridge_codec::Codec::Hevc),"nvenc_av1_main10":codec(waybridge_codec::Codec::Av1),
        "experimental_subset":{"source":"explicit wp_color_manager_v1 WindowsScRGB, 80 nits/unit","capture":"RGBA16F linear composition","transport":"CUDA P010 BT2020/PQ HEVC/AV1 Main10, raw FP16 alpha16","default_enabled":false}, "limitations":["ICC and parametric descriptions are not supported","physical Windows HDR display and Main10 MFT remain unverified","requires same NVIDIA GL/CUDA/NVENC GPU; unsupported gamut uses SDR tone map"]})
}

use smithay::backend::renderer::{
    element::{surface::WaylandSurfaceTexture, Element, Id, RenderElement},
    gles::{GlesError, GlesFrame, GlesTexProgram},
    utils::{CommitCounter, DamageSet, OpaqueRegions},
};
use smithay::utils::{Buffer as BufferCoord, Physical, Scale as FactorScale};

pub struct Programs {
    linear: GlesTexProgram,
    sdr: GlesTexProgram,
}
impl Programs {
    pub fn new(renderer: &mut GlesRenderer) -> Result<Self> {
        let sdr = HIGH_PRECISION_TEXTURE.replace(
            "gl_FragColor=color*alpha;",
            r#"
            float a=color.a;
            vec3 s=a>0.000001?clamp(color.rgb/a,0.0,1.0):vec3(0.0);
            vec3 low=s/12.92;
            vec3 high=pow((s+0.055)/1.055,vec3(2.4));
            vec3 linear=mix(low,high,step(vec3(0.04045),s));
            gl_FragColor=vec4(linear*a,a)*alpha;
        "#,
        );
        Ok(Self {
            linear: renderer.compile_custom_texture_shader(HIGH_PRECISION_TEXTURE, &[])?,
            sdr: renderer.compile_custom_texture_shader(sdr, &[])?,
        })
    }
}
pub fn scrgb_ids(root: &WlSurface) -> HashSet<Id> {
    let mut ids = HashSet::new();
    with_surface_tree_downward(
        root,
        (),
        |_, _, _| TraversalAction::DoChildren(()),
        |surface, states, _| {
            if states
                .cached_state
                .get::<crate::color_management::SurfaceColor>()
                .current()
                .scrgb
            {
                ids.insert(Id::from_wayland_resource(surface));
            }
        },
        |_, _, _| true,
    );
    ids
}
pub struct ColorElement {
    inner: WaylandSurfaceRenderElement<GlesRenderer>,
    scrgb: bool,
    linear: GlesTexProgram,
    sdr: GlesTexProgram,
}
pub fn color_elements(
    elements: Vec<WaylandSurfaceRenderElement<GlesRenderer>>,
    ids: &HashSet<Id>,
    programs: &Programs,
) -> Vec<ColorElement> {
    elements
        .into_iter()
        .map(|inner| {
            let scrgb = ids.contains(inner.id());
            ColorElement {
                inner,
                scrgb,
                linear: programs.linear.clone(),
                sdr: programs.sdr.clone(),
            }
        })
        .collect()
}
impl Element for ColorElement {
    fn id(&self) -> &Id {
        self.inner.id()
    }
    fn current_commit(&self) -> CommitCounter {
        self.inner.current_commit()
    }
    fn src(&self) -> Rectangle<f64, BufferCoord> {
        self.inner.src()
    }
    fn transform(&self) -> Transform {
        self.inner.transform()
    }
    fn geometry(&self, scale: FactorScale<f64>) -> Rectangle<i32, Physical> {
        self.inner.geometry(scale)
    }
    fn damage_since(
        &self,
        scale: FactorScale<f64>,
        commit: Option<CommitCounter>,
    ) -> DamageSet<i32, Physical> {
        self.inner.damage_since(scale, commit)
    }
    fn opaque_regions(&self, scale: FactorScale<f64>) -> OpaqueRegions<i32, Physical> {
        self.inner.opaque_regions(scale)
    }
    fn alpha(&self) -> f32 {
        self.inner.alpha()
    }
    fn kind(&self) -> Kind {
        self.inner.kind()
    }
}
impl RenderElement<GlesRenderer> for ColorElement {
    fn draw(
        &self,
        frame: &mut GlesFrame<'_, '_>,
        src: Rectangle<f64, BufferCoord>,
        dst: Rectangle<i32, Physical>,
        damage: &[Rectangle<i32, Physical>],
        opaque: &[Rectangle<i32, Physical>],
    ) -> std::result::Result<(), GlesError> {
        if !self.scrgb {
            if let WaylandSurfaceTexture::SolidColor(color) = self.inner.texture() {
                let a = color.a();
                let convert = |value: f32| {
                    let s = if a > 0. {
                        (value / a).clamp(0., 1.)
                    } else {
                        0.
                    };
                    if s <= 0.04045 {
                        s / 12.92
                    } else {
                        ((s + 0.055) / 1.055).powf(2.4)
                    }
                };
                return frame.draw_solid(
                    dst,
                    damage,
                    Color32F::new(
                        convert(color.r()) * a,
                        convert(color.g()) * a,
                        convert(color.b()) * a,
                        a,
                    ) * self.inner.alpha(),
                );
            }
        }
        frame.override_default_tex_program(
            if self.scrgb {
                self.linear.clone()
            } else {
                self.sdr.clone()
            },
            vec![],
        );
        let result = self.inner.draw(frame, src, dst, damage, opaque);
        frame.clear_tex_program_override();
        result
    }
}
pub fn tonemap_fp16(data: &[u8], width: usize, height: usize, flip: bool) -> Vec<u8> {
    let mut rgba = vec![0; width * height * 4];
    let gamma = |value: f32| {
        if value <= 0.0031308 {
            12.92 * value
        } else {
            1.055 * value.powf(1. / 2.4) - 0.055
        }
    };
    for y in 0..height {
        for x in 0..width {
            let offset = ((if flip { height - 1 - y } else { y }) * width + x) * 8;
            let read = |c| {
                f16_to_f32(u16::from_ne_bytes([
                    data[offset + c * 2],
                    data[offset + c * 2 + 1],
                ]))
            };
            let alpha = if read(3).is_finite() {
                read(3).clamp(0., 1.)
            } else {
                0.
            };
            let mut color = [0f32; 3];
            for (c, value) in color.iter_mut().enumerate() {
                let v = if alpha > 0. { read(c) / alpha } else { 0. };
                *value = if v.is_finite() { v.max(0.) } else { 0. };
            }
            let luma = 0.2126 * color[0] + 0.7152 * color[1] + 0.0722 * color[2];
            for c in 0..3 {
                rgba[(y * width + x) * 4 + c] =
                    (gamma((color[c] / (1. + luma)).clamp(0., 1.)) * 255.).round() as u8;
            }
            rgba[(y * width + x) * 4 + 3] = (alpha * 255.).round() as u8;
        }
    }
    rgba
}
