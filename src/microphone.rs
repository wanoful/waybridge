//! A session Pulse/PipeWire source fed only by the authenticated viewer.
use crate::protocol;
use anyhow::{bail, Context, Result};
use std::{
    fs::{File, OpenOptions},
    io::{self, Read, Write},
    net::{Shutdown, TcpStream},
    os::{
        fd::AsRawFd,
        unix::fs::{FileTypeExt, OpenOptionsExt},
    },
    path::PathBuf,
    process::Command,
    sync::{Arc, Mutex, Weak},
    time::{Duration, Instant},
};

const FRAMES: usize = 480;
const PCM: usize = FRAMES * 2;
const HEADER: usize = 28;
const PACKET: usize = 1 + HEADER + PCM;
const AUDIO: u8 = 3;

pub struct Source {
    name: String,
    module: u32,
    pipe: Mutex<Option<File>>,
    path: PathBuf,
    _directory: tempfile::TempDir,
}

impl Source {
    pub fn new() -> Result<Arc<Self>> {
        let directory = tempfile::Builder::new()
            .prefix("waybridge-microphone-")
            .tempdir()?;
        let suffix = directory
            .path()
            .file_name()
            .and_then(|name| name.to_str())
            .context("invalid microphone runtime directory")?;
        let name = format!("{suffix}-{}", std::process::id());
        let path = directory.path().join("samples.fifo");
        let argument = path
            .to_str()
            .context("invalid microphone FIFO path")?
            .replace('\\', "\\\\")
            .replace('"', "\\\"");
        let output = Command::new("pactl")
            .args([
                "load-module", "module-pipe-source", &format!("file=\"{argument}\""),
                &format!("source_name={name}"), "format=s16le", "rate=48000",
                "channels=1", "channel_map=mono",
                "source_properties=\"device.description='Waybridge Microphone' priority.session=1\"",
            ])
            .output()
            .context("loading the PulseAudio microphone source")?;
        if !output.status.success() {
            bail!(
                "creating microphone source: {}",
                String::from_utf8_lossy(&output.stderr)
            );
        }
        let source = Self {
            name,
            module: String::from_utf8_lossy(&output.stdout).trim().parse()?,
            pipe: Mutex::new(None),
            path,
            _directory: directory,
        };
        let pipe = OpenOptions::new()
            .write(true)
            .custom_flags(libc::O_NONBLOCK | libc::O_NOFOLLOW)
            .open(&source.path)
            .context("opening the microphone FIFO")?;
        if !pipe.metadata()?.file_type().is_fifo() {
            bail!("microphone path is not a FIFO");
        }
        // Keep at most one small Linux pipe page. Failure leaves the kernel
        // bound intact, and push() drops backlog before it can grow.
        unsafe { libc::fcntl(pipe.as_raw_fd(), libc::F_SETPIPE_SZ, 4096) };
        *source.pipe.lock().unwrap() = Some(pipe);
        tracing::info!(source = %source.name, "session microphone source available");
        Ok(Arc::new(source))
    }

    pub fn name(&self) -> &str {
        &self.name
    }

    fn clear(&self) {
        if let Ok(mut pipe) = OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_NONBLOCK | libc::O_NOFOLLOW)
            .open(&self.path)
        {
            let mut scratch = [0; 4096];
            for _ in 0..16 {
                if !matches!(pipe.read(&mut scratch), Ok(1..)) {
                    break;
                }
            }
        }
    }

    fn push(&self, pcm: &[u8]) -> io::Result<()> {
        let mut guard = self.pipe.lock().unwrap();
        let pipe = guard
            .as_mut()
            .ok_or_else(|| io::Error::other("microphone is closed"))?;
        let mut waiting: libc::c_int = 0;
        if unsafe { libc::ioctl(pipe.as_raw_fd(), libc::FIONREAD, &mut waiting) } == 0
            && waiting >= (3 * PCM) as libc::c_int
        {
            self.clear();
        }
        match pipe.write_all(pcm) {
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => Ok(()),
            result => result,
        }
    }
}

impl Drop for Source {
    fn drop(&mut self) {
        // A restarted Pulse server may reuse module IDs. Verify path ownership
        // before unloading, as we do for the session playback sink.
        let owns_module = crate::audio::pulse_list("modules").is_some_and(|modules| {
            modules.iter().any(|module| {
                module.first().and_then(|id| id.parse::<u32>().ok()) == Some(self.module)
                    && module
                        .get(1)
                        .is_some_and(|kind| kind == "module-pipe-source")
                    && module.get(2).is_some_and(|args| {
                        args.split_ascii_whitespace()
                            .any(|part| part == format!("source_name={}", self.name))
                    })
            })
        });
        if owns_module {
            let _ = Command::new("pactl")
                .args(["unload-module", &self.module.to_string()])
                .output();
        }
    }
}

#[derive(Default)]
struct Permission {
    request: u64,
    granted: bool,
    socket: Option<TcpStream>,
}

pub struct Input {
    source: Weak<Source>,
    state: Mutex<Permission>,
}

impl Input {
    pub fn new(source: Weak<Source>) -> Self {
        Self {
            source,
            state: Mutex::new(Permission::default()),
        }
    }

    fn close_socket(&self, state: &mut Permission) {
        if let Some(socket) = state.socket.take() {
            let _ = socket.shutdown(Shutdown::Both);
            if let Some(source) = self.source.upgrade() {
                source.clear();
            }
        }
    }

    pub fn revoke(&self) {
        let mut state = self.state.lock().unwrap();
        state.granted = false;
        self.close_socket(&mut state);
    }

    pub fn configure(&self, request: u64, enabled: impl FnOnce() -> bool) -> Option<bool> {
        let mut state = self.state.lock().unwrap();
        if request <= state.request {
            return None;
        }
        self.close_socket(&mut state);
        state.request = request;
        state.granted = enabled() && self.source.strong_count() > 0;
        Some(state.granted)
    }

    pub fn serve(&self, mut socket: TcpStream, request: u64) -> io::Result<()> {
        {
            let mut state = self.state.lock().unwrap();
            if !state.granted || request != state.request || state.socket.is_some() {
                return Err(io::Error::new(
                    io::ErrorKind::PermissionDenied,
                    "microphone lease rejected",
                ));
            }
            state.socket = Some(socket.try_clone()?);
        }
        let result = self.receive(&mut socket, request);
        let mut state = self.state.lock().unwrap();
        if request == state.request {
            self.close_socket(&mut state);
        }
        result
    }

    fn receive(&self, socket: &mut TcpStream, request: u64) -> io::Result<()> {
        protocol::write_packet(socket, protocol::JSON, br#"{"type":"microphone_ready"}"#)?;
        let mut packet = [0; PACKET + 4];
        let mut last = 0;
        let burst = Duration::from_millis(80);
        // The hardware capture clock and the compositor's clock can differ.
        let interval = Duration::from_micros(9_500);
        let start = Instant::now();
        let mut budget = start.checked_sub(burst).unwrap_or(start);
        loop {
            socket.read_exact(&mut packet[..4])?;
            if u32::from_be_bytes(packet[..4].try_into().unwrap()) as usize != PACKET {
                return Err(invalid_packet());
            }
            socket.read_exact(&mut packet[4..])?;
            let (sequence, pcm) = decode(&packet[4..]).ok_or_else(invalid_packet)?;
            if sequence <= last {
                return Err(invalid_packet());
            }
            last = sequence;
            let now = Instant::now();
            budget = budget.max(now.checked_sub(burst).unwrap_or(now)) + interval;
            if budget > now + interval {
                return Err(invalid_packet());
            }
            {
                let state = self.state.lock().unwrap();
                if !state.granted || request != state.request || state.socket.is_none() {
                    return Ok(());
                }
                let source = self
                    .source
                    .upgrade()
                    .ok_or_else(|| io::Error::other("microphone source closed"))?;
                source.push(pcm)?;
            }
            protocol::write_packet(socket, AUDIO, &sequence.to_be_bytes())?;
        }
    }
}

fn invalid_packet() -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, "invalid microphone packet")
}

fn decode(packet: &[u8]) -> Option<(u64, &[u8])> {
    if packet.len() != PACKET
        || packet[0] != AUDIO
        || packet[1..5] != 48000u32.to_be_bytes()
        || packet[5..7] != 1u16.to_be_bytes()
        || packet[7..9] != 16u16.to_be_bytes()
        || packet[9..13] != (FRAMES as u32).to_be_bytes()
    {
        return None;
    }
    Some((
        u64::from_be_bytes(packet[13..21].try_into().ok()?),
        &packet[1 + HEADER..],
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mono_pcm_requires_exact_wire_format() {
        let mut packet = [0; PACKET];
        packet[0] = AUDIO;
        packet[1..5].copy_from_slice(&48000u32.to_be_bytes());
        packet[5..7].copy_from_slice(&1u16.to_be_bytes());
        packet[7..9].copy_from_slice(&16u16.to_be_bytes());
        packet[9..13].copy_from_slice(&(FRAMES as u32).to_be_bytes());
        packet[13..21].copy_from_slice(&12u64.to_be_bytes());
        assert_eq!(decode(&packet), Some((12, &[0; PCM][..])));
        assert!(decode(&packet[..PACKET - 2]).is_none());
        packet[6] = 2;
        assert!(decode(&packet).is_none());
    }
}
