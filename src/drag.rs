//! Outbound COPY drags. A private, unmapped Wayland client is the drop target.
//! This keeps Smithay responsible for source negotiation and exactly one terminal
//! event; no cancelled source is resurrected to report a Windows drop as success.
use crate::{
    network::{Event, Peer},
    transfer,
};
use anyhow::{bail, Context, Result};
use base64::{engine::general_purpose::STANDARD as B64, Engine};
use serde_json::{json, Value};
use std::{
    io::Read,
    os::unix::net::UnixStream,
    sync::{
        atomic::{AtomicBool, AtomicUsize, Ordering},
        mpsc::{sync_channel, SyncSender},
        Arc,
    },
    time::Duration,
};
use wayland_client::{
    delegate_noop, event_created_child,
    protocol::{
        wl_compositor, wl_data_device, wl_data_device_manager, wl_data_offer, wl_registry, wl_seat,
        wl_surface,
    },
    Connection, Dispatch, EventQueue, QueueHandle, WEnum,
};

pub const MAX_READ: usize = 65536;
static WORKERS: AtomicUsize = AtomicUsize::new(0);
struct Worker;
impl Drop for Worker {
    fn drop(&mut self) {
        WORKERS.fetch_sub(1, Ordering::Relaxed);
    }
}
pub struct ReadRequest {
    pub request: u64,
    pub file: usize,
    pub offset: u64,
    pub length: usize,
}
pub struct Export {
    pub manifest: Vec<Value>,
    pub tree: bool,
    requests: SyncSender<ReadRequest>,
    cancelled: Arc<AtomicBool>,
}
impl Export {
    pub fn read(&self, request: ReadRequest) -> bool {
        self.requests.try_send(request).is_ok()
    }
}
impl Drop for Export {
    fn drop(&mut self) {
        self.cancelled.store(true, Ordering::Relaxed);
    }
}
#[cfg(test)]
pub fn windows_name(name: &str) -> bool {
    crate::file_tree::valid_name(name)
}

/// One bounded worker owns the opened source files and serves random range reads.
/// File descriptors pin identities; detected in-place changes fail the transfer.
pub fn prepare(
    id: u64,
    mut input: UnixStream,
    peer: Arc<Peer>,
    events: SyncSender<Event>,
    cancelled: Arc<AtomicBool>,
) -> bool {
    if WORKERS
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| {
            (n < 4).then_some(n + 1)
        })
        .is_err()
    {
        return false;
    }
    std::thread::spawn(move || {
        let _worker = Worker;
        let mut open = || -> Result<crate::file_tree::Source> {
            input.set_read_timeout(Some(Duration::from_millis(100)))?;
            let deadline = std::time::Instant::now() + Duration::from_secs(10);
            let mut bytes = Vec::new();
            let mut buffer = [0; 4096];
            loop {
                if cancelled.load(Ordering::Relaxed) || peer.is_closed() {
                    bail!("drag cancelled");
                }
                if std::time::Instant::now() >= deadline {
                    bail!("file URI request timed out");
                }
                match input.read(&mut buffer) {
                    Ok(0) => break,
                    Ok(n) => bytes.extend_from_slice(&buffer[..n]),
                    Err(e)
                        if matches!(
                            e.kind(),
                            std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                        ) =>
                    {
                        continue
                    }
                    Err(e) => return Err(e.into()),
                }
                if bytes.len() > 1024 * 1024 {
                    bail!("file URI list too large");
                }
            }
            if bytes.len() > 1024 * 1024 {
                bail!("file URI list too large");
            }
            let text = std::str::from_utf8(&bytes)?;
            let paths = text
                .lines()
                .filter(|s| !s.is_empty() && !s.starts_with('#'))
                .take(129)
                .map(|line| {
                    transfer::uri_path(line).context("only local UTF-8 file URIs are supported")
                })
                .collect::<Result<Vec<_>>>()?;
            crate::file_tree::Source::open(paths, peer.file_tree())
        };
        let files = match open() {
            Ok(files) => files,
            Err(e) => {
                let _ = events.send(Event::DragFiles {
                    id,
                    result: Err(e.to_string()),
                });
                return;
            }
        };
        let (requests, receive) = sync_channel::<ReadRequest>(8);
        let tree = files.is_tree();
        let export = Export {
            manifest: if tree {
                files
                    .entries
                    .iter()
                    .map(|e| serde_json::to_value(e).unwrap())
                    .collect()
            } else {
                files.legacy_metadata()
            },
            tree,
            requests,
            cancelled: cancelled.clone(),
        };
        if events
            .send(Event::DragFiles {
                id,
                result: Ok(export),
            })
            .is_err()
        {
            return;
        }
        while !cancelled.load(Ordering::Relaxed) && !peer.is_closed() {
            let request = match receive.recv_timeout(Duration::from_millis(100)) {
                Ok(r) => r,
                Err(std::sync::mpsc::RecvTimeoutError::Timeout) => continue,
                Err(_) => break,
            };
            let run = || files.read(request.file, request.offset, request.length);
            let response = match run() {
                Ok(bytes) => {
                    json!({"type":"drag_data","drag":id,"request":request.request,"data":B64.encode(bytes)})
                }
                Err(error) => {
                    json!({"type":"drag_data","drag":id,"request":request.request,"error":error.to_string()})
                }
            };
            if cancelled.load(Ordering::Relaxed) || !peer.bulk(response) {
                break;
            }
        }
    });
    true
}

/// A private client over a socket pair, never connected to the host desktop.
/// It has one roleless surface, no buffers and no xdg_toplevel.
pub struct Sink {
    queue: EventQueue<SinkState>,
    state: SinkState,
}
#[derive(Default)]
struct SinkState {
    manager: Option<wl_data_device_manager::WlDataDeviceManager>,
    seat: Option<wl_seat::WlSeat>,
    device: Option<wl_data_device::WlDataDevice>,
    offer: Option<wl_data_offer::WlDataOffer>,
    expected_serial: Option<u32>,
    accepted: bool,
    dropped: bool,
    result: Option<bool>,
}
impl Sink {
    pub fn new(socket: UnixStream) -> Result<Self> {
        let connection = Connection::from_socket(socket)?;
        let queue = connection.new_event_queue::<SinkState>();
        connection.display().get_registry(&queue.handle(), ());
        connection.flush()?;
        Ok(Self {
            queue,
            state: SinkState::default(),
        })
    }
    pub fn arm(&mut self, serial: u32) {
        self.state.expected_serial = Some(serial);
    }
    pub fn accepted(&self) -> bool {
        self.state.accepted
    }
    pub fn finish(&mut self, success: bool) {
        if !success {
            self.state.expected_serial = None;
        }
        self.state.result = Some(success);
        self.state.finish();
    }
    pub fn pump(&mut self) -> Result<()> {
        self.queue.dispatch_pending(&mut self.state)?;
        if let Some(read) = self.queue.prepare_read() {
            match read.read() {
                Ok(_) => {}
                Err(wayland_client::backend::WaylandError::Io(e))
                    if e.kind() == std::io::ErrorKind::WouldBlock => {}
                Err(e) => return Err(e.into()),
            }
        }
        self.queue.dispatch_pending(&mut self.state)?;
        self.queue.flush()?;
        Ok(())
    }
}
impl SinkState {
    fn finish(&mut self) {
        if let Some(success) = self.result {
            // Success waits for the actual wl_data_device.drop; cancellation
            // before drop is handled by the compositor moving off this surface.
            if success && !self.dropped {
                return;
            }
            if let Some(offer) = self.offer.take() {
                if success {
                    offer.finish();
                }
                offer.destroy();
            }
            self.result = None;
            self.expected_serial = None;
            self.accepted = false;
            self.dropped = false;
        }
    }
}
impl Dispatch<wl_registry::WlRegistry, ()> for SinkState {
    fn event(
        state: &mut Self,
        registry: &wl_registry::WlRegistry,
        event: wl_registry::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wl_registry::Event::Global {
            name,
            interface,
            version,
        } = event
        {
            match interface.as_str() {
                "wl_compositor" => {
                    let c = registry.bind::<wl_compositor::WlCompositor, _, _>(
                        name,
                        version.min(1),
                        qh,
                        (),
                    );
                    c.create_surface(qh, ());
                }
                "wl_seat" if state.seat.is_none() => {
                    state.seat = Some(registry.bind(name, version.min(3), qh, ()))
                }
                "wl_data_device_manager" => {
                    state.manager = Some(registry.bind(name, version.min(3), qh, ()))
                }
                _ => {}
            }
            if state.device.is_none() {
                if let (Some(manager), Some(seat)) = (&state.manager, &state.seat) {
                    state.device = Some(manager.get_data_device(seat, qh, ()));
                }
            }
        }
    }
}
impl Dispatch<wl_data_device::WlDataDevice, ()> for SinkState {
    fn event(
        state: &mut Self,
        _: &wl_data_device::WlDataDevice,
        event: wl_data_device::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            wl_data_device::Event::Enter {
                serial,
                id: Some(offer),
                ..
            } => {
                // Cancellation can race the queued enter. Never negotiate an
                // old offer after its source has already been cancelled.
                if state.expected_serial != Some(serial) {
                    offer.destroy();
                    return;
                }
                if let Some(old) = state.offer.take() {
                    old.destroy();
                }
                state.accepted = false;
                state.dropped = false;
                state.result = None;
                offer.accept(serial, Some("text/uri-list".into()));
                offer.set_actions(
                    wl_data_device_manager::DndAction::Copy,
                    wl_data_device_manager::DndAction::Copy,
                );
                state.offer = Some(offer);
            }
            wl_data_device::Event::Drop => {
                state.dropped = true;
                state.finish();
            }
            wl_data_device::Event::Leave if !state.dropped => {
                if let Some(offer) = state.offer.take() {
                    offer.destroy();
                }
                state.accepted = false;
            }
            wl_data_device::Event::Selection { id: Some(offer) } => offer.destroy(),
            _ => {}
        }
    }
    event_created_child!(SinkState, wl_data_device::WlDataDevice, [0 => (wl_data_offer::WlDataOffer, ())]);
}
impl Dispatch<wl_data_offer::WlDataOffer, ()> for SinkState {
    fn event(
        state: &mut Self,
        offer: &wl_data_offer::WlDataOffer,
        event: wl_data_offer::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if state.offer.as_ref() == Some(offer) {
            if let wl_data_offer::Event::Action {
                dnd_action: WEnum::Value(wl_data_device_manager::DndAction::Copy),
            } = event
            {
                state.accepted = true;
            }
        }
    }
}
delegate_noop!(SinkState: ignore wl_compositor::WlCompositor);
delegate_noop!(SinkState: ignore wl_surface::WlSurface);
delegate_noop!(SinkState: ignore wl_seat::WlSeat);
delegate_noop!(SinkState: ignore wl_data_device_manager::WlDataDeviceManager);

#[cfg(test)]
mod tests {
    #[test]
    fn reject_windows_special_names() {
        for name in [
            "../x",
            "x.",
            "x ",
            "CON",
            "con.txt",
            "LPT1",
            "COM².log",
            "CON .txt",
            "a*b",
            "a?b",
        ] {
            assert!(!super::windows_name(name), "{name}");
        }
        assert!(super::windows_name("图片 #1.txt"));
    }
}
