//! Optional authenticated, paced UDP media with 8+1 XOR shards and TCP repair.
use crate::{metrics, protocol};
#[path = "udp_pacing.rs"]
mod pacing;
use anyhow::Result;
use base64::{engine::general_purpose::STANDARD, Engine};
use pacing::{Change, Pacing};
use ring::{
    aead,
    rand::{SecureRandom, SystemRandom},
};
use serde_json::{json, Value};
use std::{
    collections::BTreeMap,
    io,
    net::{SocketAddr, TcpStream, UdpSocket},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex, Weak,
    },
    thread,
    time::{Duration, Instant},
};

const CHUNK: usize = 1024;
const GROUP: usize = 8;
const MAX_FRAME: usize = 8 * 1024 * 1024;
const MAX_CACHE: usize = 32 * 1024 * 1024;
pub const REPAIR: u8 = 6;

fn key(bytes: &[u8]) -> aead::LessSafeKey {
    aead::LessSafeKey::new(aead::UnboundKey::new(&aead::AES_256_GCM, bytes).unwrap())
}
fn nonce(counter: u64) -> aead::Nonce {
    let mut n = [0; 12];
    n[4..].copy_from_slice(&counter.to_be_bytes());
    aead::Nonce::assume_unique_for_key(n)
}
fn header(session: u64, counter: u64) -> Vec<u8> {
    let mut out = b"WBU1".to_vec();
    out.extend(session.to_be_bytes());
    out.extend(counter.to_be_bytes());
    out
}
pub fn repair(message: u64, kind: u8, body: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(9 + body.len());
    out.extend(message.to_be_bytes());
    out.push(kind);
    out.extend(body);
    out
}
struct Cached {
    kind: u8,
    body: Vec<u8>,
    retry: Instant,
}
struct Pending {
    bytes: usize,
    data_boundary: Option<Instant>,
    udp: bool,
    repaired: bool,
}
#[derive(Default)]
struct Cache {
    next: u64,
    bytes: usize,
    frames: BTreeMap<u64, Cached>,
    pending: BTreeMap<u64, Pending>,
    pending_bytes: usize,
}
impl Cache {
    fn can_send(&self, size: usize) -> bool {
        self.pending.len() < 8
            && (self.pending.is_empty() || self.pending_bytes + size <= 64 * 1024 * 1024)
    }
}
pub struct Transport {
    socket: Arc<UdpSocket>,
    session: u64,
    tx: aead::LessSafeKey,
    rx: aead::LessSafeKey,
    offer: Value,
    endpoint: Mutex<Option<(SocketAddr, Instant, u64)>>,
    counter: Mutex<u64>,
    cache: Mutex<Cache>,
    pacing: Mutex<Pacing>,
    cancelled: AtomicBool,
}
pub struct Hub {
    socket: Arc<UdpSocket>,
    current: Arc<Mutex<Weak<Transport>>>,
    mbps: u32,
}
impl Hub {
    pub fn new(address: &str, mbps: u32) -> Result<Arc<Self>> {
        let socket = Arc::new(UdpSocket::bind(address)?);
        socket.set_write_timeout(Some(Duration::from_secs(1)))?;
        let current = Arc::new(Mutex::new(Weak::<Transport>::new()));
        let (reader, sessions) = (socket.clone(), current.clone());
        thread::spawn(move || {
            let mut packet = [0; 1201];
            while let Ok((n, address)) = reader.recv_from(&mut packet) {
                let session = sessions.lock().unwrap().upgrade();
                if let Some(session) = session {
                    session.bind(&packet[..n], address);
                }
            }
        });
        tracing::info!(address=%socket.local_addr()?, mbps, "encrypted UDP/FEC listener ready");
        Ok(Arc::new(Self {
            socket,
            current,
            mbps,
        }))
    }
    pub fn session(&self) -> Result<Arc<Transport>> {
        let mut random = [0; 72];
        SystemRandom::new()
            .fill(&mut random)
            .map_err(|_| io::Error::other("UDP key generation"))?;
        let id = u64::from_be_bytes(random[..8].try_into().unwrap());
        let session = Arc::new(Transport {
            socket: self.socket.clone(),
            session: id,
            tx: key(&random[8..40]),
            rx: key(&random[40..]),
            offer: json!({"version":1,"session":id.to_string(),"port":self.socket.local_addr()?.port(),"tx_key":STANDARD.encode(&random[8..40]),"rx_key":STANDARD.encode(&random[40..]),"chunk":CHUNK,"group":GROUP}),
            endpoint: Mutex::new(None),
            counter: Mutex::new(0),
            cache: Mutex::new(Cache::default()),
            cancelled: AtomicBool::new(false),
            pacing: Mutex::new(Pacing::new(self.mbps, Instant::now())),
        });
        let mut current = self.current.lock().unwrap();
        if let Some(old) = current.upgrade() {
            old.cancel();
        }
        *current = Arc::downgrade(&session);
        Ok(session)
    }
}
impl Transport {
    pub fn offer(&self) -> Value {
        self.offer.clone()
    }
    fn bind(&self, packet: &[u8], address: SocketAddr) {
        // The only client datagram is a 1-byte encrypted HELLO/keepalive. Authenticate before reply/allocation.
        if self.cancelled.load(Ordering::Relaxed)
            || packet.len() != 37
            || &packet[..4] != b"WBU1"
            || packet[4..12] != self.session.to_be_bytes()
        {
            return;
        }
        let count = u64::from_be_bytes(packet[12..20].try_into().unwrap());
        if count == 0 {
            return;
        }
        let mut body = packet[20..].to_vec();
        if !matches!(
            self.rx
                .open_in_place(nonce(count), aead::Aad::from(&packet[..20]), &mut body),
            Ok([0])
        ) {
            return;
        }
        let mut endpoint = self.endpoint.lock().unwrap();
        if let Some((old, _, last)) = *endpoint {
            if old != address || count <= last {
                return;
            }
        }
        *endpoint = Some((address, Instant::now(), count));
    }
    pub fn cancel(&self) {
        self.cancelled.store(true, Ordering::Relaxed);
    }
    fn check_cancelled(&self) -> io::Result<()> {
        if self.cancelled.load(Ordering::Relaxed) {
            Err(io::Error::new(
                io::ErrorKind::ConnectionAborted,
                "UDP session ended",
            ))
        } else {
            Ok(())
        }
    }
    pub fn can_send(&self, size: usize) -> bool {
        self.cache.lock().unwrap().can_send(size)
    }
    pub fn ready(&self) -> bool {
        self.endpoint.lock().unwrap().is_some()
    }
    fn observe_pacing(&self, change: Option<Change>) {
        metrics::gauge(
            "udp_pacing_kbps",
            self.pacing.lock().unwrap().kbps() as usize,
        );
        if let Some(reason) = change {
            metrics::increment(match reason {
                Change::Repair => "udp_pacing_backoff_repair",
                Change::Delay => "udp_pacing_backoff_delay",
                Change::Recovery => "udp_pacing_recovery",
            });
        }
    }
    fn delivery_boundary(&self, message: u64, udp: bool) {
        if let Some(pending) = self.cache.lock().unwrap().pending.get_mut(&message) {
            pending.data_boundary = Some(Instant::now());
            pending.udp = udp;
        }
        self.observe_pacing(None);
    }
    pub fn ack(&self, message: u64) {
        let now = Instant::now();
        let sample = {
            let mut c = self.cache.lock().unwrap();
            let Some(acked) = c.pending.get(&message) else {
                return;
            };
            // Train only from UDP delivery with no requested repair. TCP repair,
            // replayed/unknown ACKs and early ACKs cannot manufacture healthy feedback.
            let sample = acked
                .data_boundary
                .filter(|sent| acked.udp && !acked.repaired && *sent <= now)
                .map(|sent| now.duration_since(sent));
            let pending: Vec<_> = c.pending.range(..=message).map(|(n, _)| *n).collect();
            for n in pending {
                if let Some(pending) = c.pending.remove(&n) {
                    c.pending_bytes -= pending.bytes;
                }
            }
            let done: Vec<_> = c.frames.range(..=message).map(|(n, _)| *n).collect();
            for n in done {
                if let Some(v) = c.frames.remove(&n) {
                    c.bytes -= v.body.len();
                }
            }
            metrics::gauge("udp_pending_messages", c.pending.len());
            metrics::gauge("udp_pending_bytes", c.pending_bytes);
            sample
        };
        if let Some(delivery) = sample {
            metrics::record("udp_delivery_ack", delivery);
            let change = self.pacing.lock().unwrap().acknowledge(now, delivery);
            self.observe_pacing(change);
        }
    }
    pub fn retry(&self, message: u64) -> Option<Vec<u8>> {
        if self.cancelled.load(Ordering::Relaxed) {
            return None;
        }
        let now = Instant::now();
        let (repair, penalize) = {
            let mut c = self.cache.lock().unwrap();
            let v = c.frames.get_mut(&message)?;
            if v.retry.elapsed() < Duration::from_millis(50) {
                return None;
            }
            v.retry = now;
            let repair = repair(message, v.kind, &v.body);
            let pending = c.pending.get_mut(&message)?;
            // The receiver can request the next message before its paced send
            // finishes. Still repair it, but do not call that packet loss.
            let penalize = pending.data_boundary.is_some_and(|sent| sent <= now)
                && pending.udp
                && !pending.repaired;
            pending.repaired = true;
            (repair, penalize)
        };
        metrics::increment("udp_tcp_repair_requests");
        if penalize {
            let change = self.pacing.lock().unwrap().repair(now);
            self.observe_pacing(change);
        }
        Some(repair)
    }
    pub fn send(&self, kind: u8, body: &[u8], tcp: &mut TcpStream) -> io::Result<()> {
        self.check_cancelled()?;
        let (address, alive) = self
            .endpoint
            .lock()
            .unwrap()
            .map(|(a, t, _)| (a, t.elapsed() < Duration::from_secs(3)))
            .unwrap();
        let (message, use_udp) = {
            let mut c = self.cache.lock().unwrap();
            if !c.can_send(body.len()) {
                return Err(io::Error::new(
                    io::ErrorKind::WouldBlock,
                    "UDP global credits exhausted",
                ));
            }
            c.next = c
                .next
                .checked_add(1)
                .ok_or_else(|| io::Error::other("UDP message sequence exhausted"))?;
            let message = c.next;
            let use_udp = alive
                && !body.is_empty()
                && body.len() <= MAX_FRAME
                && c.frames.len() < 16
                && c.bytes + body.len() <= MAX_CACHE;
            c.pending.insert(
                message,
                Pending {
                    bytes: body.len(),
                    data_boundary: None,
                    udp: use_udp,
                    repaired: false,
                },
            );
            c.pending_bytes += body.len();
            metrics::gauge("udp_pending_messages", c.pending.len());
            metrics::gauge("udp_pending_bytes", c.pending_bytes);
            if use_udp {
                c.bytes += body.len();
                c.frames.insert(
                    message,
                    Cached {
                        kind,
                        body: body.to_vec(),
                        retry: Instant::now() - Duration::from_secs(1),
                    },
                );
            }
            (message, use_udp)
        };
        if !use_udp {
            self.check_cancelled()?;
            protocol::write_packet(tcp, REPAIR, &repair(message, kind, body))?;
            self.delivery_boundary(message, false);
            return Ok(());
        }
        let count = body.len().div_ceil(CHUNK);
        let send = |typ: u8, index: usize, payload: &[u8]| -> io::Result<()> {
            self.check_cancelled()?;
            let mut plain = Vec::with_capacity(20 + payload.len());
            plain.extend([typ, kind]);
            plain.extend((index as u16).to_be_bytes());
            plain.extend((count as u16).to_be_bytes());
            plain.extend([0, 0]);
            plain.extend((body.len() as u32).to_be_bytes());
            plain.extend(message.to_be_bytes());
            plain.extend(payload);
            let counter = {
                let mut c = self.counter.lock().unwrap();
                *c = c
                    .checked_add(1)
                    .ok_or_else(|| io::Error::other("UDP nonce exhausted"))?;
                *c
            };
            let mut wire = header(self.session, counter);
            self.tx
                .seal_in_place_append_tag(nonce(counter), aead::Aad::from(&wire), &mut plain)
                .map_err(|_| io::Error::other("UDP encrypt"))?;
            wire.extend(plain);
            // Include UDP and the base IP header; link/VPN encapsulation is
            // outside this socket's accounting.
            let wire_bytes = wire.len() + if address.is_ipv4() { 28 } else { 48 };
            let slot = self
                .pacing
                .lock()
                .unwrap()
                .reserve(Instant::now(), wire_bytes);
            if let Some(delay) = slot.checked_duration_since(Instant::now()) {
                thread::sleep(delay);
            }
            self.check_cancelled()?;
            if typ == 1 && index + 1 == count {
                // A healthy low-RTT receiver can ACK the final DATA before
                // trailing parity is sent. Publish the sample boundary before
                // exposing that DATA, or every such ACK can miss training.
                self.delivery_boundary(message, true);
            }
            self.socket.send_to(&wire, address)?;
            Ok(())
        };
        for first in (0..count).step_by(GROUP) {
            let mut parity = [0u8; CHUNK];
            for index in first..(first + GROUP).min(count) {
                let block = &body[index * CHUNK..((index + 1) * CHUNK).min(body.len())];
                for (a, b) in parity.iter_mut().zip(block) {
                    *a ^= b;
                }
                if send(1, index, block).is_err() {
                    self.check_cancelled()?;
                    // Disable UDP training before TCP can expose this repair.
                    self.delivery_boundary(message, false);
                    protocol::write_packet(tcp, REPAIR, &repair(message, kind, body))?;
                    metrics::increment("udp_tcp_send_fallback");
                    return Ok(());
                }
            }
            if send(2, first, &parity).is_err() {
                self.check_cancelled()?;
                // Disable UDP training before TCP can expose this repair.
                self.delivery_boundary(message, false);
                protocol::write_packet(tcp, REPAIR, &repair(message, kind, body))?;
                metrics::increment("udp_tcp_send_fallback");
                return Ok(());
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn transport() -> Transport {
        Transport {
            socket: Arc::new(UdpSocket::bind("127.0.0.1:0").unwrap()),
            session: 17,
            tx: key(&[1; 32]),
            rx: key(&[2; 32]),
            offer: Value::Null,
            endpoint: Mutex::new(None),
            counter: Mutex::new(0),
            cache: Mutex::new(Cache::default()),
            cancelled: AtomicBool::new(false),
            pacing: Mutex::new(Pacing::new(200, Instant::now())),
        }
    }
    #[test]
    fn authenticate_endpoint_and_reject_replay_or_rebinding() {
        let t = transport();
        let make = |number, secret: &[u8]| {
            let mut h = header(17, number);
            let mut p = vec![0];
            key(secret)
                .seal_in_place_append_tag(nonce(number), aead::Aad::from(&h), &mut p)
                .unwrap();
            h.extend(p);
            h
        };
        let address = "127.0.0.1:10001".parse().unwrap();
        for packet in [make(1, &[3; 32]), make(0, &[2; 32]), vec![0; 1201]] {
            t.bind(&packet, address);
            assert!(!t.ready());
        }
        let first = make(1, &[2; 32]);
        let mut corrupt = first.clone();
        corrupt[36] ^= 1;
        t.bind(&corrupt, address);
        assert!(!t.ready());
        t.bind(&first, address);
        assert!(t.ready());
        let old = t.endpoint.lock().unwrap().unwrap();
        t.bind(&first, address);
        assert_eq!(t.endpoint.lock().unwrap().unwrap(), old);
        let second = make(2, &[2; 32]);
        t.bind(&second, "127.0.0.1:10002".parse().unwrap());
        assert_eq!(t.endpoint.lock().unwrap().unwrap(), old);
        t.bind(&second, address);
        assert_eq!(t.endpoint.lock().unwrap().unwrap().2, 2);
    }
    #[test]
    fn only_live_unrepaired_udp_feedback_trains_pacing() {
        let t = transport();
        t.pacing
            .lock()
            .unwrap()
            .repair(Instant::now() - Duration::from_secs(2));
        assert_eq!(t.pacing.lock().unwrap().kbps(), 150_000);
        for message in 1..=4 {
            assert!(t.retry(message).is_none());
            t.ack(message);
        }
        assert_eq!(t.pacing.lock().unwrap().kbps(), 150_000);
        for message in 1..=4 {
            let mut cache = t.cache.lock().unwrap();
            cache.pending.insert(
                message,
                Pending {
                    bytes: 4,
                    data_boundary: Some(Instant::now() - Duration::from_millis(20)),
                    udp: true,
                    repaired: false,
                },
            );
            cache.pending_bytes += 4;
            drop(cache);
            t.ack(message);
        }
        assert_eq!(t.pacing.lock().unwrap().kbps(), 157_500);
        t.ack(4);
        assert_eq!(t.pacing.lock().unwrap().kbps(), 157_500);
        let mut cache = t.cache.lock().unwrap();
        cache.pending.insert(
            5,
            Pending {
                bytes: 4,
                data_boundary: None,
                udp: true,
                repaired: false,
            },
        );
        cache.pending_bytes += 4;
        cache.frames.insert(
            5,
            Cached {
                kind: 2,
                body: vec![1, 2, 3, 4],
                retry: Instant::now() - Duration::from_secs(1),
            },
        );
        cache.bytes += 4;
        drop(cache);
        assert_eq!(t.retry(5).unwrap(), repair(5, 2, &[1, 2, 3, 4]));
        assert_eq!(t.pacing.lock().unwrap().kbps(), 157_500);
        assert!(t.retry(5).is_none());
        t.delivery_boundary(5, true);
        t.ack(5);
        assert_eq!(t.pacing.lock().unwrap().kbps(), 157_500);
        assert_eq!(t.cache.lock().unwrap().pending_bytes, 0);
        assert_eq!(t.cache.lock().unwrap().bytes, 0);
        // An ACK may enter before send completion but acquire the cache lock
        // afterwards. A completion timestamp newer than ACK entry is still early.
        let early = transport();
        early
            .pacing
            .lock()
            .unwrap()
            .repair(Instant::now() - Duration::from_secs(2));
        for message in 1..=4 {
            let mut cache = early.cache.lock().unwrap();
            cache.pending.insert(
                message,
                Pending {
                    bytes: 4,
                    data_boundary: Some(if message == 4 {
                        Instant::now() + Duration::from_secs(1)
                    } else {
                        Instant::now() - Duration::from_millis(20)
                    }),
                    udp: true,
                    repaired: false,
                },
            );
            cache.pending_bytes += 4;
            drop(cache);
            early.ack(message);
        }
        assert_eq!(early.pacing.lock().unwrap().kbps(), 150_000);
    }
    #[test]
    fn final_data_ack_trains_before_trailing_parity() {
        use std::net::TcpListener;
        let mut t = transport();
        // A sub-Mbit fixture gives the receiver a deterministic 500ms DATA to
        // parity gap without requiring CPU scheduler timing in microseconds.
        t.pacing = Mutex::new(Pacing::with_ceiling(16_000, Instant::now()));
        let receiver = UdpSocket::bind("127.0.0.1:0").unwrap();
        receiver
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        t.endpoint = Mutex::new(Some((receiver.local_addr().unwrap(), Instant::now(), 1)));
        let t = Arc::new(t);
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let mut tcp = TcpStream::connect(listener.local_addr().unwrap()).unwrap();
        let (_reader, _) = listener.accept().unwrap();
        let sender = t.clone();
        let worker = thread::spawn(move || sender.send(2, &[7; CHUNK], &mut tcp));
        let mut packet = [0; 1201];
        let (size, _) = receiver.recv_from(&mut packet).unwrap();
        let count = u64::from_be_bytes(packet[12..20].try_into().unwrap());
        let mut plain = packet[20..size].to_vec();
        let data = key(&[1; 32])
            .open_in_place(nonce(count), aead::Aad::from(&packet[..20]), &mut plain)
            .unwrap();
        assert_eq!(data[0], 1);
        assert_eq!(&data[20..], &[7; CHUNK]);
        let acked = Instant::now();
        assert!(t.cache.lock().unwrap().pending[&1]
            .data_boundary
            .is_some_and(|sent| sent <= acked));
        assert!(
            !worker.is_finished(),
            "parity should still be paced behind DATA"
        );
        t.ack(1);
        assert!(t.cache.lock().unwrap().pending.is_empty());
        assert_eq!(t.cache.lock().unwrap().pending_bytes, 0);
        worker.join().unwrap().unwrap();
        let (size, _) = receiver.recv_from(&mut packet).unwrap();
        let count = u64::from_be_bytes(packet[12..20].try_into().unwrap());
        let mut plain = packet[20..size].to_vec();
        let parity = key(&[1; 32])
            .open_in_place(nonce(count), aead::Aad::from(&packet[..20]), &mut plain)
            .unwrap();
        assert_eq!(parity[0], 2);
    }
    #[test]
    fn keys_and_sessions_are_fresh() {
        let hub = Hub::new("127.0.0.1:0", 200).unwrap();
        let a = hub.session().unwrap();
        let b = hub.session().unwrap();
        assert_ne!(a.offer()["session"], b.offer()["session"]);
        assert_ne!(a.offer()["tx_key"], b.offer()["tx_key"]);
        assert_ne!(a.offer()["rx_key"], a.offer()["tx_key"]);
    }
}
