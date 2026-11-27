use std::{ffi::c_void, io, ptr::NonNull, sync::Once};

pub const RS_GROUP: usize = 16;
pub const RS_PARITY: usize = 2;
const CHUNK: usize = 1024;

extern "C" {
    fn wb_rs_initialize();
    fn wb_rs_create() -> *mut c_void;
    fn wb_rs_destroy(codec: *mut c_void);
    fn wb_rs_encode(codec: *mut c_void, data: *mut u8, parity: *mut u8) -> i32;
}

pub struct ReedSolomon(NonNull<c_void>);

impl ReedSolomon {
    pub fn new() -> io::Result<Self> {
        static INIT: Once = Once::new();
        INIT.call_once(|| unsafe { wb_rs_initialize() });
        NonNull::new(unsafe { wb_rs_create() })
            .map(Self)
            .ok_or_else(|| io::Error::other("allocating Reed–Solomon codec"))
    }

    pub fn encode<'a>(
        &mut self,
        blocks: impl IntoIterator<Item = &'a [u8]>,
    ) -> io::Result<[[u8; CHUNK]; RS_PARITY]> {
        let mut data = [[0; CHUNK]; RS_GROUP];
        for (index, block) in blocks.into_iter().enumerate() {
            if index >= RS_GROUP || block.len() > CHUNK {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidInput,
                    "invalid FEC group",
                ));
            }
            data[index][..block.len()].copy_from_slice(block);
        }
        let mut parity = [[0; CHUNK]; RS_PARITY];
        let result = unsafe {
            wb_rs_encode(
                self.0.as_ptr(),
                data.as_mut_ptr().cast(),
                parity.as_mut_ptr().cast(),
            )
        };
        if result == 0 {
            Ok(parity)
        } else {
            Err(io::Error::other("encoding Reed–Solomon parity"))
        }
    }
}

impl Drop for ReedSolomon {
    fn drop(&mut self) {
        unsafe { wb_rs_destroy(self.0.as_ptr()) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ptr::null;

    extern "C" {
        fn wb_rs_recover(
            codec: *mut c_void,
            body: *mut u8,
            total: usize,
            seen: *mut u8,
            start: usize,
            parity0: *const u8,
            parity1: *const u8,
        ) -> usize;
    }

    #[test]
    fn recovers_two_losses_in_a_partial_group_and_one_from_either_parity() {
        let original: Vec<_> = (0..(RS_GROUP - 1) * CHUNK + 97)
            .map(|i| ((i * 61) ^ (i >> 10)) as u8)
            .collect();
        let mut codec = ReedSolomon::new().unwrap();
        let [first, second] = codec.encode(original.chunks(CHUNK)).unwrap();
        for (missing, p0, p1) in [
            (&[1, RS_GROUP - 1][..], first.as_ptr(), second.as_ptr()),
            (&[3][..], null(), second.as_ptr()),
            (&[RS_GROUP - 1][..], first.as_ptr(), null()),
        ] {
            let mut body = original.clone();
            let mut seen = [1; RS_GROUP];
            for &index in missing {
                seen[index] = 0;
                let end = ((index + 1) * CHUNK).min(body.len());
                body[index * CHUNK..end].fill(0);
            }
            let repaired = unsafe {
                wb_rs_recover(
                    codec.0.as_ptr(),
                    body.as_mut_ptr(),
                    body.len(),
                    seen.as_mut_ptr(),
                    0,
                    p0,
                    p1,
                )
            };
            assert_eq!(repaired, missing.len());
            assert_eq!(seen, [1; RS_GROUP]);
            assert_eq!(body, original);
        }
    }
}
