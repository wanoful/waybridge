//! Opt-in private PulseAudio/PipeWire-Pulse sink. Never captures a host/default monitor.
use crate::network::Peer;
use anyhow::{bail, Context, Result};
use std::{
    io::Read,
    process::{Child, Command, Stdio},
    sync::{Arc, Mutex, Weak},
    thread::{self, JoinHandle},
    time::Instant,
};

struct Sink {
    name: String,
    module: u32,
}
impl Drop for Sink {
    fn drop(&mut self) {
        let query = |kind: &str| -> Option<Vec<Vec<String>>> {
            let output = Command::new("pactl")
                .args(["list", "short", kind])
                .output()
                .ok()?;
            if !output.status.success() {
                return None;
            }
            Some(
                String::from_utf8_lossy(&output.stdout)
                    .lines()
                    .map(|line| line.split('\t').map(str::to_string).collect())
                    .collect(),
            )
        };
        // A PulseAudio restart may recycle module IDs. Never unload somebody else's module.
        // The stable short listing includes indices even on pactl versions whose JSON omits them.
        let Some(modules) = query("modules") else {
            return;
        };
        let owned = modules.iter().any(|m| {
            m.first().and_then(|v| v.parse::<u32>().ok()) == Some(self.module)
                && m.get(1).is_some_and(|v| v == "module-null-sink")
                && m.get(2).is_some_and(|a| {
                    a.split_whitespace()
                        .any(|s| s == format!("sink_name={}", self.name))
                })
        });
        if !owned {
            return;
        }
        // Mute this sink's streams before unload so stream-rescue cannot expose audio on the host.
        if let (Some(sinks), Some(inputs)) = (query("sinks"), query("sink-inputs")) {
            let index = sinks
                .iter()
                .find(|s| s.get(1) == Some(&self.name))
                .and_then(|s| s.first());
            if let Some(index) = index {
                for input in &inputs {
                    if input.get(1) == Some(index) {
                        if let Some(id) = input.first().filter(|s| s.parse::<u32>().is_ok()) {
                            let _ = Command::new("pactl")
                                .args(["set-sink-input-mute", id, "1"])
                                .stdout(Stdio::null())
                                .stderr(Stdio::null())
                                .status();
                        }
                    }
                }
            }
        }
        let _ = Command::new("pactl")
            .args(["unload-module", &self.module.to_string()])
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status();
    }
}
pub struct Session {
    sink: Sink,
    peer: Arc<Mutex<Weak<Peer>>>,
    capture: Child,
    worker: Option<JoinHandle<()>>,
}
impl Session {
    pub fn new() -> Result<Self> {
        let name = format!("waybridge-{}", std::process::id());
        let loaded = Command::new("pactl")
            .args([
                "load-module",
                "module-null-sink",
                &format!("sink_name={name}"),
                "rate=48000",
                "channels=2",
                "sink_properties=device.description=Waybridge",
            ])
            .output()
            .context("pactl is required for --audio")?;
        if !loaded.status.success() {
            bail!(
                "creating private audio sink: {}",
                String::from_utf8_lossy(&loaded.stderr)
            );
        }
        let module = String::from_utf8_lossy(&loaded.stdout)
            .trim()
            .parse::<u32>()?;
        let sink = Sink { name, module };
        let mut capture = Command::new("parec")
            .args([
                "--raw",
                "--format=s16le",
                "--rate=48000",
                "--channels=2",
                "--latency-msec=10",
                "--process-time-msec=5",
                &format!("--device={}.monitor", sink.name),
            ])
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .context("parec is required for --audio")?;
        let mut stdout = capture.stdout.take().unwrap();
        let peer = Arc::new(Mutex::new(Weak::<Peer>::new()));
        let subscriber = peer.clone();
        let worker = thread::spawn(move || {
            let start = Instant::now();
            let mut sequence = 0u64;
            let mut pcm = [0u8; 480 * 2 * 2]; // 10 ms, stereo signed 16-bit little endian.
            while stdout.read_exact(&mut pcm).is_ok() {
                if crate::shutdown::requested() {
                    break;
                }
                sequence = sequence.wrapping_add(1);
                let current = subscriber.lock().unwrap().upgrade();
                if let Some(peer) = current {
                    let mut frame = Vec::with_capacity(28 + pcm.len());
                    frame.extend(48000u32.to_be_bytes());
                    frame.extend(2u16.to_be_bytes());
                    frame.extend(16u16.to_be_bytes());
                    frame.extend(480u32.to_be_bytes());
                    frame.extend(sequence.to_be_bytes());
                    frame.extend((start.elapsed().as_micros() as u64).to_be_bytes());
                    frame.extend(pcm);
                    peer.audio_frame(frame);
                }
            }
            if !crate::shutdown::requested() {
                tracing::warn!("audio capture ended; control and video remain available");
            }
        });
        tracing::info!(sink=%sink.name, "private 48 kHz stereo PCM audio capture active");
        Ok(Self {
            sink,
            peer,
            capture,
            worker: Some(worker),
        })
    }
    pub fn sink(&self) -> &str {
        &self.sink.name
    }
    pub fn set_peer(&self, peer: Option<&Arc<Peer>>) {
        *self.peer.lock().unwrap() = peer.map_or_else(Weak::new, Arc::downgrade);
    }
}
impl Drop for Session {
    fn drop(&mut self) {
        self.set_peer(None);
        let _ = self.capture.kill();
        let _ = self.capture.wait();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}
