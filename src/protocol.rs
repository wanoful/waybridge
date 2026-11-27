//! Versioned, bounded framing shared by control messages and lossless RGBA PNG frames.
use std::io::{self, Read, Write};

pub const VERSION: u32 = 3;
pub const MAX_PACKET: usize = 72 * 1024 * 1024;
pub const JSON: u8 = 0;
pub const FRAME: u8 = 1;
pub const VIDEO: u8 = 2;
pub const AUDIO: u8 = 3;
pub const HEVC_VIDEO: u8 = 4;
pub const AV1_VIDEO: u8 = 5;
// 6 is reserved for authenticated UDP repair containers.
pub const HEVC_HDR_VIDEO: u8 = 7;
pub const AV1_HDR_VIDEO: u8 = 8;

pub fn read_packet(reader: &mut impl Read) -> io::Result<(u8, Vec<u8>)> {
    let mut header = [0; 4];
    reader.read_exact(&mut header)?;
    let size = u32::from_be_bytes(header) as usize;
    if size == 0 || size > MAX_PACKET {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid packet length",
        ));
    }
    let mut body = vec![0; size];
    reader.read_exact(&mut body)?;
    Ok((body[0], body[1..].to_vec()))
}

pub fn write_packet(writer: &mut impl Write, kind: u8, body: &[u8]) -> io::Result<()> {
    if body.len() >= MAX_PACKET {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "packet too large",
        ));
    }
    writer.write_all(&((body.len() + 1) as u32).to_be_bytes())?;
    writer.write_all(&[kind])?;
    writer.write_all(body)
}

pub fn frame(
    window: u32,
    width: u32,
    height: u32,
    sequence: u64,
    geometry: [i32; 4],
    png: &[u8],
) -> Vec<u8> {
    let mut out = Vec::with_capacity(36 + png.len());
    out.extend(window.to_be_bytes());
    out.extend(width.to_be_bytes());
    out.extend(height.to_be_bytes());
    out.extend(sequence.to_be_bytes());
    for n in geometry {
        out.extend(n.to_be_bytes());
    }
    out.extend(png);
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn fragmented_stream_and_multiple_packets() {
        let mut wire = Vec::new();
        write_packet(&mut wire, JSON, b"hello").unwrap();
        write_packet(&mut wire, FRAME, b"frame").unwrap();
        let mut cursor = io::Cursor::new(wire);
        assert_eq!(read_packet(&mut cursor).unwrap(), (JSON, b"hello".to_vec()));
        assert_eq!(
            read_packet(&mut cursor).unwrap(),
            (FRAME, b"frame".to_vec())
        );
        assert!(read_packet(&mut cursor).is_err());
    }
    #[test]
    fn reject_untrusted_lengths_before_allocating() {
        for size in [0, (MAX_PACKET + 1) as u32, u32::MAX] {
            assert_eq!(
                read_packet(&mut &size.to_be_bytes()[..])
                    .unwrap_err()
                    .kind(),
                io::ErrorKind::InvalidData
            );
        }
        assert!(read_packet(&mut &[0, 0, 0, 8, 0, 1][..]).is_err());
    }
}
