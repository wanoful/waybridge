use super::{BLOCK_FRAMES, CHANNELS, HEADER_BYTES, PCM_BYTES, SAMPLE_RATE};
use std::{ffi::c_int, io, ptr::NonNull};

const APPLICATION_RESTRICTED_LOWDELAY: c_int = 2051;
const SET_BITRATE: c_int = 4002;
const SET_VBR: c_int = 4006;
const BITRATE: c_int = 128_000;
const MAX_PACKET_BYTES: usize = 1275;

#[repr(C)]
struct OpusEncoder {
    _opaque: [u8; 0],
}

#[link(name = "opus")]
extern "C" {
    fn opus_encoder_create(
        rate: c_int,
        channels: c_int,
        application: c_int,
        error: *mut c_int,
    ) -> *mut OpusEncoder;
    fn opus_encoder_destroy(encoder: *mut OpusEncoder);
    fn opus_encoder_ctl(encoder: *mut OpusEncoder, request: c_int, ...) -> c_int;
    fn opus_encode(
        encoder: *mut OpusEncoder,
        pcm: *const i16,
        frame_size: c_int,
        data: *mut u8,
        max_data_bytes: c_int,
    ) -> c_int;
}

pub(crate) struct Encoder(NonNull<OpusEncoder>);

impl Encoder {
    pub(crate) fn new() -> io::Result<Self> {
        let mut status = 0;
        let pointer = unsafe {
            opus_encoder_create(
                SAMPLE_RATE as c_int,
                c_int::from(CHANNELS),
                APPLICATION_RESTRICTED_LOWDELAY,
                &mut status,
            )
        };
        let encoder = Self(NonNull::new(pointer).ok_or_else(|| {
            io::Error::other(format!("creating Opus audio encoder failed ({status})"))
        })?);
        for (request, value) in [(SET_BITRATE, BITRATE), (SET_VBR, 0)] {
            let status = unsafe { opus_encoder_ctl(encoder.0.as_ptr(), request, value) };
            if status != 0 {
                return Err(io::Error::other(format!(
                    "configuring Opus audio encoder failed ({status})"
                )));
            }
        }
        Ok(encoder)
    }

    pub(crate) fn encode(&mut self, frame: &mut Vec<u8>) -> io::Result<()> {
        if frame.len() != HEADER_BYTES + PCM_BYTES {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "invalid PCM audio block",
            ));
        }
        let mut samples = [0i16; PCM_BYTES / size_of::<i16>()];
        for (sample, bytes) in samples
            .iter_mut()
            .zip(frame[HEADER_BYTES..].chunks_exact(2))
        {
            *sample = i16::from_le_bytes([bytes[0], bytes[1]]);
        }
        let mut packet = [0u8; MAX_PACKET_BYTES];
        let length = unsafe {
            opus_encode(
                self.0.as_ptr(),
                samples.as_ptr(),
                BLOCK_FRAMES as c_int,
                packet.as_mut_ptr(),
                packet.len() as c_int,
            )
        };
        if length <= 0 {
            return Err(io::Error::other(format!(
                "Opus audio encoding failed ({length})"
            )));
        }
        frame.truncate(HEADER_BYTES);
        frame.extend_from_slice(&packet[..length as usize]);
        Ok(())
    }
}

impl Drop for Encoder {
    fn drop(&mut self) {
        unsafe { opus_encoder_destroy(self.0.as_ptr()) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn restricted_lowdelay_keeps_lookahead_and_packet_size_bounded() {
        const GET_LOOKAHEAD: c_int = 4027;
        let mut encoder = Encoder::new().unwrap();
        let mut lookahead: c_int = 0;
        let result = unsafe {
            opus_encoder_ctl(
                encoder.0.as_ptr(),
                GET_LOOKAHEAD,
                &mut lookahead as *mut c_int,
            )
        };
        assert_eq!(result, 0);
        assert_eq!(lookahead, 120);
        let mut frame = vec![0; HEADER_BYTES + PCM_BYTES];
        encoder.encode(&mut frame).unwrap();
        assert_eq!(frame.len(), HEADER_BYTES + 160);
    }
}
