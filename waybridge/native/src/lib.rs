mod gpu;
pub use gpu::{GpuDevice, GpuFrame, GpuSource};
use std::{
    ffi::{c_char, c_int, c_void, CStr},
    ptr::NonNull,
};

unsafe extern "C" {
    fn wb_hdr_probe_10bit(
        kind: c_int,
        depth: *mut c_int,
        distinct: *mut c_int,
        peak: *mut c_int,
        error: *mut c_char,
        size: usize,
    ) -> c_int;
    fn wb_encoder_new_codec(
        kind: c_int,
        width: c_int,
        height: c_int,
        fps: c_int,
        bitrate: c_int,
        error: *mut c_char,
        size: usize,
    ) -> *mut c_void;
    fn wb_encoder_new_gpu_codec(
        kind: c_int,
        width: c_int,
        height: c_int,
        fps: c_int,
        bitrate: c_int,
        frame: *const c_void,
        error: *mut c_char,
        size: usize,
    ) -> *mut c_void;
    fn wb_encoder_encode_gpu(
        encoder: *mut c_void,
        frame: *mut c_void,
        sequence: i64,
        force_idr: c_int,
        output: *mut *const u8,
        pts: *mut i64,
        key: *mut c_int,
    ) -> c_int;
    fn wb_encoder_free(encoder: *mut c_void);
    fn wb_encoder_encode(
        encoder: *mut c_void,
        rgba: *const u8,
        sequence: i64,
        force_idr: c_int,
        output: *mut *const u8,
        pts: *mut i64,
        key: *mut c_int,
    ) -> c_int;
}

/// Wire kinds are explicit: kind 3 is reserved for audio.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
#[repr(u8)]
pub enum Codec {
    H264 = 2,
    Hevc = 4,
    Av1 = 5,
}
impl Codec {
    pub const ALL: [Self; 3] = [Self::H264, Self::Hevc, Self::Av1];
    pub fn capability(self) -> &'static str {
        match self {
            Self::H264 => "h264_alpha",
            Self::Hevc => "hevc_alpha",
            Self::Av1 => "av1_alpha",
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Self::H264 => "H.264",
            Self::Hevc => "HEVC",
            Self::Av1 => "AV1",
        }
    }
}

pub struct Encoder {
    handle: NonNull<c_void>,
    bytes: usize,
}

impl Encoder {
    pub fn new(width: u32, height: u32, fps: u32, bitrate: u32) -> Result<Self, String> {
        Self::with_codec(Codec::H264, width, height, fps, bitrate)
    }
    pub fn with_codec(
        codec: Codec,
        width: u32,
        height: u32,
        fps: u32,
        bitrate: u32,
    ) -> Result<Self, String> {
        Self::new_input(codec, width, height, fps, bitrate, None)
    }
    pub fn with_gpu(
        codec: Codec,
        width: u32,
        height: u32,
        fps: u32,
        bitrate: u32,
        frame: &GpuFrame,
    ) -> Result<Self, String> {
        Self::new_input(codec, width, height, fps, bitrate, Some(frame))
    }
    fn new_input(
        codec: Codec,
        width: u32,
        height: u32,
        fps: u32,
        bitrate: u32,
        frame: Option<&GpuFrame>,
    ) -> Result<Self, String> {
        if !(48..=4096).contains(&width)
            || !(48..=2304).contains(&height)
            || !(1..=120).contains(&fps)
            || !(1000..=200000).contains(&bitrate)
        {
            return Err("Invalid NVENC dimensions or rate".into());
        }
        let mut error = [0 as c_char; 256];
        let pointer = unsafe {
            if let Some(frame) = frame {
                wb_encoder_new_gpu_codec(
                    codec as c_int,
                    width as c_int,
                    height as c_int,
                    fps as c_int,
                    bitrate as c_int,
                    frame.av_frame(),
                    error.as_mut_ptr(),
                    error.len(),
                )
            } else {
                wb_encoder_new_codec(
                    codec as c_int,
                    width as c_int,
                    height as c_int,
                    fps as c_int,
                    bitrate as c_int,
                    error.as_mut_ptr(),
                    error.len(),
                )
            }
        };
        NonNull::new(pointer)
            .map(|handle| Self {
                handle,
                bytes: width as usize * height as usize * 4,
            })
            .ok_or_else(|| {
                unsafe { CStr::from_ptr(error.as_ptr()) }
                    .to_string_lossy()
                    .into_owned()
            })
    }
    pub fn encode(
        &mut self,
        rgba: &[u8],
        sequence: u64,
        force_idr: bool,
    ) -> Result<(Vec<u8>, bool), String> {
        if rgba.len() != self.bytes {
            return Err("RGBA frame length does not match encoder dimensions".into());
        }
        let mut output = std::ptr::null();
        let mut pts = 0;
        let mut key = 0;
        let result = unsafe {
            wb_encoder_encode(
                self.handle.as_ptr(),
                rgba.as_ptr(),
                sequence as i64,
                i32::from(force_idr),
                &mut output,
                &mut pts,
                &mut key,
            )
        };
        Self::packet(sequence, force_idr, output, pts, key, result)
    }
    pub fn encode_gpu(
        &mut self,
        frame: &mut GpuFrame,
        sequence: u64,
        force_idr: bool,
    ) -> Result<(Vec<u8>, bool), String> {
        let mut output = std::ptr::null();
        let mut pts = 0;
        let mut key = 0;
        let result = unsafe {
            wb_encoder_encode_gpu(
                self.handle.as_ptr(),
                frame.av_frame(),
                sequence as i64,
                i32::from(force_idr),
                &mut output,
                &mut pts,
                &mut key,
            )
        };
        Self::packet(sequence, force_idr, output, pts, key, result)
    }
    fn packet(
        sequence: u64,
        force_idr: bool,
        output: *const u8,
        pts: i64,
        key: c_int,
        result: c_int,
    ) -> Result<(Vec<u8>, bool), String> {
        if result <= 0 || pts != sequence as i64 || (force_idr && key == 0) {
            return Err(format!(
                "NVENC output mismatch ({result}, pts {pts}, expected {sequence})"
            ));
        }
        Ok((
            unsafe { std::slice::from_raw_parts(output, result as usize) }.to_vec(),
            key != 0,
        ))
    }
}
impl Drop for Encoder {
    fn drop(&mut self) {
        unsafe {
            wb_encoder_free(self.handle.as_ptr());
        }
    }
}

/// A diagnostic only: confirms actual P010 values survive a Main10 encode/decode.
/// It does not imply the compositor or Windows presentation path is HDR-ready.
pub fn probe_10bit(codec: Codec) -> Result<(u32, u32, u32), String> {
    let mut depth = 0;
    let mut distinct = 0;
    let mut peak = 0;
    let mut error = [0 as c_char; 512];
    let status = unsafe {
        wb_hdr_probe_10bit(
            codec as c_int,
            &mut depth,
            &mut distinct,
            &mut peak,
            error.as_mut_ptr(),
            error.len(),
        )
    };
    if status < 0 {
        Err(unsafe { CStr::from_ptr(error.as_ptr()) }
            .to_string_lossy()
            .into_owned())
    } else {
        Ok((depth as u32, distinct as u32, peak as u32))
    }
}
