use std::{
    ffi::{c_char, c_int, c_void, CStr},
    ptr::NonNull,
};

unsafe extern "C" {
    fn wb_encoder_new(
        width: c_int,
        height: c_int,
        fps: c_int,
        bitrate: c_int,
        error: *mut c_char,
        size: usize,
    ) -> *mut c_void;
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

pub struct Encoder {
    handle: NonNull<c_void>,
    bytes: usize,
}

impl Encoder {
    pub fn new(width: u32, height: u32, fps: u32, bitrate: u32) -> Result<Self, String> {
        if !(48..=4096).contains(&width)
            || !(48..=2304).contains(&height)
            || !(1..=120).contains(&fps)
            || !(1000..=200000).contains(&bitrate)
        {
            return Err("Invalid NVENC dimensions or rate".into());
        }
        let mut error = [0 as c_char; 256];
        let pointer = unsafe {
            wb_encoder_new(
                width as c_int,
                height as c_int,
                fps as c_int,
                bitrate as c_int,
                error.as_mut_ptr(),
                error.len(),
            )
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
