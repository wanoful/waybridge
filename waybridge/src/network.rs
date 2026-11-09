//! One authenticated viewer; bounded input, priority control, latest-frame-only video.
use crate::protocol;
use serde_json::{json, Value};
use std::{
    collections::{BTreeMap, BTreeSet, VecDeque},
    io,
    net::{TcpListener, TcpStream},
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc::{sync_channel, Receiver, SyncSender},
        Arc, Condvar, Mutex, Weak,
    },
    thread,
    time::{Duration, Instant},
};

pub enum Event {
    Connected(Arc<Peer>),
    Message(Value),
    Disconnected,
    Clipboard {
        generation: u64,
        mime: String,
        bytes: Vec<u8>,
    },
}

type QueuedFrame = (u8, Vec<u8>, Option<Instant>);

#[derive(Default)]
struct Queue {
    controls: VecDeque<Vec<u8>>,
    frames: BTreeMap<u32, VecDeque<QueuedFrame>>,
    png_only: BTreeSet<u32>,
    active_codec: BTreeMap<u32, waybridge_codec::Codec>,
    gpu_disabled: BTreeSet<u32>,
    gpu_refresh: BTreeSet<u32>,
    in_flight: BTreeMap<u32, BTreeMap<u64, std::time::Instant>>,
    closed_windows: BTreeSet<u32>,
    last_frame_id: u32,
    bulk: VecDeque<Vec<u8>>,
    closed: bool,
    resizes: BTreeMap<u32, ResizeTiming>,
}

struct ResizeTiming {
    width: u32,
    height: u32,
    started: Instant,
    encoded: bool,
}

impl Queue {
    fn resize_marker(&mut self, id: u32, bytes: &[u8]) -> Option<Instant> {
        let resize = self.resizes.get_mut(&id)?;
        if bytes.len() < 12
            || u32::from_be_bytes(bytes[4..8].try_into().unwrap()) != resize.width
            || u32::from_be_bytes(bytes[8..12].try_into().unwrap()) != resize.height
        {
            return None;
        }
        if !resize.encoded {
            crate::metrics::record("resize_first_encoded", resize.started.elapsed());
            resize.encoded = true;
        }
        Some(resize.started)
    }
}

pub struct Peer {
    queue: Mutex<Queue>,
    wake: Condvar,
    split_media: bool,
    codecs: BTreeSet<waybridge_codec::Codec>,
    acknowledgements: bool,
    media_attached: AtomicBool,
}

impl Peer {
    pub fn begin_resize(&self, id: u32, width: u32, height: u32, started: Instant) {
        self.queue.lock().unwrap().resizes.insert(
            id,
            ResizeTiming {
                width,
                height,
                started,
                encoded: false,
            },
        );
    }
    pub fn control(&self, message: Value) {
        let mut q = self.queue.lock().unwrap();
        if q.closed {
            return;
        }
        if q.controls.len() >= 128 {
            q.closed = true;
        } else {
            q.controls.push_back(serde_json::to_vec(&message).unwrap());
        }
        self.wake.notify_all();
    }
    pub fn frame(&self, id: u32, bytes: Vec<u8>) {
        let mut q = self.queue.lock().unwrap();
        if !q.closed && !q.closed_windows.contains(&id) {
            if self.acknowledgements {
                if let Some(previous) = q.frames.get(&id) {
                    let sequences: Vec<_> = previous
                        .iter()
                        .map(|(_, bytes, _)| u64::from_be_bytes(bytes[12..20].try_into().unwrap()))
                        .collect();
                    for sequence in sequences {
                        q.in_flight.entry(id).or_default().remove(&sequence);
                    }
                }
                q.in_flight.entry(id).or_default().insert(
                    u64::from_be_bytes(bytes[12..20].try_into().unwrap()),
                    std::time::Instant::now(),
                );
            }
            let resize = q.resize_marker(id, &bytes);
            q.frames
                .insert(id, VecDeque::from([(protocol::FRAME, bytes, resize)]));
        }
        self.wake.notify_all();
    }
    pub fn supports(&self, codec: waybridge_codec::Codec, id: u32) -> bool {
        self.codecs.contains(&codec) && !self.queue.lock().unwrap().png_only.contains(&id)
    }
    pub fn gpu_allowed(&self, id: u32) -> bool {
        !self.queue.lock().unwrap().gpu_disabled.contains(&id)
    }
    pub fn disable_gpu(&self, id: u32) {
        let mut q = self.queue.lock().unwrap();
        if q.closed || q.closed_windows.contains(&id) {
            return;
        }
        if q.gpu_disabled.insert(id) {
            q.gpu_refresh.insert(id);
        }
    }
    pub fn take_gpu_refresh(&self, id: u32) -> bool {
        self.queue.lock().unwrap().gpu_refresh.remove(&id)
    }
    pub fn video_ready(&self, id: u32) -> bool {
        let q = self.queue.lock().unwrap();
        !q.closed
            && !q.closed_windows.contains(&id)
            && q.frames.get(&id).is_none_or(|frames| frames.len() < 2)
            && (!self.acknowledgements
                || q.in_flight.get(&id).is_none_or(|frames| frames.len() < 2))
    }
    pub fn window_closed(&self, id: u32) -> bool {
        self.queue.lock().unwrap().closed_windows.contains(&id)
    }
    pub fn acknowledge(&self, id: u32, sequence: u64) {
        let mut q = self.queue.lock().unwrap();
        if let Some(frames) = q.in_flight.get_mut(&id) {
            while frames
                .first_key_value()
                .is_some_and(|(s, _)| *s <= sequence)
            {
                let (_, started) = frames.pop_first().unwrap();
                crate::metrics::record("display_roundtrip", started.elapsed());
            }
        }
        self.wake.notify_all();
    }
    pub fn is_closed(&self) -> bool {
        self.queue.lock().unwrap().closed
    }
    pub fn video_frame(&self, id: u32, codec: waybridge_codec::Codec, bytes: Vec<u8>) {
        let mut q = self.queue.lock().unwrap();
        if !q.closed && !q.closed_windows.contains(&id) && !q.png_only.contains(&id) {
            if self.acknowledgements {
                q.in_flight.entry(id).or_default().insert(
                    u64::from_be_bytes(bytes[12..20].try_into().unwrap()),
                    std::time::Instant::now(),
                );
            }
            q.active_codec.insert(id, codec);
            let resize = q.resize_marker(id, &bytes);
            q.frames.entry(id).or_default().push_back((
                match codec {
                    waybridge_codec::Codec::H264 => protocol::VIDEO,
                    waybridge_codec::Codec::Hevc => protocol::HEVC_VIDEO,
                    waybridge_codec::Codec::Av1 => protocol::AV1_VIDEO,
                },
                bytes,
                resize,
            ));
        }
        self.wake.notify_all();
    }
    pub fn fallback(&self, id: u32, codec: Option<waybridge_codec::Codec>) -> bool {
        let mut q = self.queue.lock().unwrap();
        if codec.is_some_and(|codec| q.active_codec.get(&id) != Some(&codec)) {
            return false;
        }
        q.png_only.insert(id);
        q.frames.remove(&id);
        q.in_flight.remove(&id);
        self.wake.notify_all();
        true
    }
    pub fn forget_window(&self, id: u32) {
        let mut q = self.queue.lock().unwrap();
        q.frames.remove(&id);
        q.closed_windows.insert(id);
        q.active_codec.remove(&id);
        q.gpu_disabled.remove(&id);
        q.gpu_refresh.remove(&id);
        q.in_flight.remove(&id);
        q.resizes.remove(&id);
    }
    /// File workers may wait for network capacity; the compositor never does.
    pub fn bulk(&self, message: Value) -> bool {
        let mut q = self.queue.lock().unwrap();
        while !q.closed && q.bulk.len() >= 16 {
            q = self.wake.wait(q).unwrap();
        }
        if q.closed {
            return false;
        }
        q.bulk.push_back(serde_json::to_vec(&message).unwrap());
        self.wake.notify_all();
        true
    }
    fn close(&self) {
        self.queue.lock().unwrap().closed = true;
        self.wake.notify_all();
    }
    fn write_loop(&self, mut socket: TcpStream, media: bool) -> io::Result<()> {
        loop {
            let (kind, bytes, resize) = {
                let mut q = self.queue.lock().unwrap();
                let frames_allowed = media || !self.split_media;
                while !q.closed
                    && ((media || (q.controls.is_empty() && q.bulk.is_empty()))
                        && (!frames_allowed || q.frames.is_empty()))
                {
                    q = self.wake.wait(q).unwrap();
                }
                if q.closed {
                    return Ok(());
                }
                if !media && !q.controls.is_empty() {
                    (protocol::JSON, q.controls.pop_front().unwrap(), None)
                } else if frames_allowed && !q.frames.is_empty() {
                    let id = *q
                        .frames
                        .range((
                            std::ops::Bound::Excluded(q.last_frame_id),
                            std::ops::Bound::Unbounded,
                        ))
                        .next()
                        .or_else(|| q.frames.first_key_value())
                        .unwrap()
                        .0;
                    q.last_frame_id = id;
                    let frame = q.frames.get_mut(&id).unwrap().pop_front().unwrap();
                    if q.frames[&id].is_empty() {
                        q.frames.remove(&id);
                    }
                    frame
                } else {
                    (protocol::JSON, q.bulk.pop_front().unwrap(), None)
                }
            };
            self.wake.notify_all();
            let start = std::time::Instant::now();
            protocol::write_packet(&mut socket, kind, &bytes)?;
            if kind != protocol::JSON {
                crate::metrics::record("send", start.elapsed());
                if let Some(started) = resize {
                    let id = u32::from_be_bytes(bytes[..4].try_into().unwrap());
                    let mut q = self.queue.lock().unwrap();
                    if q.resizes.get(&id).is_some_and(|r| r.started == started) {
                        crate::metrics::record("resize_first_sent", started.elapsed());
                        q.resizes.remove(&id);
                    }
                }
            }
        }
    }
}

type MediaSession = Arc<Mutex<Option<(String, Weak<Peer>)>>>;

pub fn listen(
    address: &str,
    token: String,
) -> anyhow::Result<(Receiver<Event>, SyncSender<Event>)> {
    let listener = TcpListener::bind(address)?;
    tracing::info!(address = %listener.local_addr()?, "viewer listener ready");
    let (tx, rx) = sync_channel(256);
    let events = tx.clone();
    let occupied = Arc::new(AtomicBool::new(false));
    let session: MediaSession = Arc::new(Mutex::new(None));
    thread::spawn(move || {
        for socket in listener.incoming() {
            let Ok(mut socket) = socket else { continue };
            let (token, tx, occupied, session) =
                (token.clone(), tx.clone(), occupied.clone(), session.clone());
            // Authentication is handled outside the compositor loop and cannot stall Wayland.
            thread::spawn(move || {
                let _ = socket.set_nodelay(true);
                let _ = socket.set_read_timeout(Some(Duration::from_secs(5)));
                let _ = socket.set_write_timeout(Some(Duration::from_secs(3)));
                let Ok((protocol::JSON, body)) = protocol::read_packet(&mut socket) else {
                    return;
                };
                let Ok(hello) = serde_json::from_slice::<Value>(&body) else {
                    return;
                };
                if hello["type"] != "hello"
                    || hello["version"] != protocol::VERSION
                    || hello["token"].as_str() != Some(&token)
                {
                    return;
                }
                if hello["channel"] == "media" {
                    let peer = session.lock().unwrap().as_ref().and_then(|(key, peer)| {
                        (hello["media_key"].as_str() == Some(key))
                            .then(|| peer.upgrade())
                            .flatten()
                    });
                    let Some(peer) = peer else { return };
                    if !peer.split_media || peer.media_attached.swap(true, Ordering::SeqCst) {
                        return;
                    }
                    let _ = protocol::write_packet(
                        &mut socket,
                        protocol::JSON,
                        b"{\"type\":\"media_ready\"}",
                    );
                    let _ = peer.write_loop(socket, true);
                    peer.close();
                    return;
                }
                if occupied.swap(true, Ordering::SeqCst) {
                    return;
                }
                let peer = Arc::new(Peer {
                    queue: Mutex::new(Queue::default()),
                    wake: Condvar::new(),
                    split_media: hello["split_media"].as_bool().unwrap_or(false),
                    codecs: waybridge_codec::Codec::ALL
                        .into_iter()
                        .filter(|codec| {
                            hello["codecs"].as_array().is_some_and(|codecs| {
                                codecs.iter().any(|c| c == codec.capability())
                            })
                        })
                        .collect(),
                    media_attached: AtomicBool::new(false),
                    acknowledgements: hello["frame_ack"].as_bool().unwrap_or(false),
                });
                let mut key = [0; 32];
                if std::fs::File::open("/dev/urandom")
                    .and_then(|mut f| {
                        use std::io::Read;
                        f.read_exact(&mut key)
                    })
                    .is_err()
                {
                    occupied.store(false, Ordering::SeqCst);
                    return;
                }
                let key = key.iter().map(|b| format!("{b:02x}")).collect::<String>();
                *session.lock().unwrap() = Some((key.clone(), Arc::downgrade(&peer)));
                let Ok(writer) = socket.try_clone() else {
                    occupied.store(false, Ordering::SeqCst);
                    return;
                };
                let writer_peer = peer.clone();
                let writer_handle = thread::spawn(move || {
                    let shutdown_socket = writer.try_clone().ok();
                    let _ = writer_peer.write_loop(writer, false);
                    if let Some(s) = shutdown_socket {
                        let _ = s.shutdown(std::net::Shutdown::Both);
                    }
                });
                peer.control(json!({"type":"hello", "version":protocol::VERSION, "codec":"mixed", "split_media":peer.split_media, "media_key":key, "codecs":["png_rgba","h264_alpha","hevc_alpha","av1_alpha"], "features":["windows","input","resize","clipboard_text","clipboard_png","clipboard_files","file_drop_in","window_decorations","dpi","alpha"]}));
                if tx.send(Event::Connected(peer.clone())).is_ok() {
                    let _ = tx.send(Event::Message(hello));
                    let _ = socket.set_read_timeout(None);
                    while let Ok((protocol::JSON, body)) = protocol::read_packet(&mut socket) {
                        match serde_json::from_slice::<Value>(&body) {
                            Ok(value) => {
                                if tx.send(Event::Message(value)).is_err() {
                                    break;
                                }
                            }
                            Err(_) => break,
                        }
                    }
                }
                peer.close();
                let _ = socket.shutdown(std::net::Shutdown::Both);
                let _ = writer_handle.join();
                let _ = tx.send(Event::Disconnected);
                *session.lock().unwrap() = None;
                occupied.store(false, Ordering::SeqCst);
            });
        }
    });
    Ok((rx, events))
}
