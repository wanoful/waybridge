//! Standard per-window icon inputs. Copy SHM at set_icon, commit it with the
//! toplevel, and decode/resize or query X11 only on the bounded icon worker.
use crate::app_icon::{PixelFormat, RawIcon, Source};
use smithay::{
    backend::renderer::buffer_dimensions,
    reexports::{
        wayland_server::protocol::{wl_shm, wl_surface::WlSurface},
        x11rb,
    },
    wayland::{
        compositor::with_states, shm::with_buffer_contents,
        xdg_toplevel_icon::ToplevelIconCachedState,
    },
};
use std::{
    io,
    os::fd::{AsRawFd, FromRawFd, OwnedFd},
    os::unix::net::UnixStream,
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

pub const MAX_EDGE: u32 = 512;
#[derive(Default)]
struct SurfaceIcon {
    pending: Option<Source>,
    current: Source,
}

pub fn pending(surface: &WlSurface) {
    with_states(surface, |states| {
        let mut cached = states.cached_state.get::<ToplevelIconCachedState>();
        let icon = cached.pending();
        let name = icon
            .icon_name()
            .filter(|name| name.len() <= 255)
            .map(str::to_owned);
        let pixels = icon
            .buffers()
            .iter()
            .take(64)
            .filter_map(|(buffer, scale)| {
                let dimensions = buffer_dimensions(buffer)?;
                let dimensions = (dimensions.w, dimensions.h);
                (dimensions.0 > 0
                    && dimensions.0 <= MAX_EDGE as i32
                    && dimensions.1 == dimensions.0
                    && *scale > 0)
                    .then_some((buffer, dimensions.0 as u32))
            })
            .min_by_key(|(_, edge)| edge.abs_diff(crate::app_icon::EDGE))
            .and_then(|(buffer, _)| snapshot(buffer));
        // Our double-buffered snapshot owns the data. Do not retain protocol
        // objects/SHM pools after the application destroys its icon and buffers.
        *icon = ToplevelIconCachedState::default();
        drop(cached);
        states
            .data_map
            .insert_if_missing_threadsafe(|| Mutex::new(SurfaceIcon::default()));
        states
            .data_map
            .get::<Mutex<SurfaceIcon>>()
            .unwrap()
            .lock()
            .unwrap()
            .pending = Some(Source {
            name,
            pixels: pixels.map(Arc::new),
            ..Source::default()
        });
    });
}
fn snapshot(
    buffer: &smithay::reexports::wayland_server::protocol::wl_buffer::WlBuffer,
) -> Option<RawIcon> {
    with_buffer_contents(buffer, |ptr, length, data| {
        let edge = u32::try_from(data.width).ok()?;
        if edge == 0
            || edge > MAX_EDGE
            || data.height != data.width
            || data.offset < 0
            || data.stride < data.width * 4
            || !matches!(
                data.format,
                wl_shm::Format::Argb8888 | wl_shm::Format::Xrgb8888
            )
        {
            return None;
        }
        let row = edge as usize * 4;
        let end = (data.offset as usize)
            .checked_add((edge as usize - 1).checked_mul(data.stride as usize)?)?
            .checked_add(row)?;
        if end > length {
            return None;
        }
        let mut bytes = vec![0; row * edge as usize];
        for y in 0..edge as usize {
            // The client owns shared memory; do not form Rust references into it.
            unsafe {
                std::ptr::copy_nonoverlapping(
                    ptr.add(data.offset as usize + y * data.stride as usize),
                    bytes.as_mut_ptr().add(y * row),
                    row,
                );
            }
        }
        Some(RawIcon {
            width: edge,
            height: edge,
            bytes,
            format: if data.format == wl_shm::Format::Argb8888 {
                PixelFormat::Argb8888
            } else {
                PixelFormat::Xrgb8888
            },
        })
    })
    .ok()?
}
pub fn commit(surface: &WlSurface) -> bool {
    with_states(surface, |states| {
        let Some(icon) = states.data_map.get::<Mutex<SurfaceIcon>>() else {
            return false;
        };
        let mut icon = icon.lock().unwrap();
        if let Some(pending) = icon.pending.take() {
            icon.current = pending;
            true
        } else {
            false
        }
    })
}
pub fn source(surface: &WlSurface) -> Source {
    with_states(surface, |states| {
        states
            .data_map
            .get::<Mutex<SurfaceIcon>>()
            .map(|icon| icon.lock().unwrap().current.clone())
            .unwrap_or_default()
    })
}

// Bound the entire private-X11 round trip, including setup, even when a client
// grabs/stalls the X server. No property requests run on Wayland dispatch.
struct TimedStream {
    stream: x11rb::rust_connection::DefaultStream,
    deadline: Instant,
}
impl x11rb::rust_connection::Stream for TimedStream {
    fn poll(&self, mode: x11rb::rust_connection::PollMode) -> io::Result<()> {
        loop {
            let remaining = self.deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Err(io::ErrorKind::TimedOut.into());
            }
            let events = (if mode.readable() { libc::POLLIN } else { 0 })
                | (if mode.writable() { libc::POLLOUT } else { 0 });
            let mut fd = libc::pollfd {
                fd: self.stream.as_raw_fd(),
                events,
                revents: 0,
            };
            let ready =
                unsafe { libc::poll(&mut fd, 1, remaining.as_millis().clamp(1, 500) as i32) };
            if ready > 0 {
                return Ok(());
            }
            if ready == 0 {
                return Err(io::ErrorKind::TimedOut.into());
            }
            let error = io::Error::last_os_error();
            if error.kind() != io::ErrorKind::Interrupted {
                return Err(error);
            }
        }
    }
    fn read(
        &self,
        bytes: &mut [u8],
        fds: &mut Vec<x11rb::utils::RawFdContainer>,
    ) -> io::Result<usize> {
        self.stream.read(bytes, fds)
    }
    fn write(
        &self,
        bytes: &[u8],
        fds: &mut Vec<x11rb::utils::RawFdContainer>,
    ) -> io::Result<usize> {
        self.stream.write(bytes, fds)
    }
}
fn private_x11_stream(display: u32) -> io::Result<UnixStream> {
    let raw = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_STREAM | libc::SOCK_CLOEXEC, 0) };
    if raw < 0 {
        return Err(io::Error::last_os_error());
    }
    let fd = unsafe { OwnedFd::from_raw_fd(raw) };
    // SO_SNDTIMEO also bounds a blocking Unix connect when the accept backlog is
    // full. RustConnection's setup/property waits are separately deadline-bound.
    let timeout = libc::timeval {
        tv_sec: 0,
        tv_usec: 500_000,
    };
    if unsafe {
        libc::setsockopt(
            fd.as_raw_fd(),
            libc::SOL_SOCKET,
            libc::SO_SNDTIMEO,
            (&timeout as *const libc::timeval).cast(),
            std::mem::size_of_val(&timeout) as libc::socklen_t,
        )
    } != 0
    {
        return Err(io::Error::last_os_error());
    }
    let mut address: libc::sockaddr_un = unsafe { std::mem::zeroed() };
    address.sun_family = libc::AF_UNIX as libc::sa_family_t;
    let path = format!("/tmp/.X11-unix/X{display}");
    for (destination, byte) in address.sun_path.iter_mut().zip(path.bytes()) {
        *destination = byte as libc::c_char;
    }
    if unsafe {
        libc::connect(
            fd.as_raw_fd(),
            (&address as *const libc::sockaddr_un).cast(),
            std::mem::size_of_val(&address) as libc::socklen_t,
        )
    } != 0
    {
        return Err(io::Error::last_os_error());
    }
    Ok(UnixStream::from(fd))
}
pub fn x11(display: u32, window: u32) -> Option<RawIcon> {
    use x11rb::protocol::xproto::{AtomEnum, ConnectionExt};
    let stream = private_x11_stream(display).ok()?;
    let stream = TimedStream {
        stream: x11rb::rust_connection::DefaultStream::from_unix_stream(stream)
            .ok()?
            .0,
        deadline: Instant::now() + Duration::from_millis(500),
    };
    let connection = x11rb::rust_connection::RustConnection::connect_to_stream(stream, 0).ok()?;
    let atom = connection
        .intern_atom(false, b"_NET_WM_ICON")
        .ok()?
        .reply()
        .ok()?
        .atom;
    let property = connection
        .get_property(false, window, atom, AtomEnum::CARDINAL, 0, 512 * 1024)
        .ok()?
        .reply()
        .ok()?;
    if property.bytes_after != 0 || property.format != 32 {
        return None;
    }
    let words = property.value32()?.collect::<Vec<_>>();
    net_wm_icon(&words)
}
fn net_wm_icon(mut words: &[u32]) -> Option<RawIcon> {
    let mut best: Option<(u32, u32, &[u32])> = None;
    while !words.is_empty() {
        if words.len() < 2 {
            return None;
        }
        let (width, height) = (words[0], words[1]);
        let count = usize::try_from(u64::from(width) * u64::from(height)).ok()?;
        if width == 0 || height == 0 || count > words.len() - 2 {
            return None;
        }
        let pixels = &words[2..count + 2];
        if width <= MAX_EDGE
            && height <= MAX_EDGE
            && best.as_ref().is_none_or(|(w, h, _)| {
                width.abs_diff(crate::app_icon::EDGE) + height.abs_diff(crate::app_icon::EDGE)
                    < w.abs_diff(crate::app_icon::EDGE) + h.abs_diff(crate::app_icon::EDGE)
            })
        {
            best = Some((width, height, pixels));
        }
        words = &words[count + 2..];
    }
    let (width, height, pixels) = best?;
    let bytes = pixels
        .iter()
        .flat_map(|argb| {
            [
                (*argb >> 16) as u8,
                (*argb >> 8) as u8,
                *argb as u8,
                (*argb >> 24) as u8,
            ]
        })
        .collect();
    Some(RawIcon {
        width,
        height,
        bytes,
        format: PixelFormat::Rgba,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn stalled_x11_setup_times_out() {
        let (socket, peer) = UnixStream::pair().unwrap();
        let start = Instant::now();
        let stream = TimedStream {
            stream: x11rb::rust_connection::DefaultStream::from_unix_stream(socket)
                .unwrap()
                .0,
            deadline: start + Duration::from_millis(100),
        };
        // A connected server accepts the setup bytes but never replies. This
        // exercises actual socket I/O instead of treating a missing path as a timeout.
        match x11rb::rust_connection::RustConnection::connect_to_stream(stream, 0) {
            Err(x11rb::errors::ConnectError::IoError(error)) => {
                assert_eq!(error.kind(), io::ErrorKind::TimedOut)
            }
            Err(error) => panic!("unexpected X11 setup error: {error}"),
            Ok(_) => panic!("silent X11 server unexpectedly completed setup"),
        }
        assert!(start.elapsed() >= Duration::from_millis(80));
        assert!(start.elapsed() < Duration::from_secs(1));
        drop(peer);
    }
}
