//! Bounded raw-frame queue. Dependent H.264 packets are never discarded after encoding.
use crate::{metrics, network::Peer, protocol};
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
    pub peer: Arc<Peer>,
    pub video_eligible: bool,
    pub enqueued: Instant,
}
#[derive(Default)]
struct Queue {
    frames: BTreeMap<u32, Frame>,
    bytes: usize,
    last_id: u32,
    forget: Vec<u32>,
    reset: bool,
}
pub struct Encoder {
    queue: Arc<(Mutex<Queue>, Condvar)>,
}
struct Session {
    width: u32,
    height: u32,
    codec: nvenc::Codec,
    owner: Weak<Peer>,
    encoder: Option<nvenc::Encoder>,
    failed: bool,
    epoch: u32,
    force_idr: bool,
}

// Bound inactive GPU sessions globally, rather than reserving two for every window.
// The pixel budget bounds cached resolutions; driver allocations also include codec surfaces.
const MAX_IDLE_ENCODERS: usize = 2;
const MAX_IDLE_PIXELS: u64 = 4 * 1024 * 1024;

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
    pub fn new(codec: Option<nvenc::Codec>, fps: u32, bitrate: u32) -> Self {
        let queue = Arc::new((Mutex::new(Queue::default()), Condvar::new()));
        let worker = queue.clone();
        thread::spawn(move || {
            let mut sessions: BTreeMap<u32, Session> = BTreeMap::new();
            let mut idle = VecDeque::<(u32, Session)>::new();
            let mut epoch = 0u32;
            loop {
                let (frame, reset, forget) = {
                    let (lock, wake) = &*worker;
                    let mut q = lock.lock().unwrap();
                    while q.frames.is_empty() && !q.reset && q.forget.is_empty() {
                        q = wake.wait(q).unwrap();
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
                        q.bytes -= frame.rgba.len();
                        frame
                    });
                    (frame, reset, forget)
                };
                if reset {
                    sessions.clear();
                    idle.clear();
                }
                for id in forget {
                    sessions.remove(&id);
                    idle.retain(|(cached_id, _)| *cached_id != id);
                }
                let Some(mut frame) = frame else { continue };
                if frame.peer.is_closed() || frame.peer.window_closed(frame.id) {
                    continue;
                }
                let selected_codec = codec.unwrap_or(nvenc::Codec::H264);
                let use_video = codec.is_some()
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
                    if !q.frames.contains_key(&frame.id) {
                        q.bytes += frame.rgba.len();
                        q.frames.insert(frame.id, frame);
                    }
                    let _ = wake.wait_timeout(q, Duration::from_millis(2)).unwrap();
                    continue;
                }
                let mut queue_wait = frame.enqueued.elapsed();
                if use_video {
                    let same_owner = |s: &Session| {
                        s.owner
                            .upgrade()
                            .is_some_and(|peer| Arc::ptr_eq(&peer, &frame.peer))
                    };
                    let recreate = sessions.get(&frame.id).is_none_or(|s| {
                        s.width != frame.width
                            || s.height != frame.height
                            || s.codec != selected_codec
                            || !same_owner(s)
                    });
                    if recreate {
                        epoch = epoch.wrapping_add(1);
                        let cached = idle.iter().position(|(id, s)| {
                            *id == frame.id
                                && s.width == frame.width
                                && s.height == frame.height
                                && s.codec == selected_codec
                                && same_owner(s)
                        });
                        let mut next = cached.map(|index| idle.remove(index).unwrap().1);
                        if let Some(previous) = sessions.remove(&frame.id) {
                            if previous.encoder.is_some() && same_owner(&previous) {
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
                            owner: Arc::downgrade(&frame.peer),
                            encoder: None,
                            failed: false,
                            epoch,
                            force_idr: true,
                        });
                        next.epoch = epoch;
                        next.force_idr = true;
                        sessions.insert(frame.id, next);
                    }
                    let session = sessions.get_mut(&frame.id).unwrap();
                    if !session.failed && session.encoder.is_none() {
                        let started = Instant::now();
                        let mut result = nvenc::Encoder::with_codec(
                            selected_codec,
                            frame.width,
                            frame.height,
                            fps,
                            bitrate,
                        );
                        if result.is_err() && !idle.is_empty() {
                            // Cached sessions must never make another window lose hardware video.
                            idle.clear();
                            result = nvenc::Encoder::with_codec(
                                selected_codec,
                                frame.width,
                                frame.height,
                                fps,
                                bitrate,
                            );
                        }
                        metrics::record("encoder_init", started.elapsed());
                        match result {
                            Ok(encoder) => {
                                session.encoder = Some(encoder);
                                tracing::info!(
                                    id = frame.id,
                                    width = frame.width,
                                    height = frame.height,
                                    codec = selected_codec.name(),
                                    "NVENC ultra-low-latency encoder active"
                                );
                            }
                            Err(error) => {
                                session.failed = true;
                                tracing::warn!(id=frame.id,%error,"NVENC unavailable; using lossless PNG");
                            }
                        }
                    }
                    // A cold session can take >100 ms. Prefer the latest unencoded
                    // image, and never transmit an obsolete intermediate resize.
                    let (lock, _) = &*worker;
                    let mut q = lock.lock().unwrap();
                    if let Some(latest) = q.frames.get(&frame.id) {
                        if latest.width != frame.width
                            || latest.height != frame.height
                            || !Arc::ptr_eq(&latest.peer, &frame.peer)
                        {
                            continue;
                        }
                        let latest = q.frames.remove(&frame.id).unwrap();
                        q.bytes -= latest.rgba.len();
                        frame = latest;
                        queue_wait = frame.enqueued.elapsed();
                    }
                }
                metrics::record("encode_queue", queue_wait);
                let conversion = Instant::now();
                let transparent = frame.rgba.chunks_exact(4).any(|p| p[3] != 255);
                let alpha = if use_video && transparent {
                    frame.rgba.chunks_exact(4).map(|p| p[3]).collect::<Vec<_>>()
                } else {
                    vec![]
                };
                if transparent {
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
                }
                metrics::record("unpremultiply", conversion.elapsed());
                if use_video {
                    let session = sessions.get_mut(&frame.id).unwrap();
                    if let Some(encoder) = session.encoder.as_mut() {
                        let started = Instant::now();
                        match encoder.encode(&frame.rgba, frame.sequence, session.force_idr) {
                            Ok((color, key)) => {
                                session.force_idr = false;
                                metrics::record("nvenc", started.elapsed());
                                let mask = if alpha.is_empty() {
                                    vec![]
                                } else {
                                    png(
                                        &alpha,
                                        frame.width,
                                        frame.height,
                                        image::ExtendedColorType::L8,
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
                                frame.peer.video_frame(frame.id, selected_codec, body);
                                continue;
                            }
                            Err(error) => {
                                tracing::warn!(id=frame.id,%error,"NVENC failed; falling back to PNG");
                                session.encoder = None;
                                session.failed = true;
                            }
                        }
                    }
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
                        frame.peer.frame(frame.id, bytes);
                    }
                }
            }
        });
        Self { queue }
    }
    pub fn submit(&self, frame: Frame) {
        let (lock, wake) = &*self.queue;
        let mut q = lock.lock().unwrap();
        if let Some(old) = q.frames.remove(&frame.id) {
            q.bytes -= old.rgba.len();
        }
        while q.bytes + frame.rgba.len() > 128 * 1024 * 1024 {
            let Some((_, old)) = q.frames.pop_first() else {
                return;
            };
            q.bytes -= old.rgba.len();
        }
        q.bytes += frame.rgba.len();
        q.frames.insert(frame.id, frame);
        wake.notify_one();
    }
    pub fn forget(&self, id: u32) {
        let (lock, wake) = &*self.queue;
        let mut q = lock.lock().unwrap();
        if let Some(frame) = q.frames.remove(&id) {
            q.bytes -= frame.rgba.len();
        }
        q.forget.push(id);
        wake.notify_one();
    }
    pub fn reset(&self) {
        let (lock, wake) = &*self.queue;
        let mut q = lock.lock().unwrap();
        q.frames.clear();
        q.bytes = 0;
        q.reset = true;
        wake.notify_one();
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
