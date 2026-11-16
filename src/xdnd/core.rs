//! An asynchronous COPY-only XDND source on the private Xwayland server.
//! Every drag has its own source XID. Late Status/Finished/SelectionRequest
//! messages therefore cannot act on another drag, even against the same target.
use anyhow::{bail, Context, Result};
use std::{
    collections::{BTreeMap, VecDeque},
    io::{self, IoSlice},
    os::{
        fd::{AsRawFd, FromRawFd, OwnedFd},
        unix::net::UnixStream,
    },
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Condvar, Mutex,
    },
    thread,
    time::{Duration, Instant},
};
use x11rb::{
    atom_manager,
    connection::Connection,
    protocol::{xproto::*, Event},
    rust_connection::{DefaultStream, PollMode, RustConnection, Stream},
    utils::RawFdContainer,
    wrapper::ConnectionExt as _,
    COPY_DEPTH_FROM_PARENT, CURRENT_TIME, NONE,
};

pub type Payload = Arc<BTreeMap<String, Vec<u8>>>;
pub const MAX_PAYLOAD: usize = 8 * 1024 * 1024;
const CHUNK: usize = 32 * 1024;
const ROUNDTRIP: Duration = Duration::from_millis(500);
const IDLE: Duration = Duration::from_secs(5);
const LIFETIME: Duration = Duration::from_secs(120);
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum OutcomeKind {
    Accepted(bool),
    Dropped,
    Finished(bool),
    Error(String),
}
#[derive(Clone, Debug)]
pub struct Outcome {
    pub generation: u64,
    pub target: u32,
    pub kind: OutcomeKind,
}
#[derive(Clone)]
struct Desired {
    generation: u64,
    target: u32,
    position: (i32, i32),
    payload: Payload,
    dropped: bool,
}
#[derive(Default)]
struct Jobs {
    desired: Option<Desired>,
    outcomes: VecDeque<Outcome>,
}
struct Shared {
    jobs: Mutex<Jobs>,
    wake: Condvar,
    stopped: AtomicBool,
    available: AtomicBool,
}
pub struct Xdnd {
    shared: Arc<Shared>,
}
impl Xdnd {
    /// Only starts a thread. Connecting/atom/property roundtrips never run here.
    pub fn new(display: u32) -> Self {
        let shared = Arc::new(Shared {
            jobs: Mutex::default(),
            wake: Condvar::new(),
            stopped: AtomicBool::new(false),
            available: AtomicBool::new(false),
        });
        let worker = shared.clone();
        thread::spawn(move || {
            if let Err(error) = run(display, &worker) {
                worker.available.store(false, Ordering::Release);
                let request = worker.jobs.lock().unwrap().desired.clone();
                if let Some(request) = request {
                    publish(&worker, &request, OutcomeKind::Error(error.to_string()));
                }
                tracing::warn!(%error,"private Xwayland XDND source unavailable");
            }
        });
        Self { shared }
    }
    pub fn available(&self) -> bool {
        self.shared.available.load(Ordering::Acquire)
    }
    pub fn begin(
        &self,
        generation: u64,
        target: u32,
        position: (i32, i32),
        payload: Payload,
    ) -> bool {
        let bytes = payload
            .values()
            .try_fold(0usize, |total, v| total.checked_add(v.len()));
        if !self.available()
            || generation == 0
            || target == NONE
            || payload.is_empty()
            || payload.len() > 16
            || bytes.is_none_or(|n| n > MAX_PAYLOAD)
            || payload
                .keys()
                .any(|mime| mime.len() > 255 || !mime.is_ascii() || mime.bytes().any(|b| b < 32))
        {
            return false;
        }
        let mut jobs = self.shared.jobs.lock().unwrap();
        jobs.desired = Some(Desired {
            generation,
            target,
            position,
            payload,
            dropped: false,
        });
        self.shared.wake.notify_one();
        true
    }
    pub fn motion(&self, generation: u64, target: u32, position: (i32, i32)) {
        if let Some(request) = self
            .shared
            .jobs
            .lock()
            .unwrap()
            .desired
            .as_mut()
            .filter(|r| r.generation == generation && !r.dropped)
        {
            request.target = target;
            request.position = position;
            self.shared.wake.notify_one();
        }
    }
    pub fn drop(&self, generation: u64) {
        if let Some(request) = self
            .shared
            .jobs
            .lock()
            .unwrap()
            .desired
            .as_mut()
            .filter(|r| r.generation == generation)
        {
            request.dropped = true;
            self.shared.wake.notify_one();
        }
    }
    pub fn cancel(&self, generation: u64) {
        let mut jobs = self.shared.jobs.lock().unwrap();
        if jobs
            .desired
            .as_ref()
            .is_some_and(|r| r.generation == generation)
        {
            jobs.desired = None;
            self.shared.wake.notify_one();
        }
    }
    pub fn cancel_all(&self) {
        self.shared.jobs.lock().unwrap().desired = None;
        self.shared.wake.notify_one();
    }
    pub fn poll(&self) -> Vec<Outcome> {
        let outcomes: Vec<Outcome> = self
            .shared
            .jobs
            .lock()
            .unwrap()
            .outcomes
            .drain(..)
            .collect();
        for event in &outcomes {
            tracing::trace!(generation=event.generation,target=event.target,kind=?event.kind,"XDND outcome");
        }
        outcomes
    }
}
impl Drop for Xdnd {
    fn drop(&mut self) {
        self.shared.stopped.store(true, Ordering::Release);
        self.cancel_all();
    }
}
fn generation_current(shared: &Shared, generation: u64) -> bool {
    !shared.stopped.load(Ordering::Acquire)
        && shared
            .jobs
            .lock()
            .unwrap()
            .desired
            .as_ref()
            .is_some_and(|r| r.generation == generation)
}
fn publish(shared: &Shared, request: &Desired, kind: OutcomeKind) {
    let mut jobs = shared.jobs.lock().unwrap();
    if matches!(kind, OutcomeKind::Accepted(_)) {
        jobs.outcomes.retain(|o| {
            o.generation != request.generation || !matches!(o.kind, OutcomeKind::Accepted(_))
        });
    }
    while jobs.outcomes.len() >= 64 {
        jobs.outcomes.pop_front();
    }
    jobs.outcomes.push_back(Outcome {
        generation: request.generation,
        target: request.target,
        kind,
    });
}

// x11rb's default blocking reply path waits forever. Bound its actual poll,
// including handshake, so Xwayland death/stall cannot strand the worker.
struct BoundedStream(DefaultStream);
impl Stream for BoundedStream {
    fn poll(&self, mode: PollMode) -> io::Result<()> {
        let mut descriptor = libc::pollfd {
            fd: self.0.as_raw_fd(),
            events: 0,
            revents: 0,
        };
        if mode.readable() {
            descriptor.events |= libc::POLLIN;
        }
        if mode.writable() {
            descriptor.events |= libc::POLLOUT;
        }
        let deadline = Instant::now() + ROUNDTRIP;
        loop {
            let remaining = deadline
                .saturating_duration_since(Instant::now())
                .as_millis()
                .max(1) as i32;
            let result = unsafe { libc::poll(&mut descriptor, 1, remaining) };
            if result > 0 {
                return Ok(());
            }
            if result == 0 {
                return Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    "Xwayland reply deadline",
                ));
            }
            let error = io::Error::last_os_error();
            if error.kind() != io::ErrorKind::Interrupted {
                return Err(error);
            }
            if Instant::now() >= deadline {
                return Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    "Xwayland poll deadline",
                ));
            }
        }
    }
    fn read(&self, buf: &mut [u8], fds: &mut Vec<RawFdContainer>) -> io::Result<usize> {
        self.0.read(buf, fds)
    }
    fn write(&self, buf: &[u8], fds: &mut Vec<RawFdContainer>) -> io::Result<usize> {
        self.0.write(buf, fds)
    }
    fn write_vectored(
        &self,
        bufs: &[IoSlice<'_>],
        fds: &mut Vec<RawFdContainer>,
    ) -> io::Result<usize> {
        self.0.write_vectored(bufs, fds)
    }
}
type Bus = RustConnection<BoundedStream>;
fn connect(display: u32) -> Result<Bus> {
    let name = format!("/tmp/.X11-unix/X{display}");
    let mut address: libc::sockaddr_un = unsafe { std::mem::zeroed() };
    address.sun_family = libc::AF_UNIX as _;
    if name.len() >= address.sun_path.len() {
        bail!("Xwayland socket path too long");
    }
    for (to, from) in address.sun_path.iter_mut().zip(name.bytes()) {
        *to = from as _;
    }
    let raw = unsafe {
        libc::socket(
            libc::AF_UNIX,
            libc::SOCK_STREAM | libc::SOCK_CLOEXEC | libc::SOCK_NONBLOCK,
            0,
        )
    };
    if raw < 0 {
        return Err(io::Error::last_os_error().into());
    }
    let fd = unsafe { OwnedFd::from_raw_fd(raw) };
    let connected = unsafe {
        libc::connect(
            fd.as_raw_fd(),
            (&address as *const libc::sockaddr_un).cast(),
            std::mem::size_of_val(&address) as _,
        )
    };
    if connected < 0 {
        let error = io::Error::last_os_error();
        if !matches!(error.raw_os_error(), Some(libc::EINPROGRESS | libc::EAGAIN)) {
            return Err(error.into());
        }
    }
    let (stream, _) = DefaultStream::from_unix_stream(UnixStream::from(fd))?;
    let stream = BoundedStream(stream);
    stream.poll(PollMode::Writable)?;
    Ok(RustConnection::connect_to_stream(stream, 0)?)
}
atom_manager! {Atoms:AtomsCookie {
    XdndAware,XdndProxy,XdndEnter,XdndPosition,XdndStatus,XdndDrop,XdndLeave,XdndFinished,
    XdndSelection,XdndTypeList,XdndActionCopy,TARGETS,TIMESTAMP,UTF8_STRING,TEXT,INCR,
    _WAYBRIDGE_XDND_TIME,
}}
#[derive(Clone)]
struct Target {
    window: u32,
    recipient: u32,
    version: u32,
}
struct Increment {
    requestor: u32,
    property: Atom,
    target: Atom,
    bytes: Arc<Vec<u8>>,
    offset: usize,
    deadline: Instant,
}
struct Active {
    request: Desired,
    source: u32,
    time: u32,
    target: Option<Target>,
    types: Vec<(Atom, Arc<Vec<u8>>)>,
    last_position: Option<(i32, i32)>,
    waiting: bool,
    accepted: bool,
    sent_drop: bool,
    drop_clock_armed: bool,
    drop_time: Option<u32>,
    touched: Instant,
    created: Instant,
    increments: Vec<Increment>,
}
fn atom(bus: &Bus, name: &str) -> Result<Atom> {
    Ok(bus.intern_atom(false, name.as_bytes())?.reply()?.atom)
}
fn property(bus: &Bus, window: u32, name: Atom, kind: Atom) -> Result<Vec<u32>> {
    let reply = bus
        .get_property(false, window, name, kind, 0, 16)?
        .reply()?;
    if reply.format != 32 {
        return Ok(vec![]);
    }
    Ok(reply.value32().into_iter().flatten().collect())
}
fn target(bus: &Bus, atoms: &Atoms, window: u32) -> Result<Target> {
    let proxy = property(bus, window, atoms.XdndProxy, AtomEnum::WINDOW.into())?
        .first()
        .copied();
    let recipient = if let Some(proxy) = proxy.filter(|p| *p != NONE) {
        if property(bus, proxy, atoms.XdndProxy, AtomEnum::WINDOW.into())
            .ok()
            .and_then(|p| p.first().copied())
            == Some(proxy)
        {
            proxy
        } else {
            window
        }
    } else {
        window
    };
    let version = property(bus, recipient, atoms.XdndAware, AtomEnum::ATOM.into())?
        .first()
        .copied()
        .context("X11 target does not advertise XdndAware")?;
    if version < 3 {
        bail!("X11 target only supports obsolete XDND");
    }
    bus.change_window_attributes(
        window,
        &ChangeWindowAttributesAux::new().event_mask(EventMask::STRUCTURE_NOTIFY),
    )?
    .check()?;
    if recipient != window {
        bus.change_window_attributes(
            recipient,
            &ChangeWindowAttributesAux::new().event_mask(EventMask::STRUCTURE_NOTIFY),
        )?
        .check()?;
    }
    Ok(Target {
        window,
        recipient,
        version: version.min(5),
    })
}
fn message(bus: &Bus, target: u32, window: u32, kind: Atom, data: [u32; 5]) -> Result<()> {
    bus.send_event(
        false,
        target,
        EventMask::NO_EVENT,
        ClientMessageEvent::new(32, window, kind, data),
    )?;
    bus.flush()?;
    Ok(())
}
struct StartingWindow<'a> {
    bus: &'a Bus,
    id: Window,
    armed: bool,
}
impl Drop for StartingWindow<'_> {
    fn drop(&mut self) {
        if self.armed {
            let _ = self.bus.destroy_window(self.id);
            let _ = self.bus.flush();
        }
    }
}
fn start(bus: &Bus, atoms: &Atoms, request: Desired, shared: &Shared) -> Result<Active> {
    if !generation_current(shared, request.generation) {
        bail!("XDND generation superseded");
    }
    let source = bus.generate_id()?;
    let root = bus.setup().roots[0].root;
    bus.create_window(
        COPY_DEPTH_FROM_PARENT,
        source,
        root,
        0,
        0,
        1,
        1,
        0,
        WindowClass::INPUT_ONLY,
        0,
        &CreateWindowAux::new()
            .event_mask(EventMask::PROPERTY_CHANGE | EventMask::STRUCTURE_NOTIFY),
    )?
    .check()?;
    let mut guard = StartingWindow {
        bus,
        id: source,
        armed: true,
    };
    let mut types = Vec::new();
    for (name, bytes) in request.payload.iter() {
        if name == "text/plain" {
            continue;
        }
        types.push((atom(bus, name)?, Arc::new(bytes.clone())));
    }
    let text = request
        .payload
        .get("text/plain;charset=utf-8")
        .or_else(|| request.payload.get("text/plain"));
    if let Some(text) = text {
        match std::str::from_utf8(text) {
            Ok(value) => {
                let bytes = Arc::new(text.clone());
                types.push((atom(bus, "text/plain;charset=utf-8")?, bytes.clone()));
                types.push((atoms.UTF8_STRING, bytes.clone()));
                types.push((atoms.TEXT, bytes));
                // XDND's legacy text/plain target defaults to Latin-1. Never
                // label non-Latin UTF-8 bytes as that old encoding.
                if value.chars().all(|c| (c as u32) <= 255) {
                    let latin = Arc::new(value.chars().map(|c| c as u8).collect::<Vec<_>>());
                    types.push((atom(bus, "text/plain")?, latin.clone()));
                    types.push((AtomEnum::STRING.into(), latin));
                }
            }
            Err(error) => {
                if request.payload.contains_key("text/plain;charset=utf-8") {
                    bail!("invalid UTF-8 plain text payload: {error}");
                }
                // A generic Wayland MIME offer may contain an unqualified
                // legacy encoding. Preserve it without inventing UTF-8 aliases.
                types.push((atom(bus, "text/plain")?, Arc::new(text.clone())));
            }
        }
    }
    types.sort_by_key(|(atom, _)| *atom);
    types.dedup_by_key(|(atom, _)| *atom);
    bus.change_property32(
        PropMode::REPLACE,
        source,
        atoms.XdndTypeList,
        AtomEnum::ATOM,
        &types.iter().map(|(a, _)| *a).collect::<Vec<_>>(),
    )?;
    bus.change_property32(
        PropMode::REPLACE,
        source,
        atoms._WAYBRIDGE_XDND_TIME,
        AtomEnum::CARDINAL,
        &[1],
    )?;
    bus.flush()?;
    // This PropertyNotify carries the X server timestamp, not compositor or Windows uptime.
    let deadline = Instant::now() + ROUNDTRIP;
    let time = loop {
        if let Some(Event::PropertyNotify(event)) = bus.poll_for_event()? {
            if event.window == source && event.atom == atoms._WAYBRIDGE_XDND_TIME {
                break event.time;
            }
        }
        if Instant::now() >= deadline {
            bail!("X server timestamp deadline");
        }
        thread::sleep(Duration::from_millis(1));
    };
    if !generation_current(shared, request.generation) {
        bail!("XDND generation superseded");
    }
    bus.set_selection_owner(source, atoms.XdndSelection, time)?
        .check()?;
    if bus.get_selection_owner(atoms.XdndSelection)?.reply()?.owner != source {
        bail!("XdndSelection ownership was superseded");
    }
    guard.armed = false;
    Ok(Active {
        request,
        source,
        time,
        target: None,
        types,
        last_position: None,
        waiting: false,
        accepted: false,
        sent_drop: false,
        drop_clock_armed: false,
        drop_time: None,
        touched: Instant::now(),
        created: Instant::now(),
        increments: vec![],
    })
}
fn leave(bus: &Bus, atoms: &Atoms, active: &Active) {
    if let Some(target) = &active.target {
        let _ = message(
            bus,
            target.recipient,
            target.window,
            atoms.XdndLeave,
            [active.source, 0, 0, 0, 0],
        );
    }
}
fn end(bus: &Bus, atoms: &Atoms, active: Active, kind: OutcomeKind, shared: &Shared) {
    if !matches!(kind, OutcomeKind::Finished(true)) {
        leave(bus, atoms, &active);
    }
    let _ = bus.destroy_window(active.source);
    let _ = bus.flush();
    publish(shared, &active.request, kind);
}
fn reply(bus: &Bus, request: &SelectionRequestEvent, property: Atom) -> Result<()> {
    bus.send_event(
        false,
        request.requestor,
        EventMask::NO_EVENT,
        SelectionNotifyEvent {
            response_type: SELECTION_NOTIFY_EVENT,
            sequence: 0,
            time: request.time,
            requestor: request.requestor,
            selection: request.selection,
            target: request.target,
            property,
        },
    )?;
    bus.flush()?;
    Ok(())
}
fn selection(
    bus: &Bus,
    atoms: &Atoms,
    active: &mut Active,
    request: SelectionRequestEvent,
    shared: &Shared,
) -> Result<()> {
    if request.owner != active.source
        || request.selection != atoms.XdndSelection
        || (request.time == CURRENT_TIME
            && request.target != atoms.TARGETS
            && request.target != atoms.TIMESTAMP)
        || (request.time != CURRENT_TIME && request.time.wrapping_sub(active.time) > u32::MAX / 2)
    {
        return reply(bus, &request, NONE);
    }
    let output = if request.property == NONE {
        request.target
    } else {
        request.property
    };
    if request.target == atoms.TARGETS {
        let mut values = vec![atoms.TARGETS, atoms.TIMESTAMP];
        values.extend(active.types.iter().map(|(a, _)| *a));
        bus.change_property32(
            PropMode::REPLACE,
            request.requestor,
            output,
            AtomEnum::ATOM,
            &values,
        )?;
    } else if request.target == atoms.TIMESTAMP {
        bus.change_property32(
            PropMode::REPLACE,
            request.requestor,
            output,
            AtomEnum::INTEGER,
            &[active.time],
        )?;
    } else if let Some((_, bytes)) = active.types.iter().find(|(a, _)| *a == request.target) {
        // ICCCM TEXT is a polymorphic target, never a property encoding.
        let encoding = if request.target == atoms.TEXT {
            atoms.UTF8_STRING
        } else {
            request.target
        };
        if bytes.len() <= CHUNK {
            bus.change_property8(
                PropMode::REPLACE,
                request.requestor,
                output,
                encoding,
                bytes,
            )?;
        } else {
            if active.increments.len() >= 8
                || active
                    .increments
                    .iter()
                    .any(|i| i.requestor == request.requestor && i.property == output)
            {
                return reply(bus, &request, NONE);
            }
            bus.change_window_attributes(
                request.requestor,
                &ChangeWindowAttributesAux::new()
                    .event_mask(EventMask::PROPERTY_CHANGE | EventMask::STRUCTURE_NOTIFY),
            )?
            .check()?;
            // The X server roundtrip may overlap a disconnect/read-only
            // transition. Recheck immediately before exposing payload data.
            if !generation_current(shared, active.request.generation) {
                bail!("XDND generation superseded");
            }
            bus.change_property32(
                PropMode::REPLACE,
                request.requestor,
                output,
                atoms.INCR,
                &[bytes.len() as u32],
            )?;
            active.increments.push(Increment {
                requestor: request.requestor,
                property: output,
                target: encoding,
                bytes: bytes.clone(),
                offset: 0,
                deadline: Instant::now() + IDLE,
            });
        }
    } else {
        return reply(bus, &request, NONE);
    }
    active.touched = Instant::now();
    reply(bus, &request, output)
}
fn increment(
    bus: &Bus,
    active: &mut Active,
    event: PropertyNotifyEvent,
    shared: &Shared,
) -> Result<()> {
    if event.state != Property::DELETE {
        return Ok(());
    }
    if let Some(index) = active
        .increments
        .iter()
        .position(|i| i.requestor == event.window && i.property == event.atom)
    {
        // Several PropertyDelete events can already be queued in this batch.
        // Cancellation must stop each individual chunk, not just the next loop.
        if !generation_current(shared, active.request.generation) {
            bail!("XDND generation superseded");
        }
        let transfer = &mut active.increments[index];
        let end = (transfer.offset + CHUNK).min(transfer.bytes.len());
        bus.change_property8(
            PropMode::REPLACE,
            transfer.requestor,
            transfer.property,
            transfer.target,
            &transfer.bytes[transfer.offset..end],
        )?;
        bus.flush()?;
        if transfer.offset == end {
            active.increments.remove(index);
        } else {
            transfer.offset = end;
            transfer.deadline = Instant::now() + IDLE;
        }
        active.touched = Instant::now();
    }
    Ok(())
}
fn pump(
    bus: &Bus,
    atoms: &Atoms,
    active: &mut Active,
    desired: &Desired,
    shared: &Shared,
) -> Result<()> {
    if !generation_current(shared, active.request.generation) {
        bail!("XDND generation superseded");
    }
    if active.sent_drop {
        return Ok(());
    }
    if active
        .target
        .as_ref()
        .is_none_or(|t| t.window != desired.target)
    {
        leave(bus, atoms, active);
        active.request.target = desired.target;
        active.target = Some(target(bus, atoms, desired.target)?);
        active.accepted = false;
        active.waiting = false;
        active.last_position = None;
        if !generation_current(shared, active.request.generation) {
            bail!("XDND generation superseded");
        }
        let target = active.target.as_ref().unwrap();
        let mut data = [active.source, target.version << 24, 0, 0, 0];
        if active.types.len() > 3 {
            data[1] |= 1;
        } else {
            for (index, (kind, _)) in active.types.iter().take(3).enumerate() {
                data[index + 2] = *kind;
            }
        }
        message(bus, target.recipient, target.window, atoms.XdndEnter, data)?;
        publish(shared, &active.request, OutcomeKind::Accepted(false));
    }
    let target = active.target.as_ref().unwrap();
    if active.last_position != Some(desired.position) && !active.waiting {
        let x = desired.position.0.clamp(0, 65535) as u32;
        let y = desired.position.1.clamp(0, 65535) as u32;
        message(
            bus,
            target.recipient,
            target.window,
            atoms.XdndPosition,
            [
                active.source,
                0,
                (x << 16) | y,
                active.time,
                atoms.XdndActionCopy,
            ],
        )?;
        active.last_position = Some(desired.position);
        active.waiting = true;
        active.touched = Instant::now();
    }
    if desired.dropped && !active.waiting && active.last_position == Some(desired.position) {
        if !generation_current(shared, active.request.generation) {
            bail!("XDND generation superseded");
        }
        if !active.accepted {
            bail!("X11 drop target rejected COPY");
        }
        if !active.drop_clock_armed {
            bus.change_property32(
                PropMode::REPLACE,
                active.source,
                atoms._WAYBRIDGE_XDND_TIME,
                AtomEnum::CARDINAL,
                &[2],
            )?;
            bus.flush()?;
            active.drop_clock_armed = true;
            active.touched = Instant::now();
            return Ok(());
        }
        let Some(time) = active.drop_time else {
            return Ok(());
        };
        message(
            bus,
            target.recipient,
            target.window,
            atoms.XdndDrop,
            [active.source, 0, time, 0, 0],
        )?;
        active.sent_drop = true;
        active.touched = Instant::now();
        publish(shared, &active.request, OutcomeKind::Dropped);
    }
    Ok(())
}
fn run(display: u32, shared: &Shared) -> Result<()> {
    let bus = connect(display)?;
    let atoms = Atoms::new(&bus)?.reply()?;
    shared.available.store(true, Ordering::Release);
    let mut active: Option<Active> = None;
    while !shared.stopped.load(Ordering::Acquire) {
        let desired = shared.jobs.lock().unwrap().desired.clone();
        if active.as_ref().is_some_and(|a| {
            desired
                .as_ref()
                .is_none_or(|r| r.generation != a.request.generation)
        }) {
            end(
                &bus,
                &atoms,
                active.take().unwrap(),
                OutcomeKind::Finished(false),
                shared,
            );
        }
        if active.is_none() {
            if let Some(request) = desired.clone() {
                match start(&bus, &atoms, request.clone(), shared) {
                    Ok(value) => active = Some(value),
                    Err(error) => {
                        publish(shared, &request, OutcomeKind::Error(error.to_string()));
                        let mut jobs = shared.jobs.lock().unwrap();
                        if jobs
                            .desired
                            .as_ref()
                            .is_some_and(|r| r.generation == request.generation)
                        {
                            jobs.desired = None;
                        }
                    }
                }
            }
        }
        let mut terminal = None;
        if let Some(current) = active.as_mut() {
            for _ in 0..64 {
                let Some(event) = bus.poll_for_event()? else {
                    break;
                };
                match event {
                    Event::ClientMessage(event)
                        if event.window == current.source && event.format == 32 =>
                    {
                        let data = event.data.as_data32();
                        if current.target.as_ref().is_some_and(|t| data[0] == t.window) {
                            if event.type_ == atoms.XdndStatus && !current.sent_drop {
                                current.accepted =
                                    data[1] & 1 != 0 && data[4] == atoms.XdndActionCopy;
                                current.waiting = false;
                                current.touched = Instant::now();
                                publish(
                                    shared,
                                    &current.request,
                                    OutcomeKind::Accepted(current.accepted),
                                );
                            } else if event.type_ == atoms.XdndFinished && current.sent_drop {
                                let target = current.target.as_ref().unwrap();
                                let success = target.version < 5
                                    || data[1] & 1 != 0 && data[2] == atoms.XdndActionCopy;
                                terminal = Some(OutcomeKind::Finished(success));
                                break;
                            }
                        }
                    }
                    Event::SelectionRequest(event) => {
                        if !generation_current(shared, current.request.generation) {
                            terminal = Some(OutcomeKind::Finished(false));
                            break;
                        }
                        if let Err(error) = selection(&bus, &atoms, current, event, shared) {
                            terminal = Some(OutcomeKind::Error(error.to_string()));
                            break;
                        }
                    }
                    Event::PropertyNotify(event) => {
                        if event.window == current.source
                            && event.atom == atoms._WAYBRIDGE_XDND_TIME
                            && current.drop_clock_armed
                        {
                            current.drop_time = Some(event.time);
                        }
                        if let Err(error) = increment(&bus, current, event, shared) {
                            terminal = Some(OutcomeKind::Error(error.to_string()));
                            break;
                        }
                    }
                    Event::SelectionClear(event)
                        if event.owner == current.source
                            && event.selection == atoms.XdndSelection =>
                    {
                        terminal = Some(OutcomeKind::Error("XdndSelection ownership lost".into()));
                        break;
                    }
                    Event::DestroyNotify(event)
                        if current.target.as_ref().is_some_and(|t| {
                            event.window == t.window || event.window == t.recipient
                        }) =>
                    {
                        terminal = Some(OutcomeKind::Error("X11 drop target disappeared".into()));
                        break;
                    }
                    Event::DestroyNotify(event) => {
                        current.increments.retain(|i| i.requestor != event.window)
                    }
                    _ => (),
                }
            }
            if terminal.is_none() {
                if current.created.elapsed() > LIFETIME
                    || current
                        .increments
                        .iter()
                        .any(|i| i.deadline < Instant::now())
                    || !current.sent_drop && current.touched.elapsed() > IDLE
                {
                    terminal = Some(OutcomeKind::Error("XDND deadline exceeded".into()));
                } else if let Some(desired) = desired.as_ref() {
                    if let Err(error) = pump(&bus, &atoms, current, desired, shared) {
                        terminal = Some(OutcomeKind::Error(error.to_string()));
                    }
                }
            }
        }
        if let Some(kind) = terminal {
            let done = active.take().unwrap();
            let generation = done.request.generation;
            end(&bus, &atoms, done, kind, shared);
            let mut jobs = shared.jobs.lock().unwrap();
            if jobs
                .desired
                .as_ref()
                .is_some_and(|r| r.generation == generation)
            {
                jobs.desired = None;
            }
        }
        let jobs = shared.jobs.lock().unwrap();
        let _guard = shared
            .wake
            .wait_timeout(
                jobs,
                if active.is_some() {
                    Duration::from_millis(5)
                } else {
                    Duration::from_millis(100)
                },
            )
            .unwrap();
    }
    if let Some(active) = active {
        end(&bus, &atoms, active, OutcomeKind::Finished(false), shared);
    }
    shared.available.store(false, Ordering::Release);
    Ok(())
}
