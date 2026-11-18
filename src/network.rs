//! One authenticated viewer; bounded input, priority control, latest-frame-only video.
use crate::{protocol, udp};
use serde_json::{json, Value};
use std::{
    collections::{BTreeMap, BTreeSet, VecDeque},
    io,
    net::{TcpListener, TcpStream},
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        mpsc::{sync_channel, Receiver, SyncSender},
        Arc, Condvar, Mutex, Weak,
    },
    thread,
    time::{Duration, Instant},
};

pub enum Event {
    TrayActivation {
        peer: Arc<Peer>,
        request_id: String,
        pid: u32,
        ready: SyncSender<bool>,
    },
    UploadedFiles(crate::transfer_worker::Completed),
    DragFiles {
        id: u64,
        result: Result<crate::drag::Export, String>,
    },
    Connected(Arc<Peer>),
    Message(Value),
    Disconnected,
    Icon {
        id: u32,
        revision: u64,
        app_id: String,
        png: Option<Arc<Vec<u8>>>,
    },
    Clipboard {
        generation: u64,
        mime: String,
        bytes: Vec<u8>,
    },
}

struct QueuedFrame {
    kind: u8,
    bytes: Vec<u8>,
    resize: Option<Instant>,
    queued: Instant,
}

#[derive(Default)]
struct Queue {
    audio: VecDeque<Vec<u8>>,
    audio_unacked: BTreeSet<u64>,
    repairs: VecDeque<Vec<u8>>,
    controls: VecDeque<Vec<u8>>,
    cursor: Option<Vec<u8>>,
    drag_visual: Option<Vec<u8>>,
    icons: BTreeMap<u32, Vec<u8>>,
    tray_icons: BTreeMap<String, Vec<u8>>,
    frames: BTreeMap<u32, VecDeque<QueuedFrame>>,
    png_only: BTreeSet<u32>,
    active_codec: BTreeMap<u32, waybridge_codec::Codec>,
    gpu_disabled: BTreeSet<u32>,
    capture_refresh: BTreeSet<u32>,
    hdr_windows: BTreeSet<u32>,
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
    fn next_frame(&self) -> Option<(u32, usize)> {
        self.frames
            .range((
                std::ops::Bound::Excluded(self.last_frame_id),
                std::ops::Bound::Unbounded,
            ))
            .next()
            .or_else(|| self.frames.first_key_value())
            .and_then(|(id, frames)| frames.front().map(|frame| (*id, frame.bytes.len())))
    }
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
    audio_enabled: bool,
    audio_attached: AtomicBool,
    udp: Option<Arc<udp::Transport>>,
    queue: Mutex<Queue>,
    wake: Condvar,
    file_tree: bool,
    file_epoch: AtomicU64,
    file_export_generation: AtomicU64,
    split_media: bool,
    codecs: BTreeSet<waybridge_codec::Codec>,
    acknowledgements: bool,
    cursor_v1: bool,
    window_icon_v1: bool,
    notifications_v1: bool,
    notifications_initial: bool,
    window_semantics_v1: bool,
    activation_v1: bool,
    tray_v1: bool,
    data_drag_v1: bool,
    desktop_input: AtomicBool,
    media_attached: AtomicBool,
}

impl Peer {
    pub fn drag_visual(&self, message: Value) {
        let bytes = serde_json::to_vec(&message).unwrap();
        if bytes.len() > 352 * 1024 {
            return;
        }
        let mut q = self.queue.lock().unwrap();
        if !q.closed {
            q.drag_visual = Some(bytes);
        }
        self.wake.notify_all();
    }
    pub fn notifications_enabled(&self) -> bool {
        self.notifications_v1
    }
    pub fn notifications_initial(&self) -> bool {
        self.notifications_v1 && self.notifications_initial
    }
    pub fn audio_frame(&self, frame: Vec<u8>) {
        if !self.audio_enabled || !self.audio_attached.load(Ordering::Relaxed) {
            return;
        }
        let mut q = self.queue.lock().unwrap();
        if q.closed {
            return;
        }
        while q.audio.len() >= 4 {
            q.audio.pop_front();
        }
        q.audio.push_back(frame);
        self.wake.notify_all();
    }
    fn write_audio(&self, mut socket: TcpStream, alive: &AtomicBool) -> io::Result<()> {
        loop {
            let frame = {
                let mut q = self.queue.lock().unwrap();
                while (q.audio.is_empty() || q.audio_unacked.len() >= 4)
                    && !q.closed
                    && alive.load(Ordering::Relaxed)
                {
                    q = self.wake.wait(q).unwrap();
                }
                if q.closed || !alive.load(Ordering::Relaxed) {
                    return Ok(());
                }
                let frame = q.audio.pop_front().unwrap();
                q.audio_unacked
                    .insert(u64::from_be_bytes(frame[12..20].try_into().unwrap()));
                frame
            };
            protocol::write_packet(&mut socket, 3, &frame)?;
        }
    }
    pub fn data_drag_enabled(&self) -> bool {
        self.data_drag_v1
    }
    pub fn cursor_enabled(&self) -> bool {
        self.cursor_v1
    }
    pub fn cursor(&self, message: Value) {
        if !self.cursor_v1 {
            return;
        }
        let mut q = self.queue.lock().unwrap();
        if !q.closed {
            q.cursor = Some(serde_json::to_vec(&message).unwrap());
        }
        self.wake.notify_all();
    }
    pub fn icons_enabled(&self) -> bool {
        self.window_icon_v1
    }
    pub fn window_icon(&self, id: u32, message: Value) -> bool {
        if !self.window_icon_v1 {
            return false;
        }
        let mut q = self.queue.lock().unwrap();
        if q.closed
            || q.closed_windows.contains(&id)
            || (q.icons.len() >= 128 && !q.icons.contains_key(&id))
        {
            return false;
        }
        let bytes = serde_json::to_vec(&message).unwrap();
        if bytes.len() > 132 * 1024 {
            return false;
        }
        q.icons.insert(id, bytes);
        self.wake.notify_all();
        true
    }

    pub fn set_desktop_input(&self, enabled: bool) {
        self.desktop_input.store(enabled, Ordering::SeqCst);
    }
    pub fn desktop_input(&self) -> bool {
        self.desktop_input.load(Ordering::SeqCst) && !self.is_closed()
    }
    pub fn tray_enabled(&self) -> bool {
        self.tray_v1
    }
    pub fn tray_icon(&self, id: &str, message: Value) -> bool {
        if !self.tray_v1 {
            return false;
        }
        let bytes = serde_json::to_vec(&message).unwrap();
        if bytes.len() > 132 * 1024 {
            return false;
        }
        let mut q = self.queue.lock().unwrap();
        let cost: usize = q.tray_icons.values().map(Vec::len).sum();
        let replaced = q.tray_icons.get(id).map_or(0, Vec::len);
        if q.closed
            || q.tray_icons.len() >= 64 && !q.tray_icons.contains_key(id)
            || cost - replaced + bytes.len() > 512 * 1024
        {
            return false;
        }
        q.tray_icons.insert(id.to_owned(), bytes);
        self.wake.notify_all();
        true
    }
    pub fn semantics_enabled(&self) -> bool {
        self.window_semantics_v1
    }
    pub fn file_tree(&self) -> bool {
        self.file_tree
    }
    pub fn file_epoch(&self) -> u64 {
        self.file_epoch.load(Ordering::Relaxed)
    }
    pub fn set_file_epoch(&self, epoch: u64) {
        self.file_epoch.store(epoch, Ordering::Relaxed);
    }
    pub fn begin_file_export(&self, generation: u64) {
        self.file_export_generation
            .store(generation, Ordering::Relaxed);
    }
    pub fn file_export_current(&self, generation: u64) -> bool {
        !self.is_closed() && self.file_export_generation.load(Ordering::Relaxed) == generation
    }

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
                        .map(|frame| u64::from_be_bytes(frame.bytes[12..20].try_into().unwrap()))
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
            q.frames.insert(
                id,
                VecDeque::from([QueuedFrame {
                    kind: protocol::FRAME,
                    bytes,
                    resize,
                    queued: Instant::now(),
                }]),
            );
        }
        self.wake.notify_all();
    }
    pub fn supports(&self, codec: waybridge_codec::Codec, id: u32) -> bool {
        self.codecs.contains(&codec) && !self.queue.lock().unwrap().png_only.contains(&id)
    }
    pub fn hdr_allowed(&self, id: u32) -> bool {
        self.queue.lock().unwrap().hdr_windows.contains(&id)
    }
    pub fn set_hdr(&self, id: u32, enabled: bool) {
        let mut q = self.queue.lock().unwrap();
        if enabled {
            q.hdr_windows.insert(id);
        } else {
            q.hdr_windows.remove(&id);
        }
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
            q.capture_refresh.insert(id);
        }
    }
    pub fn request_capture(&self, id: u32) {
        let mut q = self.queue.lock().unwrap();
        if !q.closed && !q.closed_windows.contains(&id) {
            q.capture_refresh.insert(id);
        }
    }
    pub fn take_capture_refresh(&self, id: u32) -> bool {
        self.queue.lock().unwrap().capture_refresh.remove(&id)
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
            q.frames.entry(id).or_default().push_back(QueuedFrame {
                kind: match codec {
                    waybridge_codec::Codec::H264 => protocol::VIDEO,
                    waybridge_codec::Codec::Hevc => protocol::HEVC_VIDEO,
                    waybridge_codec::Codec::Av1 => protocol::AV1_VIDEO,
                    waybridge_codec::Codec::HevcHdr => protocol::HEVC_HDR_VIDEO,
                    waybridge_codec::Codec::Av1Hdr => protocol::AV1_HDR_VIDEO,
                },
                bytes,
                resize,
                queued: Instant::now(),
            });
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
        q.capture_refresh.remove(&id);
        q.hdr_windows.remove(&id);
        q.in_flight.remove(&id);
        q.resizes.remove(&id);
        q.icons.remove(&id);
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
    pub fn close(&self) {
        if let Some(udp) = &self.udp {
            udp.cancel();
        }
        self.queue.lock().unwrap().closed = true;
        self.wake.notify_all();
    }
    fn write_loop(&self, mut socket: TcpStream, media: bool) -> io::Result<()> {
        let mut udp_started = false;
        loop {
            let (kind, bytes, resize) = {
                let mut q = self.queue.lock().unwrap();
                let frames_allowed = media || !self.split_media;
                loop {
                    let frame_ready = q.next_frame().is_some_and(|(_, size)| {
                        !media
                            || self
                                .udp
                                .as_ref()
                                .is_none_or(|u| !u.ready() || u.can_send(size))
                    });
                    let ready = (!media
                        && (!q.controls.is_empty()
                            || !q.bulk.is_empty()
                            || q.cursor.is_some()
                            || q.drag_visual.is_some()
                            || !q.icons.is_empty()
                            || !q.tray_icons.is_empty()))
                        || (frames_allowed && (!q.repairs.is_empty() || frame_ready));
                    if q.closed || ready {
                        break;
                    }
                    q = self.wake.wait(q).unwrap();
                }
                if q.closed {
                    return Ok(());
                }
                if !media && !q.controls.is_empty() {
                    (protocol::JSON, q.controls.pop_front().unwrap(), None)
                } else if frames_allowed && !q.repairs.is_empty() {
                    (udp::REPAIR, q.repairs.pop_front().unwrap(), None)
                } else if !media && q.cursor.is_some() {
                    (protocol::JSON, q.cursor.take().unwrap(), None)
                } else if !media && q.drag_visual.is_some() {
                    (protocol::JSON, q.drag_visual.take().unwrap(), None)
                } else if !media && !q.icons.is_empty() {
                    (protocol::JSON, q.icons.pop_first().unwrap().1, None)
                } else if !media && !q.tray_icons.is_empty() {
                    (protocol::JSON, q.tray_icons.pop_first().unwrap().1, None)
                } else if frames_allowed && !q.frames.is_empty() {
                    let id = q.next_frame().unwrap().0;
                    q.last_frame_id = id;
                    let frame = q.frames.get_mut(&id).unwrap().pop_front().unwrap();
                    if q.frames[&id].is_empty() {
                        q.frames.remove(&id);
                    }
                    crate::metrics::record("media_send_queue", frame.queued.elapsed());
                    (frame.kind, frame.bytes, frame.resize)
                } else {
                    (protocol::JSON, q.bulk.pop_front().unwrap(), None)
                }
            };
            self.wake.notify_all();
            let start = std::time::Instant::now();
            if media
                && (kind == protocol::FRAME || waybridge_codec::Codec::from_kind(kind).is_some())
                && self.udp.as_ref().is_some_and(|u| u.ready())
            {
                if !udp_started {
                    protocol::write_packet(
                        &mut socket,
                        protocol::JSON,
                        b"{\"type\":\"udp_start\"}",
                    )?;
                    udp_started = true;
                }
                self.udp.as_ref().unwrap().send(kind, &bytes, &mut socket)?;
            } else {
                protocol::write_packet(&mut socket, kind, &bytes)?;
            }
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

pub struct Options {
    pub hdr_enabled: bool,
    pub audio: bool,
    pub udp: Option<Arc<udp::Hub>>,
}
pub fn listen(
    address: &str,
    token: String,
    options: Options,
) -> anyhow::Result<(Receiver<Event>, SyncSender<Event>)> {
    let Options {
        hdr_enabled,
        audio,
        udp,
    } = options;
    let listener = TcpListener::bind(address)?;
    tracing::info!(address = %listener.local_addr()?, "viewer listener ready");
    let (tx, rx) = sync_channel(256);
    let events = tx.clone();
    let occupied = Arc::new(AtomicBool::new(false));
    let session: MediaSession = Arc::new(Mutex::new(None));
    thread::spawn(move || {
        for socket in listener.incoming() {
            let Ok(mut socket) = socket else { continue };
            let (token, tx, occupied, session, udp) = (
                token.clone(),
                tx.clone(),
                occupied.clone(),
                session.clone(),
                udp.clone(),
            );
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
                if hello["channel"] == "audio" {
                    let peer = session.lock().unwrap().as_ref().and_then(|(key, peer)| {
                        (hello["media_key"].as_str() == Some(key))
                            .then(|| peer.upgrade())
                            .flatten()
                    });
                    let Some(peer) = peer else { return };
                    if !peer.audio_enabled || peer.audio_attached.swap(true, Ordering::SeqCst) {
                        return;
                    }
                    let Ok(mut ack_socket) = socket.try_clone() else {
                        peer.audio_attached.store(false, Ordering::SeqCst);
                        return;
                    };
                    let shutdown = socket.try_clone().ok();
                    let alive = Arc::new(AtomicBool::new(true));
                    let reader_alive = alive.clone();
                    let reader_peer = peer.clone();
                    let reader = thread::spawn(move || {
                        use std::io::Read;
                        let mut header = [0; 4];
                        while ack_socket.read_exact(&mut header).is_ok() {
                            let size = u32::from_be_bytes(header) as usize;
                            if !(2..=512).contains(&size) {
                                break;
                            }
                            let mut body = vec![0; size];
                            if ack_socket.read_exact(&mut body).is_err()
                                || body[0] != protocol::JSON
                            {
                                break;
                            }
                            let Ok(value) = serde_json::from_slice::<Value>(&body[1..]) else {
                                break;
                            };
                            if value["type"] != "audio_ack" {
                                break;
                            }
                            let Some(sequence) = value["sequence"].as_u64() else {
                                break;
                            };
                            let mut q = reader_peer.queue.lock().unwrap();
                            if q.audio_unacked.contains(&sequence) {
                                q.audio_unacked.retain(|s| *s > sequence);
                                reader_peer.wake.notify_all();
                            }
                        }
                        // Change the wait predicate under its mutex to prevent a lost EOF wake.
                        let _queue = reader_peer.queue.lock().unwrap();
                        reader_alive.store(false, Ordering::Relaxed);
                        reader_peer.wake.notify_all();
                    });
                    let _ = protocol::write_packet(
                        &mut socket,
                        protocol::JSON,
                        b"{\"type\":\"audio_ready\"}",
                    );
                    let _ = peer.write_audio(socket, &alive);
                    if let Some(socket) = shutdown {
                        let _ = socket.shutdown(std::net::Shutdown::Both);
                    }
                    let _ = reader.join();
                    let mut q = peer.queue.lock().unwrap();
                    q.audio.clear();
                    q.audio_unacked.clear();
                    peer.audio_attached.store(false, Ordering::SeqCst);
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
                let udp = if hello["udp_fec_v1"] == true
                    && hello["split_media"] == true
                    && hello["frame_ack"] == true
                {
                    udp.and_then(|hub| hub.session().ok())
                } else {
                    None
                };
                let peer = Arc::new(Peer {
                    audio_enabled: audio && hello["audio_pcm"].as_bool().unwrap_or(false),
                    audio_attached: AtomicBool::new(false),
                    udp,
                    queue: Mutex::new(Queue::default()),
                    wake: Condvar::new(),
                    file_tree: hello["file_tree"].as_bool().unwrap_or(false),
                    file_epoch: AtomicU64::new(0),
                    file_export_generation: AtomicU64::new(0),
                    split_media: hello["split_media"].as_bool().unwrap_or(false),
                    codecs: waybridge_codec::Codec::ALL
                        .into_iter()
                        .filter(|codec| {
                            (!codec.hdr()
                                || (hdr_enabled
                                    && hello["hdr"].as_bool() == Some(true)
                                    && hello["color_spaces"].as_array().is_some_and(|spaces| {
                                        spaces.iter().any(|s| s == "bt2020_pq")
                                    })))
                                && hello["codecs"].as_array().is_some_and(|codecs| {
                                    codecs.iter().any(|c| c == codec.capability())
                                })
                        })
                        .collect(),
                    media_attached: AtomicBool::new(false),
                    acknowledgements: hello["frame_ack"].as_bool().unwrap_or(false),
                    cursor_v1: hello["cursor_v1"].as_bool().unwrap_or(false),
                    window_icon_v1: hello["window_icon_v1"].as_bool().unwrap_or(false),
                    notifications_v1: hello["notifications_v1"].as_bool().unwrap_or(false),
                    notifications_initial: hello["notifications_enabled"].as_bool().unwrap_or(true),
                    window_semantics_v1: hello["window_semantics_v1"].as_bool().unwrap_or(false),
                    activation_v1: hello["activation_v1"].as_bool().unwrap_or(false),
                    tray_v1: hello["tray_v1"].as_bool().unwrap_or(false),
                    data_drag_v1: hello["data_drag_v1"].as_bool().unwrap_or(false),
                    desktop_input: AtomicBool::new(true),
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
                peer.control(json!({"type":"hello", "version":protocol::VERSION, "codec":"mixed", "split_media":peer.split_media, "media_key":key,"udp_fec_v1":peer.udp.as_ref().map(|u|u.offer()), "cursor_v1":peer.cursor_v1,"window_icon_v1":peer.window_icon_v1,"window_semantics_v1":peer.window_semantics_v1,"notifications_v1":peer.notifications_v1,"tray_v1":peer.tray_v1,"pen_v1":hello["pen_v1"] == true,"pen_settings_v1":hello["pen_v1"] == true,"gamepad_v1":hello["gamepad_v1"] == true,"gamepad_settings_v1":hello["gamepad_v1"] == true,"touch_v1":hello["touch_v1"] == true, "touch_settings_v1":hello["touch_v1"] == true,"touch_interaction_v1":hello["touch_v1"] == true && hello["touch_interaction_v1"] == true,"touchpad_v1":hello["touchpad_v1"] == true,"data_drag_v1":peer.data_drag_v1,"data_drag_result_v1":peer.data_drag_v1,"activation_v1":peer.activation_v1, "hdr":hdr_enabled, "audio_pcm":peer.audio_enabled, "color_spaces":if hdr_enabled {vec!["bt709_sdr","bt2020_pq"]}else{vec!["bt709_sdr"]}, "color_depths":if hdr_enabled {vec![8,10]}else{vec![8]}, "codecs":["png_rgba","h264_alpha","hevc_alpha","av1_alpha","hevc_main10_pq_alpha16","av1_main10_pq_alpha16"], "features":["windows","input","resize","clipboard_text","clipboard_png","clipboard_files","file_drop_in","file_drop_out","file_tree","window_decorations","dpi","alpha","cursor_v1","window_icon_v1","window_semantics_v1","relative_pointer_locked","locked_pointer","ime_v3","pen_v1","pen_settings_v1","gamepad_v1","gamepad_settings_v1","touch_v1","touch_settings_v1","touch_interaction_v1","touchpad_v1","toplevel_drag_v1","stream_config_v1","notifications_v1","tray_v1","data_drag_v1","data_drag_result_v1","activation_v1"]}));
                if tx.send(Event::Connected(peer.clone())).is_ok() {
                    let _ = tx.send(Event::Message(hello));
                    let _ = socket.set_read_timeout(None);
                    while let Ok((protocol::JSON, body)) = protocol::read_packet(&mut socket) {
                        match serde_json::from_slice::<Value>(&body) {
                            Ok(value) => {
                                if let Some(udp) = &peer.udp {
                                    if value["type"] == "udp_ack" {
                                        if let Some(n) = value["message"].as_u64() {
                                            udp.ack(n);
                                            // Writer checks budget while holding q; synchronize the wake predicate.
                                            let _q = peer.queue.lock().unwrap();
                                            peer.wake.notify_all();
                                        }
                                        continue;
                                    }
                                    if value["type"] == "udp_retry" {
                                        if let Some(bytes) =
                                            value["message"].as_u64().and_then(|n| udp.retry(n))
                                        {
                                            let mut q = peer.queue.lock().unwrap();
                                            if q.repairs.len() < 4 {
                                                q.repairs.push_back(bytes);
                                                peer.wake.notify_all();
                                            }
                                        }
                                        continue;
                                    }
                                }
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
