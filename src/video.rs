//! Bounded raw-frame queue. Dependent H.264 packets are never discarded after encoding.
use crate::{
    media_budget::{self, Budget, Lease},
    metrics,
    network::Peer,
    protocol,
};
use image::{
    codecs::png::{CompressionType, FilterType, PngEncoder},
    ImageEncoder,
};
use std::{
    collections::{BTreeMap, VecDeque},
    sync::{Arc, Condvar, Mutex, Weak},
    thread,
    time::{Duration, Instant},
};
use waybridge_codec as nvenc;

pub struct Frame {
    pub id: u32,
    pub width: u32,
    pub height: u32,
    pub sequence: u64,
    pub geometry: [i32; 4],
    pub rgba: Vec<u8>,
    pub gpu: Option<nvenc::GpuFrame>,
    pub straight: bool,
    pub peer: Arc<Peer>,
    pub codec: Option<nvenc::Codec>,
    pub video_eligible: bool,
    pub budget_video: bool,
    pub enqueued: Instant,
    pub captured: Instant,
    pub _lease: Lease,
}
impl Frame {
    fn memory_bytes(&self) -> usize {
        self.gpu
            .as_ref()
            .map_or(self.rgba.len(), |frame| frame.memory_bytes())
    }
    fn input_key(&self) -> usize {
        self.gpu.as_ref().map_or(0, |frame| frame.device_key())
    }
    fn same_capture_format(&self, newer: &Self) -> bool {
        self.width == newer.width
            && self.height == newer.height
            && self.input_key() == newer.input_key()
            && self.gpu.as_ref().is_some_and(|f| f.hdr())
                == newer.gpu.as_ref().is_some_and(|f| f.hdr())
            && Arc::ptr_eq(&self.peer, &newer.peer)
    }
    fn download_gpu(&mut self) -> Result<(), String> {
        if let Some(gpu) = self.gpu.take() {
            let started = Instant::now();
            self.rgba = gpu.download()?;
            self.straight = true;
            metrics::record("gpu_fallback_readback", started.elapsed());
        }
        Ok(())
    }
}
#[derive(Default)]
struct Queue {
    frames: BTreeMap<u32, Frame>,
    bytes: usize,
    last_id: u32,
    forget: Vec<u32>,
    reset: bool,
    closed: bool,
    config: Option<(u32, u32)>,
    reclaim: Option<(u32, bool, bool)>,
    last_reclaim: Option<Instant>,
}
pub struct Encoder {
    queue: Arc<(Mutex<Queue>, Condvar)>,
    worker: Option<thread::JoinHandle<()>>,
    budget: Budget,
}
struct Session {
    width: u32,
    height: u32,
    codec: nvenc::Codec,
    input_key: usize,
    owner: Weak<Peer>,
    encoder: Option<nvenc::Encoder>,
    lease: Option<Lease>,
    touched: Instant,
    bitrate: u32,
    configured_bitrate: u32,
    rate_since: Instant,
    recovery_since: Option<Instant>,
    live_bitrate: bool,
    failed: bool,
    epoch: u32,
    force_idr: bool,
}

// Bound inactive GPU sessions globally, rather than reserving two for every window.
// The pixel budget bounds cached resolutions; driver allocations also include codec surfaces.
const MAX_IDLE_ENCODERS: usize = 2;
const MAX_IDLE_PIXELS: u64 = 4 * 1024 * 1024;
const MAX_RAW_BYTES: usize = 128 * 1024 * 1024;
const ACTIVE_VIDEO_WINDOW: Duration = Duration::from_secs(2);
const MIN_ENCODER_KBPS: u64 = 1_000;
const BACKOFF_INTERVAL: Duration = Duration::from_millis(500);
const RECOVERY_INTERVAL: Duration = Duration::from_secs(2);

fn target_bitrate(configured: u32, pacing: Option<u64>, active_windows: usize) -> u32 {
    let Some(pacing) = pacing else {
        return configured;
    };
    // Budget some headroom for FEC, headers, alpha and bitrate bursts.
    let share = pacing.saturating_mul(3) / 4 / active_windows.max(1) as u64;
    share.max(MIN_ENCODER_KBPS).min(u64::from(configured)) as u32
}

fn next_bitrate(current: u32, target: u32, held: Duration) -> u32 {
    if target < current
        && held >= BACKOFF_INTERVAL
        && (current - target >= current / 10 || held >= RECOVERY_INTERVAL)
    {
        target
    } else if target > current && held >= RECOVERY_INTERVAL {
        target.min(current.saturating_add((current / 5).max(500)))
    } else {
        current
    }
}

impl Session {
    fn choose_bitrate(&mut self, configured: u32, target: u32, now: Instant) -> u32 {
        if self.failed {
            return self.bitrate;
        }
        if self.encoder.is_none() || self.configured_bitrate != configured {
            return target;
        }
        // A brief transport recovery can be followed by another backoff.
        // Wait for sustained headroom before increasing the encoder rate.
        let since = if target > self.bitrate {
            *self.recovery_since.get_or_insert(now)
        } else {
            self.recovery_since = None;
            self.rate_since
        };
        next_bitrate(self.bitrate, target, now.duration_since(since))
    }

    fn update_bitrate(&mut self, bitrate: u32, now: Instant) {
        self.bitrate = bitrate;
        self.rate_since = now;
        self.recovery_since = None;
    }

    fn encode_frame(&mut self, frame: &mut Frame) -> Result<(Vec<u8>, bool), String> {
        let encoder = self.encoder.as_mut().unwrap();
        if let Some(gpu) = &mut frame.gpu {
            encoder.encode_gpu(gpu, frame.sequence, self.force_idr)
        } else {
            encoder.encode(&frame.rgba, frame.sequence, self.force_idr)
        }
    }

    fn ensure_encoder(
        &mut self,
        frame: &Frame,
        fps: u32,
        budget: &Budget,
        idle: &mut VecDeque<(u32, Session)>,
        pacing: Option<u64>,
        active_windows: usize,
    ) {
        if self.failed || self.encoder.is_some() {
            return;
        }
        let cost = media_budget::encoder(
            frame.width,
            frame.height,
            self.codec.hdr(),
            frame.gpu.is_some(),
        );
        let Some(lease) = budget.acquire(frame.id, cost).or_else(|| {
            idle.clear();
            budget.acquire(frame.id, cost)
        }) else {
            return;
        };
        let started = Instant::now();
        let create = || match &frame.gpu {
            Some(gpu) => nvenc::Encoder::with_gpu(
                self.codec,
                frame.width,
                frame.height,
                fps,
                self.bitrate,
                gpu,
            ),
            None => {
                nvenc::Encoder::with_codec(self.codec, frame.width, frame.height, fps, self.bitrate)
            }
        };
        let mut result = create();
        if result.is_err() && !idle.is_empty() {
            idle.clear();
            result = create();
        }
        metrics::record("encoder_init", started.elapsed());
        match result {
            Ok(encoder) => {
                self.encoder = Some(encoder);
                self.lease = Some(lease);
                tracing::info!(
                    id = frame.id,
                    width = frame.width,
                    height = frame.height,
                    codec = self.codec.name(),
                    input = if frame.gpu.is_some() { "cuda" } else { "cpu" },
                    bitrate_kbps = self.bitrate,
                    udp_pacing_kbps = pacing,
                    active_windows,
                    epoch = self.epoch,
                    "NVENC ultra-low-latency encoder active"
                );
            }
            Err(error) => {
                self.failed = true;
                if frame.gpu.is_some() {
                    frame.peer.disable_gpu(frame.id);
                }
                tracing::warn!(id=frame.id,%error,"NVENC unavailable; using lossless PNG");
            }
        }
    }
}

impl Queue {
    fn fits(&self, bytes: usize) -> bool {
        bytes <= MAX_RAW_BYTES.saturating_sub(self.bytes)
    }
    fn observe(&self) {
        metrics::gauge("raw_queue_frames", self.frames.len());
        metrics::gauge("raw_queue_bytes", self.bytes);
    }
}

fn trim_idle(idle: &mut VecDeque<(u32, Session)>) {
    while idle.len() > MAX_IDLE_ENCODERS
        || idle
            .iter()
            .map(|(_, s)| u64::from(s.width) * u64::from(s.height))
            .sum::<u64>()
            > MAX_IDLE_PIXELS
    {
        idle.pop_front();
    }
}

impl Encoder {
    pub fn new(fps: u32, bitrate: u32, budget: Budget) -> Self {
        let queue = Arc::new((Mutex::new(Queue::default()), Condvar::new()));
        let worker = queue.clone();
        let worker_budget = budget.clone();
        let worker = thread::spawn(move || {
            let (mut fps, mut bitrate) = (fps, bitrate);
            let mut sessions: BTreeMap<u32, Session> = BTreeMap::new();
            let mut idle = VecDeque::<(u32, Session)>::new();
            let mut epoch = 0u32;
            loop {
                let (frame, reset, forget, config, reclaim) = {
                    let (lock, wake) = &*worker;
                    let mut q = lock.lock().unwrap();
                    while q.frames.is_empty()
                        && !q.reset
                        && q.forget.is_empty()
                        && q.config.is_none()
                        && q.reclaim.is_none()
                        && !q.closed
                    {
                        q = wake.wait(q).unwrap();
                    }
                    if q.closed {
                        return;
                    }
                    let reset = std::mem::take(&mut q.reset);
                    let forget = std::mem::take(&mut q.forget);
                    let id = q
                        .frames
                        .range((
                            std::ops::Bound::Excluded(q.last_id),
                            std::ops::Bound::Unbounded,
                        ))
                        .next()
                        .or_else(|| q.frames.first_key_value())
                        .map(|(id, _)| *id);
                    let frame = id.map(|id| {
                        q.last_id = id;
                        let frame = q.frames.remove(&id).unwrap();
                        q.bytes -= frame.memory_bytes();
                        q.observe();
                        frame
                    });
                    let config = q.config.take();
                    let reclaim = q.reclaim.take();
                    (frame, reset, forget, config, reclaim)
                };
                if let Some((next_fps, next_bitrate)) = config {
                    if next_fps != fps {
                        sessions.clear();
                        idle.clear();
                    }
                    (fps, bitrate) = (next_fps, next_bitrate);
                }
                if reset {
                    sessions.clear();
                    idle.clear();
                }
                for id in forget {
                    sessions.remove(&id);
                    idle.retain(|(cached_id, _)| *cached_id != id);
                }
                if let Some((requested, obsolete, global)) = reclaim {
                    idle.clear();
                    // Also release this window's old pool/codec after a resize.
                    if obsolete {
                        sessions.remove(&requested);
                    }
                    let victim = sessions
                        .iter()
                        .filter(|(id, _)| {
                            global && **id != requested && Some(**id) != worker_budget.focused()
                        })
                        .min_by_key(|(_, s)| s.touched)
                        .map(|(id, _)| *id);
                    if let Some(victim) = victim {
                        sessions.remove(&victim);
                    }
                    metrics::increment("media_encoder_reclaim");
                }
                let Some(mut frame) = frame else { continue };
                if frame.peer.is_closed() || frame.peer.window_closed(frame.id) {
                    continue;
                }
                let codec = frame.codec;
                let selected_codec = if frame.gpu.as_ref().is_some_and(|f| f.hdr()) {
                    codec
                        .and_then(|c| c.hdr_variant())
                        .unwrap_or(nvenc::Codec::HevcHdr)
                } else {
                    codec.unwrap_or(nvenc::Codec::H264)
                };
                let use_video = codec.is_some()
                    && (!selected_codec.hdr()
                        || codec.and_then(|c| c.hdr_variant()) == Some(selected_codec))
                    && frame.video_eligible
                    && frame.peer.supports(selected_codec, frame.id)
                    && frame.width >= 48
                    && frame.height >= 48
                    && frame.width <= 4096
                    && frame.height <= 2304;
                if !frame.peer.video_ready(frame.id) {
                    // Keep the final raw update even if the application stops repainting under backpressure.
                    let (lock, wake) = &*worker;
                    let mut q = lock.lock().unwrap();
                    if q.closed {
                        return;
                    }
                    if !q.frames.contains_key(&frame.id) && q.fits(frame.memory_bytes()) {
                        q.bytes += frame.memory_bytes();
                        q.frames.insert(frame.id, frame);
                        q.observe();
                    } else if !q.frames.contains_key(&frame.id) {
                        // A concurrent capture may have used this slot. Keep the
                        // queue bounded and ask the compositor for current content.
                        frame.peer.request_capture(frame.id);
                        metrics::increment("raw_retry_budget");
                    }
                    let _ = wake.wait_timeout(q, Duration::from_millis(2)).unwrap();
                    continue;
                }
                let mut queue_wait = frame.enqueued.elapsed();
                let mut rate_selection = None;
                if use_video {
                    let now = Instant::now();
                    let same_owner = |s: &Session| {
                        s.owner
                            .upgrade()
                            .is_some_and(|peer| Arc::ptr_eq(&peer, &frame.peer))
                    };
                    let pacing = frame.peer.udp_video_rate();
                    let active_windows = 1 + if pacing.is_some() {
                        sessions
                            .iter()
                            .filter(|(id, s)| {
                                **id != frame.id
                                    && s.encoder.is_some()
                                    && now.duration_since(s.touched) < ACTIVE_VIDEO_WINDOW
                                    && same_owner(s)
                            })
                            .count()
                    } else {
                        0
                    };
                    let target = target_bitrate(bitrate, pacing, active_windows);
                    let selected_bitrate = sessions
                        .get_mut(&frame.id)
                        .filter(|s| same_owner(s))
                        .map_or(target, |s| s.choose_bitrate(bitrate, target, now));
                    rate_selection = Some((selected_bitrate, pacing, active_windows));
                    let recreate = sessions.get(&frame.id).is_none_or(|s| {
                        s.width != frame.width
                            || s.height != frame.height
                            || s.codec != selected_codec
                            || s.input_key != frame.input_key()
                            || !same_owner(s)
                    });
                    if recreate {
                        epoch = epoch.wrapping_add(1).max(1);
                        // Release obsolete bitrate variants before acquiring a lease.
                        idle.retain(|(id, s)| {
                            *id != frame.id || (same_owner(s) && s.bitrate == selected_bitrate)
                        });
                        let cached = idle.iter().position(|(id, s)| {
                            *id == frame.id
                                && s.width == frame.width
                                && s.height == frame.height
                                && s.codec == selected_codec
                                && s.input_key == frame.input_key()
                                && same_owner(s)
                        });
                        let mut next = cached.map(|index| idle.remove(index).unwrap().1);
                        if let Some(previous) = sessions.remove(&frame.id) {
                            if previous.encoder.is_some()
                                && same_owner(&previous)
                                && previous.bitrate == selected_bitrate
                            {
                                idle.push_back((frame.id, previous));
                                trim_idle(&mut idle);
                            }
                        }
                        if next.is_some() {
                            metrics::record("encoder_reuse", Duration::ZERO);
                            tracing::info!(
                                id = frame.id,
                                width = frame.width,
                                height = frame.height,
                                "NVENC encoder reused for resize; forcing fresh IDR"
                            );
                        }
                        let mut next = next.take().unwrap_or(Session {
                            width: frame.width,
                            height: frame.height,
                            codec: selected_codec,
                            input_key: frame.input_key(),
                            owner: Arc::downgrade(&frame.peer),
                            encoder: None,
                            lease: None,
                            touched: now,
                            bitrate: selected_bitrate,
                            configured_bitrate: bitrate,
                            rate_since: now,
                            recovery_since: None,
                            live_bitrate: true,
                            failed: false,
                            epoch,
                            force_idr: true,
                        });
                        next.epoch = epoch;
                        next.force_idr = true;
                        next.rate_since = now;
                        next.recovery_since = None;
                        next.configured_bitrate = bitrate;
                        sessions.insert(frame.id, next);
                    }
                    let session = sessions.get_mut(&frame.id).unwrap();
                    session.touched = Instant::now();
                    if session.encoder.is_none() {
                        session.update_bitrate(selected_bitrate, now);
                    }
                    session.ensure_encoder(
                        &frame,
                        fps,
                        &worker_budget,
                        &mut idle,
                        pacing,
                        active_windows,
                    );
                    // A cold session can take >100 ms. Prefer the latest unencoded
                    // image, and never transmit an obsolete intermediate resize.
                    let (lock, _) = &*worker;
                    let mut q = lock.lock().unwrap();
                    if let Some(latest) = q.frames.get(&frame.id) {
                        if !frame.same_capture_format(latest) {
                            continue;
                        }
                        let latest = q.frames.remove(&frame.id).unwrap();
                        q.bytes -= latest.memory_bytes();
                        q.observe();
                        frame = latest;
                        queue_wait = frame.enqueued.elapsed();
                    }
                }
                metrics::record("encode_queue", queue_wait);
                metrics::record("capture_to_encode", frame.captured.elapsed());
                let conversion = Instant::now();
                let transparent = frame.gpu.as_ref().map_or_else(
                    || frame.rgba.chunks_exact(4).any(|p| p[3] != 255),
                    |gpu| !gpu.alpha().is_empty(),
                );
                let alpha = if use_video && transparent {
                    if let Some(gpu) = &frame.gpu {
                        gpu.alpha().to_vec()
                    } else {
                        frame.rgba.chunks_exact(4).map(|p| p[3]).collect::<Vec<_>>()
                    }
                } else {
                    vec![]
                };
                if transparent && !frame.straight {
                    for p in frame.rgba.chunks_exact_mut(4) {
                        let a = p[3] as u32;
                        if a == 255 {
                            continue;
                        }
                        for c in &mut p[..3] {
                            *c = ((*c as u32 * 255 + a / 2).checked_div(a).unwrap_or(0)).min(255)
                                as u8;
                        }
                    }
                    frame.straight = true;
                }
                metrics::record("unpremultiply", conversion.elapsed());
                if let Some((selected_bitrate, pacing, active_windows)) = rate_selection {
                    let session = sessions.get_mut(&frame.id).unwrap();
                    let refresh = frame.peer.needs_video_refresh(frame.id);
                    let mut started = Instant::now();
                    let mut packet = None;
                    if session.encoder.is_some() && selected_bitrate != session.bitrate {
                        let staged = session.live_bitrate
                            && !session.force_idr
                            && !refresh
                            && session
                                .encoder
                                .as_mut()
                                .unwrap()
                                .stage_bitrate(selected_bitrate);
                        if staged {
                            let result = session.encode_frame(&mut frame);
                            if matches!(&result, Ok((_, true))) {
                                session.update_bitrate(selected_bitrate, started);
                                packet = Some(result);
                                metrics::record("encoder_reconfigure", started.elapsed());
                                tracing::info!(
                                    id = frame.id,
                                    bitrate_kbps = selected_bitrate,
                                    udp_pacing_kbps = pacing,
                                    active_windows,
                                    epoch = session.epoch,
                                    sequence = frame.sequence,
                                    "NVENC bitrate reconfigured in place"
                                );
                            } else {
                                session.live_bitrate = false;
                                metrics::increment("encoder_reconfigure_failed");
                                tracing::debug!(
                                    id = frame.id,
                                    bitrate_kbps = selected_bitrate,
                                    "NVENC did not confirm live bitrate change"
                                );
                            }
                        }
                        if packet.is_none() {
                            // The aborted encode was never published. Restart
                            // at the same input with an independent reference chain.
                            session.encoder = None;
                            session.lease = None;
                            epoch = epoch.wrapping_add(1).max(1);
                            session.epoch = epoch;
                            session.force_idr = true;
                            session.update_bitrate(selected_bitrate, started);
                            idle.retain(|(id, s)| *id != frame.id || s.bitrate == selected_bitrate);
                            session.ensure_encoder(
                                &frame,
                                fps,
                                &worker_budget,
                                &mut idle,
                                pacing,
                                active_windows,
                            );
                            if worker
                                .0
                                .lock()
                                .unwrap()
                                .frames
                                .get(&frame.id)
                                .is_some_and(|latest| !frame.same_capture_format(latest))
                            {
                                continue;
                            }
                            started = Instant::now();
                        }
                    }
                    session.configured_bitrate = bitrate;
                    if session.encoder.is_some() {
                        if refresh && !session.force_idr {
                            epoch = epoch.wrapping_add(1).max(1);
                            session.epoch = epoch;
                            session.force_idr = true;
                        }
                        let result = packet.unwrap_or_else(|| session.encode_frame(&mut frame));
                        match result {
                            Ok((color, key)) => {
                                session.force_idr = false;
                                metrics::record("nvenc", started.elapsed());
                                metrics::gauge("video_target_kbps", session.bitrate as usize);
                                let mask = if alpha.is_empty() {
                                    vec![]
                                } else {
                                    png(
                                        &alpha,
                                        frame.width,
                                        frame.height,
                                        if selected_codec.hdr() {
                                            image::ExtendedColorType::L16
                                        } else {
                                            image::ExtendedColorType::L8
                                        },
                                    )
                                    .unwrap_or_default()
                                };
                                if !alpha.is_empty() && mask.is_empty() {
                                    continue;
                                }
                                let mut body = protocol::frame(
                                    frame.id,
                                    frame.width,
                                    frame.height,
                                    frame.sequence,
                                    frame.geometry,
                                    &[],
                                );
                                for n in [
                                    session.epoch,
                                    ((frame.width + 1) & !1),
                                    ((frame.height + 1) & !1),
                                    u32::from(key),
                                    color.len() as u32,
                                    mask.len() as u32,
                                ] {
                                    body.extend(n.to_be_bytes());
                                }
                                body.extend(color);
                                body.extend(mask);
                                metrics::record(
                                    "capture_to_network_queue",
                                    frame.captured.elapsed(),
                                );
                                frame.peer.video_frame(frame.id, selected_codec, body);
                                continue;
                            }
                            Err(error) => {
                                tracing::warn!(id=frame.id,%error,"NVENC failed; falling back to PNG");
                                session.encoder = None;
                                session.lease = None;
                                session.failed = true;
                                if frame.gpu.is_some() {
                                    frame.peer.disable_gpu(frame.id);
                                }
                            }
                        }
                    }
                }
                if let Err(error) = frame.download_gpu() {
                    frame.peer.disable_gpu(frame.id);
                    tracing::warn!(id=frame.id,%error,"GPU readback fallback failed; requesting fresh CPU capture");
                    continue;
                }
                let started = Instant::now();
                if let Ok(png) = png(
                    &frame.rgba,
                    frame.width,
                    frame.height,
                    image::ExtendedColorType::Rgba8,
                ) {
                    metrics::record("png", started.elapsed());
                    let bytes = protocol::frame(
                        frame.id,
                        frame.width,
                        frame.height,
                        frame.sequence,
                        frame.geometry,
                        &png,
                    );
                    if bytes.len() < protocol::MAX_PACKET {
                        // PNG resets the client's hardware decoder. Preserve an
                        // optional idle encoder, but returning to any video must
                        // create a fresh epoch and independent keyframe.
                        if let Some(previous) = sessions.remove(&frame.id) {
                            if previous.encoder.is_some() {
                                idle.push_back((frame.id, previous));
                                trim_idle(&mut idle);
                            }
                        }
                        metrics::record("capture_to_network_queue", frame.captured.elapsed());
                        frame.peer.frame(frame.id, bytes);
                    }
                }
            }
        });
        Self {
            queue,
            worker: Some(worker),
            budget,
        }
    }
    pub fn budget(&self) -> &Budget {
        &self.budget
    }
    pub fn reclaim(&self, window: u32, obsolete: bool, global: bool) {
        let (lock, wake) = &*self.queue;
        let mut q = lock.lock().unwrap();
        if q.last_reclaim
            .is_none_or(|t| t.elapsed() >= Duration::from_millis(250))
        {
            q.reclaim = Some((window, obsolete, global));
            q.last_reclaim = Some(Instant::now());
            wake.notify_one();
        }
    }
    pub fn reconfigure(&self, fps: u32, bitrate: u32) {
        let (lock, wake) = &*self.queue;
        let mut queue = lock.lock().unwrap();
        queue.frames.clear();
        queue.bytes = 0;
        queue.observe();
        queue.config = Some((fps, bitrate));
        wake.notify_one();
    }
    pub fn shutdown(&mut self) {
        let Some(worker) = self.worker.take() else {
            return;
        };
        {
            let (lock, wake) = &*self.queue;
            let mut q = lock.lock().unwrap();
            q.closed = true;
            q.frames.clear();
            q.bytes = 0;
            q.observe();
            wake.notify_one();
        }
        // Release active and cached NVENC/CUDA sessions before EGL/driver teardown.
        // Joining while holding the queue mutex would deadlock an idle worker.
        if worker.join().is_err() {
            tracing::warn!("video worker panicked during shutdown");
        }
    }
    /// The compositor is the sole producer. A pending raw update already owns
    /// this window's slot; newer commits stay dirty until that slot is consumed.
    pub fn capture_ready(&self, id: u32, estimated_bytes: usize) -> bool {
        let q = self.queue.0.lock().unwrap();
        if q.closed {
            return false;
        }
        if q.frames.contains_key(&id) {
            metrics::increment("capture_wait_encoder");
            return false;
        }
        if !q.fits(estimated_bytes) {
            metrics::increment("capture_wait_budget");
            return false;
        }
        true
    }
    pub fn submit(&self, frame: Frame) -> bool {
        let (lock, wake) = &*self.queue;
        let mut q = lock.lock().unwrap();
        if q.closed {
            return false;
        }
        // CUDA pitch/alpha may exceed the RGBA admission estimate. Refuse this
        // capture and retain caller dirty; never evict another window's final frame.
        let replaced = q.frames.get(&frame.id).map_or(0, Frame::memory_bytes);
        if frame.memory_bytes() > MAX_RAW_BYTES - q.bytes + replaced {
            metrics::increment("raw_capture_retry_budget");
            return false;
        }
        q.bytes = q.bytes - replaced + frame.memory_bytes();
        q.frames.insert(frame.id, frame);
        q.observe();
        wake.notify_one();
        true
    }
    pub fn forget(&self, id: u32) {
        let (lock, wake) = &*self.queue;
        let mut q = lock.lock().unwrap();
        if let Some(frame) = q.frames.remove(&id) {
            q.bytes -= frame.memory_bytes();
        }
        q.observe();
        q.forget.push(id);
        wake.notify_one();
    }
    pub fn reset(&self) {
        let (lock, wake) = &*self.queue;
        let mut q = lock.lock().unwrap();
        q.frames.clear();
        q.bytes = 0;
        q.observe();
        q.reset = true;
        wake.notify_one();
    }
}
impl Drop for Encoder {
    fn drop(&mut self) {
        self.shutdown();
    }
}
fn png(
    bytes: &[u8],
    width: u32,
    height: u32,
    kind: image::ExtendedColorType,
) -> image::ImageResult<Vec<u8>> {
    let mut png = vec![];
    PngEncoder::new_with_quality(&mut png, CompressionType::Fast, FilterType::Adaptive)
        .write_image(bytes, width, height, kind)?;
    Ok(png)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn udp_video_share_reserves_headroom_and_caps_each_window() {
        assert_eq!(target_bitrate(30_000, None, 3), 30_000);
        assert_eq!(target_bitrate(30_000, Some(80_000), 1), 30_000);
        assert_eq!(target_bitrate(30_000, Some(80_000), 3), 20_000);
        assert_eq!(target_bitrate(30_000, Some(8_000), 1), 6_000);
        assert_eq!(target_bitrate(30_000, Some(8_000), 2), 3_000);
        assert_eq!(target_bitrate(30_000, Some(1_000), 3), 1_000);
        assert_eq!(target_bitrate(2_000, Some(20_000), 1), 2_000);
    }

    #[test]
    fn video_rate_changes_have_hysteresis_and_recover_in_steps() {
        let ms = Duration::from_millis;
        assert_eq!(next_bitrate(6_000, 4_500, ms(499)), 6_000);
        assert_eq!(next_bitrate(6_000, 4_500, ms(500)), 4_500);
        assert_eq!(next_bitrate(6_000, 5_950, ms(500)), 6_000);
        assert_eq!(next_bitrate(6_000, 5_950, ms(2_000)), 5_950);
        assert_eq!(next_bitrate(4_500, 6_000, ms(1_999)), 4_500);
        assert_eq!(next_bitrate(4_500, 6_000, ms(2_000)), 5_400);
        assert_eq!(next_bitrate(5_400, 6_000, ms(2_000)), 6_000);
        assert_eq!(next_bitrate(1_000, 3_000, ms(2_000)), 1_500);
    }
}
