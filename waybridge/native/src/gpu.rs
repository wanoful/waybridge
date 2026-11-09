//! Optional same-GPU GL -> CUDA input. Only completed independent CUDA frames
//! cross threads; GL registration and source lifetime stay on the compositor.
use std::{
    ffi::{c_char, c_int, c_void, CStr},
    ptr::NonNull,
    rc::Rc,
};
unsafe extern "C" {
    fn wb_gpu_device_new(error: *mut c_char, size: usize) -> *mut c_void;
    fn wb_gpu_device_free(device: *mut c_void);
    fn wb_gpu_source_new_mode(
        device: *mut c_void,
        renderbuffer: u32,
        width: c_int,
        height: c_int,
        hdr: c_int,
        error: *mut c_char,
        size: usize,
    ) -> *mut c_void;
    fn wb_gpu_source_free(source: *mut c_void);
    fn wb_gpu_capture(
        source: *mut c_void,
        flip: c_int,
        error: *mut c_char,
        size: usize,
    ) -> *mut c_void;
    fn wb_gpu_frame_free(frame: *mut c_void);
    fn wb_gpu_frame_avframe(frame: *const c_void) -> *mut c_void;
    fn wb_gpu_frame_alpha(frame: *const c_void, length: *mut usize) -> *const u8;
    fn wb_gpu_frame_bytes(frame: *const c_void) -> usize;
    fn wb_gpu_frame_device(frame: *const c_void) -> usize;
    fn wb_gpu_frame_download(frame: *const c_void, rgba: *mut u8, length: usize) -> c_int;
}
fn message(error: &[c_char]) -> String {
    unsafe { CStr::from_ptr(error.as_ptr()) }
        .to_string_lossy()
        .into_owned()
}
pub struct GpuDevice {
    handle: NonNull<c_void>,
}
impl GpuDevice {
    /// Must be called with the renderer's GL context current. CUDA's own GL
    /// device query selects and verifies that GPU, rather than assuming ordinal 0.
    pub fn new() -> Result<Rc<Self>, String> {
        let mut error = [0 as c_char; 512];
        let handle = unsafe { wb_gpu_device_new(error.as_mut_ptr(), error.len()) };
        NonNull::new(handle)
            .map(|handle| Rc::new(Self { handle }))
            .ok_or_else(|| message(&error))
    }
}
impl Drop for GpuDevice {
    fn drop(&mut self) {
        unsafe {
            wb_gpu_device_free(self.handle.as_ptr());
        }
    }
}
pub struct GpuSource {
    handle: NonNull<c_void>,
    _device: Rc<GpuDevice>,
    width: u32,
    height: u32,
    hdr: bool,
}
impl GpuSource {
    /// The GL renderbuffer must remain alive and unmapped until this source is
    /// dropped. Drop it before resizing/replacing/deleting the renderbuffer.
    pub fn new(
        device: Rc<GpuDevice>,
        renderbuffer: u32,
        width: u32,
        height: u32,
    ) -> Result<Self, String> {
        Self::new_mode(device, renderbuffer, width, height, false)
    }
    /// HDR registers RGBA16F and produces limited P010/PQ plus a GPU SDR fallback.
    pub fn new_mode(
        device: Rc<GpuDevice>,
        renderbuffer: u32,
        width: u32,
        height: u32,
        hdr: bool,
    ) -> Result<Self, String> {
        let mut error = [0 as c_char; 512];
        let handle = unsafe {
            wb_gpu_source_new_mode(
                device.handle.as_ptr(),
                renderbuffer,
                width as c_int,
                height as c_int,
                i32::from(hdr),
                error.as_mut_ptr(),
                error.len(),
            )
        };
        NonNull::new(handle)
            .map(|handle| Self {
                handle,
                _device: device,
                width,
                height,
                hdr,
            })
            .ok_or_else(|| message(&error))
    }
    /// GL must not touch the resource while mapped. The native implementation
    /// completes the color/alpha kernel and unmaps it before returning.
    pub fn capture(&mut self, flip: bool) -> Result<GpuFrame, String> {
        let mut error = [0 as c_char; 512];
        let handle = unsafe {
            wb_gpu_capture(
                self.handle.as_ptr(),
                i32::from(flip),
                error.as_mut_ptr(),
                error.len(),
            )
        };
        NonNull::new(handle)
            .map(|handle| GpuFrame {
                handle,
                width: self.width,
                height: self.height,
                hdr: self.hdr,
            })
            .ok_or_else(|| message(&error))
    }
}
impl Drop for GpuSource {
    fn drop(&mut self) {
        unsafe {
            wb_gpu_source_free(self.handle.as_ptr());
        }
    }
}
pub struct GpuFrame {
    handle: NonNull<c_void>,
    width: u32,
    height: u32,
    hdr: bool,
}
// The CUDA kernel is complete and GL resource unmapped before construction.
// AVFrame/AVBufferRef own independent GPU storage and a CUDA device reference;
// FFmpeg pushes that context on whichever thread releases/encodes the frame.
unsafe impl Send for GpuFrame {}
impl GpuFrame {
    pub fn hdr(&self) -> bool {
        self.hdr
    }
    pub(crate) fn av_frame(&self) -> *mut c_void {
        unsafe { wb_gpu_frame_avframe(self.handle.as_ptr()) }
    }
    /// Empty means opaque. SDR is L8; HDR contains native-endian raw FP16 bits
    /// (two bytes per pixel), preserved verbatim by the alpha16 PNG transport.
    pub fn alpha(&self) -> &[u8] {
        let mut length = 0;
        let pointer = unsafe { wb_gpu_frame_alpha(self.handle.as_ptr(), &mut length) };
        if length == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(pointer, length) }
        }
    }
    pub fn memory_bytes(&self) -> usize {
        unsafe { wb_gpu_frame_bytes(self.handle.as_ptr()) }
    }
    pub fn device_key(&self) -> usize {
        unsafe { wb_gpu_frame_device(self.handle.as_ptr()) }
    }
    /// Explicit emergency CPU fallback; never used by successful GPU encoding.
    /// Pixels are straight RGBA8. HDR downloads its independently generated
    /// SDR tone-map, including explicitly quantized eight-bit fallback alpha.
    pub fn download(&self) -> Result<Vec<u8>, String> {
        let mut rgba = vec![0; self.width as usize * self.height as usize * 4];
        let status =
            unsafe { wb_gpu_frame_download(self.handle.as_ptr(), rgba.as_mut_ptr(), rgba.len()) };
        if status < 0 {
            Err(format!("CUDA fallback readback failed: {status}"))
        } else {
            Ok(rgba)
        }
    }
}
impl Drop for GpuFrame {
    fn drop(&mut self) {
        unsafe {
            wb_gpu_frame_free(self.handle.as_ptr());
        }
    }
}
