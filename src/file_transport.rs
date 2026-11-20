//! Bounded, ordered file data transport. The control and media writers never
//! wait on this queue, socket or rate limiter.
use crate::{network::Peer, protocol, transfer_worker::Ingress};
use serde_json::{json, Value};
use std::{
    collections::VecDeque,
    io::{self, Read},
    net::{Shutdown, TcpStream},
    sync::{Arc, Condvar, Mutex},
    thread,
    time::{Duration, Instant},
};

const BYTE_LIMIT: usize = 4 * 1024 * 1024;
const DRAG_RESERVE: usize = 128 * 1024;
const PACKET_LIMIT: usize = 2 * 1024 * 1024;
const OPEN_TIMEOUT: Duration = Duration::from_secs(12);

#[derive(Clone, Copy, Eq, PartialEq)]
enum Transfer {
    Clipboard(u64),
    Drag(u64),
}

struct Packet {
    bytes: Vec<u8>,
    transfer: Transfer,
    payload: bool,
}

#[derive(Default)]
struct State {
    socket: Option<TcpStream>,
    queue: VecDeque<Packet>,
    bytes: usize,
    rate: u64,
    next: Option<Instant>,
    cancelled_clipboard: u64,
    closed: bool,
}

pub struct FileLane {
    state: Mutex<State>,
    changed: Condvar,
    opened: Instant,
}

impl FileLane {
    pub fn new() -> Self {
        Self {
            state: Mutex::new(State::default()),
            changed: Condvar::new(),
            opened: Instant::now(),
        }
    }

    pub fn push(&self, message: Value) -> bool {
        let transfer = if let Some(id) = message["batch"].as_u64() {
            Transfer::Clipboard(id)
        } else if let Some(id) = message["drag"].as_u64() {
            Transfer::Drag(id)
        } else {
            return false;
        };
        let payload = message["data"].is_string();
        let bytes = serde_json::to_vec(&message).unwrap();
        if bytes.len() >= PACKET_LIMIT {
            return false;
        }
        let mut state = self.state.lock().unwrap();
        let limit = if matches!(transfer, Transfer::Drag(_)) {
            BYTE_LIMIT
        } else {
            BYTE_LIMIT - DRAG_RESERVE
        };
        while !state.closed && bytes.len() > limit.saturating_sub(state.bytes) {
            if matches!(transfer, Transfer::Clipboard(id) if id <= state.cancelled_clipboard) {
                return false;
            }
            if state.socket.is_none() && self.opened.elapsed() >= OPEN_TIMEOUT {
                return false;
            }
            state = self
                .changed
                .wait_timeout(state, Duration::from_millis(100))
                .unwrap()
                .0;
        }
        if state.closed
            || state.socket.is_none() && self.opened.elapsed() >= OPEN_TIMEOUT
            || matches!(transfer, Transfer::Clipboard(id) if id <= state.cancelled_clipboard)
        {
            return false;
        }
        state.bytes += bytes.len();
        state.queue.push_back(Packet {
            bytes,
            transfer,
            payload,
        });
        self.changed.notify_all();
        true
    }

    pub fn clipboard_current(&self, batch: u64) -> bool {
        let state = self.state.lock().unwrap();
        !state.closed && batch > state.cancelled_clipboard
    }

    pub fn cancel_clipboard(&self, batch: u64) {
        let mut state = self.state.lock().unwrap();
        state.cancelled_clipboard = state.cancelled_clipboard.max(batch);
        state
            .queue
            .retain(|packet| !matches!(packet.transfer, Transfer::Clipboard(id) if id <= batch));
        state.bytes = state.queue.iter().map(|packet| packet.bytes.len()).sum();
        self.changed.notify_all();
    }

    pub fn set_rate(&self, kib: u64) {
        let mut state = self.state.lock().unwrap();
        let rate = if kib == 0 {
            0
        } else {
            kib.clamp(32, 1_048_576) * 1024
        };
        if state.rate != rate {
            state.rate = rate;
            state.next = None;
            self.changed.notify_all();
        }
    }

    pub fn is_closed(&self) -> bool {
        self.state.lock().unwrap().closed
    }

    pub fn close(&self) {
        let mut state = self.state.lock().unwrap();
        state.closed = true;
        state.queue.clear();
        state.bytes = 0;
        if let Some(socket) = state.socket.take() {
            let _ = socket.shutdown(Shutdown::Both);
        }
        self.changed.notify_all();
    }

    fn write(&self, mut socket: TcpStream) -> io::Result<()> {
        loop {
            let packet = {
                let mut state = self.state.lock().unwrap();
                loop {
                    if state.closed {
                        return Ok(());
                    }
                    let index = state
                        .queue
                        .iter()
                        .position(|packet| matches!(packet.transfer, Transfer::Drag(_)))
                        .unwrap_or(0);
                    let Some(packet) = state.queue.get(index) else {
                        state = self.changed.wait(state).unwrap();
                        continue;
                    };
                    let now = Instant::now();
                    if packet.payload && state.rate > 0 {
                        if let Some(next) = state.next.filter(|next| *next > now) {
                            state = self.changed.wait_timeout(state, next - now).unwrap().0;
                            continue;
                        }
                        state.next = Some(
                            now + Duration::from_secs_f64(
                                (packet.bytes.len() + 5) as f64 / state.rate as f64,
                            ),
                        );
                    }
                    let packet = state.queue.remove(index).unwrap();
                    state.bytes -= packet.bytes.len();
                    self.changed.notify_all();
                    break packet;
                }
            };
            protocol::write_packet(&mut socket, protocol::JSON, &packet.bytes)?;
        }
    }

    pub fn run(&self, mut socket: TcpStream, peer: Arc<Peer>, ingress: Ingress) {
        let Ok(writer) = socket.try_clone() else {
            return;
        };
        {
            let mut state = self.state.lock().unwrap();
            if state.closed || state.socket.is_some() {
                return;
            }
            let Ok(shutdown) = socket.try_clone() else {
                return;
            };
            state.socket = Some(shutdown);
            self.changed.notify_all();
        }
        let _ = socket.set_read_timeout(None);
        thread::scope(|scope| {
            scope.spawn(|| {
                let mut writer = writer;
                if protocol::write_packet(&mut writer, protocol::JSON, br#"{"type":"files_ready"}"#)
                    .is_ok()
                {
                    let _ = self.write(writer);
                }
                self.close();
            });
            let mut header = [0; 4];
            while socket.read_exact(&mut header).is_ok() {
                let size = u32::from_be_bytes(header) as usize;
                if !(2..=PACKET_LIMIT).contains(&size) {
                    break;
                }
                let mut body = vec![0; size];
                if socket.read_exact(&mut body).is_err() || body[0] != protocol::JSON {
                    break;
                }
                let Ok(message) = serde_json::from_slice::<Value>(&body[1..]) else {
                    break;
                };
                if !matches!(
                    message["type"].as_str(),
                    Some("tree_upload_chunk" | "tree_upload_end" | "tree_upload_barrier")
                ) || !ingress.submit_data(peer.clone(), message)
                {
                    break;
                }
            }
            self.close();
        });
        ingress.abort_data(peer.clone());
        peer.control(json!({"type":"file_channel_lost"}));
    }
}
