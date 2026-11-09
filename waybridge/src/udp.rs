//! Optional authenticated, paced UDP media with 8+1 XOR shards and TCP repair.
use crate::protocol;
use anyhow::Result;
use base64::{engine::general_purpose::STANDARD, Engine};
use ring::{
    aead,
    rand::{SecureRandom, SystemRandom},
};
use serde_json::{json, Value};
use std::{
    collections::BTreeMap,
    io,
    net::{SocketAddr, TcpStream, UdpSocket},
    sync::{Arc, Mutex, Weak},
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
#[derive(Default)]
struct Cache {
    next: u64,
    bytes: usize,
    frames: BTreeMap<u64, Cached>,
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
    pace: Duration,
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
            pace: Duration::from_secs_f64(1100.0 * 8.0 / (f64::from(self.mbps) * 1_000_000.0)),
        });
        *self.current.lock().unwrap() = Arc::downgrade(&session);
        Ok(session)
    }
}
impl Transport {
    pub fn offer(&self) -> Value {
        self.offer.clone()
    }
    fn bind(&self, packet: &[u8], address: SocketAddr) {
        // The only client datagram is a 1-byte encrypted HELLO/keepalive. Authenticate before reply/allocation.
        if packet.len() != 37
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
    pub fn ready(&self) -> bool {
        self.endpoint.lock().unwrap().is_some()
    }
    pub fn ack(&self, message: u64) {
        let mut c = self.cache.lock().unwrap();
        if message > c.next {
            return;
        }
        let done: Vec<_> = c.frames.range(..=message).map(|(n, _)| *n).collect();
        for n in done {
            if let Some(v) = c.frames.remove(&n) {
                c.bytes -= v.body.len();
            }
        }
    }
    pub fn retry(&self, message: u64) -> Option<Vec<u8>> {
        let mut c = self.cache.lock().unwrap();
        let v = c.frames.get_mut(&message)?;
        if v.retry.elapsed() < Duration::from_millis(50) {
            return None;
        }
        v.retry = Instant::now();
        Some(repair(message, v.kind, &v.body))
    }
    pub fn send(&self, kind: u8, body: &[u8], tcp: &mut TcpStream) -> io::Result<()> {
        let (address, alive) = self
            .endpoint
            .lock()
            .unwrap()
            .map(|(a, t, _)| (a, t.elapsed() < Duration::from_secs(3)))
            .unwrap();
        let (message, use_udp) = {
            let mut c = self.cache.lock().unwrap();
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
            return protocol::write_packet(tcp, REPAIR, &repair(message, kind, body));
        }
        let count = body.len().div_ceil(CHUNK);
        let mut next = Instant::now();
        let mut send = |typ: u8, index: usize, payload: &[u8]| -> io::Result<()> {
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
            if let Some(delay) = next.checked_duration_since(Instant::now()) {
                thread::sleep(delay);
            }
            self.socket.send_to(&wire, address)?;
            next += self.pace;
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
                    return protocol::write_packet(tcp, REPAIR, &repair(message, kind, body));
                }
            }
            if send(2, first, &parity).is_err() {
                return protocol::write_packet(tcp, REPAIR, &repair(message, kind, body));
            }
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn authenticate_endpoint_and_reject_replay_or_rebinding() {
        let t = Transport {
            socket: Arc::new(UdpSocket::bind("127.0.0.1:0").unwrap()),
            session: 17,
            tx: key(&[1; 32]),
            rx: key(&[2; 32]),
            offer: Value::Null,
            endpoint: Mutex::new(None),
            counter: Mutex::new(0),
            cache: Mutex::new(Cache::default()),
            pace: Duration::ZERO,
        };
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
    fn keys_and_sessions_are_fresh() {
        let hub = Hub::new("127.0.0.1:0", 200).unwrap();
        let a = hub.session().unwrap();
        let b = hub.session().unwrap();
        assert_ne!(a.offer()["session"], b.offer()["session"]);
        assert_ne!(a.offer()["tx_key"], b.offer()["tx_key"]);
        assert_ne!(a.offer()["rx_key"], a.offer()["tx_key"]);
    }
}
