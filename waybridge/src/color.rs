//! Precision preflight. Hardware capabilities are distinct from implemented HDR.
use super::*;
use smithay::backend::renderer::{ImportMem, Texture, TextureFilter};
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
    json!({"streaming_hdr":false,"status":"partial-preflight","wire_color_spaces":["bt709_sdr"],"current_capture_bits":8,
        "gl_rgb10":rgb10,"gl_fp16":fp16,"nvenc_hevc_main10":codec(waybridge_codec::Codec::Hevc),"nvenc_av1_main10":codec(waybridge_codec::Codec::Av1),
        "missing":["Wayland color descriptions and per-surface color conversion","FP16 application capture into P010 Main10 transport","Windows P010 decoding and scRGB FP16 composition"]})
}
