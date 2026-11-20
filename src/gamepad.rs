//! Opt-in uinput gamepads. The authenticated connection owns the devices and a
//! foreground lease gates their input and feedback.
use serde_json::{json, Value};
use std::{
    collections::BTreeMap,
    fs::{File, OpenOptions},
    io::{self, Read, Write},
    mem::{size_of, zeroed},
    os::{fd::AsRawFd, unix::fs::OpenOptionsExt},
    path::{Path, PathBuf},
    time::{Duration, Instant},
};

const EV_SYN: u16 = 0;
const EV_KEY: u16 = 1;
const EV_ABS: u16 = 3;
const EV_FF: u16 = 21;
const EV_UINPUT: u16 = 0x101;
const FF_RUMBLE: u16 = 0x50;
const FF_GAIN: u16 = 0x60;
const BUTTON_MASK: u16 = 0xf3ff;
const GUIDE_BUTTON: u16 = 0x0400;
const MAX_EFFECTS: usize = 16;
const RECONNECT_GRACE: Duration = Duration::from_secs(5);
const BUTTONS: [(u16, u16); 11] = [
    (0x1000, 0x130), // A / south
    (0x2000, 0x131), // B / east
    (0x4000, 0x133), // X / west
    (0x8000, 0x134), // Y / north
    (0x0100, 0x136),
    (0x0200, 0x137), // shoulders
    (0x0020, 0x13a),
    (0x0010, 0x13b),       // back / start
    (GUIDE_BUTTON, 0x13c), // Guide / BTN_MODE
    (0x0040, 0x13d),
    (0x0080, 0x13e), // thumb buttons
];
const AXES: [(u16, i32, i32); 8] = [
    (0, -32768, 32767),
    (1, -32768, 32767),
    (3, -32768, 32767),
    (4, -32768, 32767),
    (2, 0, 255),
    (5, 0, 255),
    (16, -1, 1),
    (17, -1, 1),
];
const fn ioctl_code(direction: u32, kind: u8, number: u8, size: usize) -> libc::c_ulong {
    ((direction << 30) | ((size as u32) << 16) | ((kind as u32) << 8) | number as u32) as _
}
fn ioctl_value(file: &File, number: u8, value: i32) -> io::Result<()> {
    let result = unsafe { libc::ioctl(file.as_raw_fd(), ioctl_code(1, b'U', number, 4), value) };
    if result < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}
fn ioctl_data<T>(file: &File, direction: u32, number: u8, data: &mut T) -> io::Result<()> {
    let result = unsafe {
        libc::ioctl(
            file.as_raw_fd(),
            ioctl_code(direction, b'U', number, size_of::<T>()),
            data,
        )
    };
    if result < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}
fn ioctl_empty(file: &File, number: u8) -> io::Result<()> {
    let result = unsafe { libc::ioctl(file.as_raw_fd(), ioctl_code(0, b'U', number, 0)) };
    if result < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}
fn open_backend(path: &Path) -> io::Result<File> {
    OpenOptions::new()
        .read(true)
        .write(true)
        .custom_flags(libc::O_NONBLOCK | libc::O_CLOEXEC)
        .open(path)
}
fn backend_error(error: &io::Error) -> String {
    match error.raw_os_error() {
        Some(libc::EACCES | libc::EPERM) => {
            "The session cannot access /dev/uinput. Controller forwarding is unavailable.".into()
        }
        Some(libc::ENOENT | libc::ENODEV) => {
            "No /dev/uinput device is available. Controller forwarding is unavailable.".into()
        }
        _ => format!("Virtual controller backend failed: {error}"),
    }
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct Sample {
    buttons: u16,
    axes: [i32; 8],
}
impl Sample {
    fn parse(value: &Value) -> Option<Self> {
        let mut buttons = u16::try_from(value["buttons"].as_u64()?).ok()?;
        if buttons & !BUTTON_MASK != 0 {
            return None;
        }
        if let Some(guide) = value.get("guide") {
            if guide.as_bool()? {
                buttons |= GUIDE_BUTTON;
            }
        }
        let mut axes = [0; 8];
        for (i, field) in ["lx", "ly", "rx", "ry", "lt", "rt"].into_iter().enumerate() {
            let n = value[field].as_i64()?;
            let (min, max) = if i < 4 { (-32768, 32767) } else { (0, 255) };
            if !(min..=max).contains(&n) {
                return None;
            }
            axes[i] = n as i32;
        }
        // XInput's positive Y means up; evdev's negative Y means up. Saturate
        // the asymmetric endpoint rather than overflowing -(-32768).
        axes[1] = (-axes[1]).clamp(-32768, 32767);
        axes[3] = (-axes[3]).clamp(-32768, 32767);
        axes[6] = i32::from(buttons & 8 != 0) - i32::from(buttons & 4 != 0);
        axes[7] = i32::from(buttons & 2 != 0) - i32::from(buttons & 1 != 0);
        Some(Self { buttons, axes })
    }
}
#[derive(Clone, Copy)]
struct Effect {
    strong: u16,
    weak: u16,
    length: u16,
    delay: u16,
}
struct Playing {
    start: Instant,
    end: Option<Instant>,
}
struct Device {
    file: File,
    name: String,
    sysname: String,
    incarnation: u64,
    sequence: u64,
    sample: Sample,
    effects: BTreeMap<i16, Effect>,
    playing: BTreeMap<i16, Playing>,
    gain: u16,
    last_rumble: (u16, u16),
    rumble_sent: Instant,
    reconnect_until: Option<Instant>,
}
impl Device {
    fn new(path: &Path, slot: usize, incarnation: u64, session: u64) -> io::Result<Self> {
        let file = open_backend(path)?;
        for kind in [EV_SYN, EV_KEY, EV_ABS, EV_FF] {
            ioctl_value(&file, 100, i32::from(kind))?;
        }
        for (_, code) in BUTTONS {
            ioctl_value(&file, 101, i32::from(code))?;
        }
        for (code, min, max) in AXES {
            ioctl_value(&file, 103, i32::from(code))?;
            let mut setup: libc::uinput_abs_setup = unsafe { zeroed() };
            setup.code = code;
            setup.absinfo.minimum = min;
            setup.absinfo.maximum = max;
            ioctl_data(&file, 1, 4, &mut setup)?;
        }
        ioctl_value(&file, 107, i32::from(FF_RUMBLE))?;
        ioctl_value(&file, 107, i32::from(FF_GAIN))?;
        let name = format!(
            "Waybridge Gamepad {} [{}-{}]",
            slot + 1,
            std::process::id(),
            session
        );
        let phys =
            std::ffi::CString::new(format!("waybridge/{}/{session}/{slot}", std::process::id()))
                .unwrap();
        let result = unsafe {
            libc::ioctl(
                file.as_raw_fd(),
                ioctl_code(1, b'U', 108, size_of::<*const libc::c_char>()),
                phys.as_ptr(),
            )
        };
        if result < 0 {
            return Err(io::Error::last_os_error());
        }
        let mut setup: libc::uinput_setup = unsafe { zeroed() };
        // USB Xbox 360's standard axes/button layout is understood by SDL,
        // joydev, X11 games and Steam. The name/physical path identify us.
        setup.id = libc::input_id {
            bustype: 3,
            vendor: 0x045e,
            product: 0x028e,
            version: 0x0114,
        };
        for (target, source) in setup.name.iter_mut().zip(name.bytes()) {
            *target = source as _;
        }
        setup.ff_effects_max = MAX_EFFECTS as _;
        ioctl_data(&file, 1, 3, &mut setup)?;
        ioctl_empty(&file, 1)?;
        let mut sysname = [0u8; 128];
        let result = unsafe {
            libc::ioctl(
                file.as_raw_fd(),
                ioctl_code(2, b'U', 44, sysname.len()),
                sysname.as_mut_ptr(),
            )
        };
        let sysname = if result >= 0 {
            String::from_utf8_lossy(
                &sysname[..sysname
                    .iter()
                    .position(|b| *b == 0)
                    .unwrap_or(sysname.len())],
            )
            .into_owned()
        } else {
            String::new()
        };
        Ok(Self {
            file,
            name,
            sysname,
            incarnation,
            sequence: 0,
            sample: Sample::default(),
            effects: BTreeMap::new(),
            playing: BTreeMap::new(),
            gain: u16::MAX,
            last_rumble: (0, 0),
            rumble_sent: Instant::now(),
            reconnect_until: None,
        })
    }
    fn write(&mut self, sample: Sample) -> io::Result<()> {
        let mut events = Vec::with_capacity(BUTTONS.len() + AXES.len() + 1);
        for (mask, code) in BUTTONS {
            events.push(event(EV_KEY, code, i32::from(sample.buttons & mask != 0)));
        }
        for (i, (code, _, _)) in AXES.into_iter().enumerate() {
            events.push(event(EV_ABS, code, sample.axes[i]));
        }
        events.push(event(EV_SYN, 0, 0));
        let bytes = unsafe {
            std::slice::from_raw_parts(
                events.as_ptr().cast::<u8>(),
                events.len() * size_of::<libc::input_event>(),
            )
        };
        self.file.write_all(bytes)?;
        self.sample = sample;
        Ok(())
    }
    fn pause(&mut self) -> io::Result<()> {
        self.playing.clear();
        self.last_rumble = (0, 0);
        if self.sample != Sample::default() {
            self.write(Sample::default())?;
        }
        let _ = self.feedback(Instant::now(), false)?;
        Ok(())
    }
    fn feedback(&mut self, now: Instant, playback: bool) -> io::Result<Option<(u16, u16)>> {
        // Kernel FF upload/erase requests must always be acknowledged, including
        // unsupported effects, otherwise the game's EVIOCSFF ioctl blocks.
        for _ in 0..64 {
            let mut input: libc::input_event = unsafe { zeroed() };
            let bytes = unsafe {
                std::slice::from_raw_parts_mut(
                    (&mut input as *mut libc::input_event).cast::<u8>(),
                    size_of::<libc::input_event>(),
                )
            };
            match self.file.read(bytes) {
                Ok(0) => break,
                Ok(n) if n == bytes.len() => {}
                Ok(_) => {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        "short uinput event",
                    ))
                }
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(error) => return Err(error),
            }
            if input.type_ == EV_UINPUT && input.code == 1 {
                let mut upload: libc::uinput_ff_upload = unsafe { zeroed() };
                upload.request_id = input.value as _;
                ioctl_data(&self.file, 3, 200, &mut upload)?;
                let effect = &upload.effect;
                if effect.type_ != FF_RUMBLE
                    || !(0..MAX_EFFECTS as i16).contains(&effect.id)
                    || effect.trigger.button != 0
                {
                    upload.retval = -libc::EINVAL;
                } else {
                    let rumble = unsafe {
                        std::ptr::read_unaligned(effect.u.as_ptr().cast::<libc::ff_rumble_effect>())
                    };
                    self.effects.insert(
                        effect.id,
                        Effect {
                            strong: rumble.strong_magnitude,
                            weak: rumble.weak_magnitude,
                            length: effect.replay.length,
                            delay: effect.replay.delay,
                        },
                    );
                    upload.retval = 0;
                }
                ioctl_data(&self.file, 1, 201, &mut upload)?;
            } else if input.type_ == EV_UINPUT && input.code == 2 {
                let mut erase: libc::uinput_ff_erase = unsafe { zeroed() };
                erase.request_id = input.value as _;
                ioctl_data(&self.file, 3, 202, &mut erase)?;
                self.effects.remove(&(erase.effect_id as i16));
                self.playing.remove(&(erase.effect_id as i16));
                erase.retval = 0;
                ioctl_data(&self.file, 1, 203, &mut erase)?;
            } else if input.type_ == EV_FF && input.code == FF_GAIN {
                self.gain = input.value.clamp(0, 65535) as _;
            } else if input.type_ == EV_FF && input.code < MAX_EFFECTS as u16 {
                let id = input.code as i16;
                if input.value <= 0 {
                    self.playing.remove(&id);
                } else if let Some(effect) = self.effects.get(&id).filter(|_| playback) {
                    let start = now + Duration::from_millis(u64::from(effect.delay));
                    let duration =
                        u64::from(effect.length).saturating_mul(input.value.min(32767) as u64);
                    self.playing.insert(
                        id,
                        Playing {
                            start,
                            end: (duration != 0).then(|| start + Duration::from_millis(duration)),
                        },
                    );
                }
            }
        }
        self.playing
            .retain(|_, play| play.end.is_none_or(|end| now < end));
        let mut motors = (0u16, 0u16);
        for (id, play) in &self.playing {
            if now < play.start {
                continue;
            }
            if let Some(effect) = self.effects.get(id) {
                motors.0 = motors.0.max(effect.strong);
                motors.1 = motors.1.max(effect.weak);
            }
        }
        motors.0 = (u32::from(motors.0) * u32::from(self.gain) / 65535) as _;
        motors.1 = (u32::from(motors.1) * u32::from(self.gain) / 65535) as _;
        if motors != self.last_rumble
            || (motors != (0, 0)
                && now.duration_since(self.rumble_sent) >= Duration::from_millis(250))
        {
            self.last_rumble = motors;
            self.rumble_sent = now;
            return Ok(Some(motors));
        }
        Ok(None)
    }
}
fn event(kind: u16, code: u16, value: i32) -> libc::input_event {
    let mut result: libc::input_event = unsafe { zeroed() };
    result.type_ = kind;
    result.code = code;
    result.value = value;
    result
}
impl Drop for Device {
    fn drop(&mut self) {
        let _ = self.write(Sample::default());
        let _ = ioctl_empty(&self.file, 2);
    }
}

pub struct Gamepads {
    backend: PathBuf,
    initialized: bool,
    available: bool,
    requested: bool,
    supported: bool,
    error: String,
    client_focus: u32,
    server_focus: Option<u32>,
    allowed: bool,
    target: Option<u32>,
    generation: u64,
    connection_generation: u64,
    session: u64,
    incarnations: [u64; 4],
    devices: [Option<Device>; 4],
    reconnect: bool,
    outgoing: Vec<Value>,
}
impl Default for Gamepads {
    fn default() -> Self {
        Self::with_backend(PathBuf::from("/dev/uinput"))
    }
}
impl Gamepads {
    fn with_backend(backend: PathBuf) -> Self {
        Self {
            backend,
            initialized: false,
            available: false,
            requested: false,
            supported: false,
            error: String::new(),
            client_focus: 0,
            server_focus: None,
            allowed: false,
            target: None,
            generation: 0,
            connection_generation: 0,
            session: 0,
            incarnations: [0; 4],
            devices: std::array::from_fn(|_| None),
            reconnect: false,
            outgoing: Vec::new(),
        }
    }
    pub fn negotiate(&mut self, available: bool, enabled: bool, reconnect: bool) {
        if self.initialized {
            return;
        }
        self.initialized = true;
        self.available = available;
        self.requested = available && enabled;
        self.reconnect = available && reconnect;
        self.session = self.session.wrapping_add(1).max(1);
        self.generation = self.generation.wrapping_add(1).max(1);
        self.connection_generation = self.generation;
        self.probe();
        self.sync();
        self.publish();
    }
    fn probe(&mut self) {
        if !self.available {
            self.supported = false;
            return;
        }
        match open_backend(&self.backend) {
            Ok(file) => {
                let mut version = 0u32;
                match ioctl_data(&file, 2, 45, &mut version) {
                    Ok(()) if version >= 5 => {
                        self.supported = true;
                        self.error.clear();
                    }
                    Ok(()) => {
                        self.supported = false;
                        self.error = "The installed uinput version does not support virtual controller setup".into();
                    }
                    Err(error) => {
                        self.supported = false;
                        self.error = backend_error(&error);
                    }
                }
            }
            Err(error) => {
                self.supported = false;
                self.error = backend_error(&error);
            }
        }
    }
    pub fn set_enabled(&mut self, enabled: bool) {
        if !self.initialized || !self.available {
            return;
        }
        self.requested = enabled;
        if enabled {
            self.probe();
        }
        self.sync();
        self.publish();
    }
    pub fn focus(&mut self, id: u32) {
        self.client_focus = id;
        self.sync();
    }
    pub fn policy(&mut self, focused: Option<u32>, allowed: bool) {
        self.server_focus = focused;
        self.allowed = allowed;
        self.sync();
    }
    pub fn release_focus(&mut self) {
        self.server_focus = None;
        self.sync();
    }
    fn sync(&mut self) {
        let keep_devices = self.available && self.requested && self.supported && self.allowed;
        let mut target = (keep_devices
            && self.client_focus != 0
            && self.server_focus == Some(self.client_focus))
        .then_some(self.client_focus);
        let had_devices = self.devices.iter().any(Option::is_some);
        if !keep_devices {
            self.clear_devices();
        } else if target != self.target {
            // Drain pending rumble on both sides of the transition. A game may
            // keep its evdev handle and uploaded effects while the lease is paused.
            if let Err(error) = self.pause_devices() {
                self.error = backend_error(&error);
                self.supported = false;
                target = None;
                self.clear_devices();
            }
        }
        let target_changed = target != self.target;
        if target_changed {
            self.target = target;
            self.generation = self.generation.wrapping_add(1).max(1);
        }
        if target_changed || (had_devices && (!keep_devices || !self.supported)) {
            self.publish();
        }
    }
    fn publish(&mut self) {
        if !self.available {
            return;
        }
        let devices: Vec<_> = self.devices.iter().enumerate().filter_map(|(slot, d)| d.as_ref().map(|d| json!({"slot":slot,"name":d.name,"device_generation":d.incarnation,"sysname":d.sysname,"reconnecting":d.reconnect_until.is_some()}))).collect();
        let message = json!({"type":"gamepad_state","supported":self.supported,"requested":self.requested,"enabled":self.target.is_some(),"id":self.target.unwrap_or(0),"generation":self.generation,"devices":devices,"rumble_supported":self.supported,"error":self.error});
        // Status is latest-only, but zero-motor terminal events retain ordering.
        self.outgoing.retain(|m| m["type"] != "gamepad_state");
        self.outgoing.push(message);
    }
    fn rumble(&mut self, slot: usize, incarnation: u64, strong: u16, weak: u16) {
        self.outgoing.push(json!({"type":"gamepad_rumble","generation":self.generation,"slot":slot,"device_generation":incarnation,"strong":strong,"weak":weak,"duration_ms":750}));
    }
    fn stop_rumble(&mut self, slot: usize, incarnation: u64) {
        self.outgoing.retain(|message| {
            message["type"] != "gamepad_rumble"
                || message["slot"] != slot
                || (message["strong"] == 0 && message["weak"] == 0)
        });
        self.rumble(slot, incarnation, 0, 0);
    }
    fn pause_devices(&mut self) -> io::Result<()> {
        for slot in 0..self.devices.len() {
            if let Some(device) = &mut self.devices[slot] {
                device.pause()?;
                let incarnation = device.incarnation;
                if self.target.is_some() {
                    self.stop_rumble(slot, incarnation);
                }
            }
        }
        Ok(())
    }
    fn clear_devices(&mut self) {
        for slot in 0..self.devices.len() {
            if let Some(device) = self.devices[slot].take() {
                self.stop_rumble(slot, device.incarnation);
                drop(device);
            }
        }
        self.incarnations = [0; 4];
    }
    fn detach(&mut self, slot: usize, incarnation: u64) {
        if self.devices[slot].take().is_some() {
            self.stop_rumble(slot, incarnation);
            self.publish();
        }
        self.incarnations[slot] = incarnation.saturating_add(1);
    }
    pub fn input(&mut self, value: &Value) {
        let Some(generation) = value["generation"].as_u64() else {
            return;
        };
        let Some(id) = value["id"].as_u64().and_then(|id| u32::try_from(id).ok()) else {
            return;
        };
        let Some(slot) = value["slot"]
            .as_u64()
            .filter(|s| *s < 4)
            .map(|s| s as usize)
        else {
            return;
        };
        let Some(incarnation) = value["device_generation"].as_u64().filter(|g| *g != 0) else {
            return;
        };
        let Some(sequence) = value["sequence"].as_u64().filter(|g| *g != 0) else {
            return;
        };
        let Some(connected) = value["connected"].as_bool() else {
            return;
        };
        if incarnation < self.incarnations[slot] {
            return;
        }
        if let Some(device) = &self.devices[slot] {
            if device.incarnation == incarnation && sequence <= device.sequence {
                return;
            }
        }
        if !connected {
            // A physical device may unplug during blur, or before the viewer
            // receives the latest focus transition. Accept the exact current
            // device's newer detach within this authenticated connection.
            if self.connection_generation == 0
                || !(self.connection_generation..=self.generation).contains(&generation)
                || self.incarnations[slot] != incarnation
            {
                return;
            }
            if self.reconnect && value["suspended"] == true {
                let Some(device) = self.devices[slot].as_mut() else {
                    return;
                };
                if device.reconnect_until.is_some() {
                    return;
                }
                if let Err(error) = device.pause() {
                    self.error = backend_error(&error);
                    self.supported = false;
                    self.sync();
                    self.publish();
                    return;
                }
                device.sequence = sequence;
                device.reconnect_until = Some(Instant::now() + RECONNECT_GRACE);
                self.stop_rumble(slot, incarnation);
                self.publish();
            } else {
                self.detach(slot, incarnation);
            }
            return;
        }
        if generation != self.generation || self.target != Some(id) {
            return;
        }
        let Some(sample) = Sample::parse(value) else {
            return;
        };
        if let Some(device) = self.devices[slot]
            .as_mut()
            .filter(|d| d.reconnect_until.is_some())
        {
            if incarnation == device.incarnation {
                // Delayed snapshots cannot resume a physically detached device.
                return;
            }
            if self.reconnect
                && value["reconnect_from"].as_u64() == Some(device.incarnation)
                && device
                    .reconnect_until
                    .is_some_and(|until| Instant::now() < until)
            {
                if let Err(error) = device.pause() {
                    self.error = backend_error(&error);
                    self.supported = false;
                    self.sync();
                    self.publish();
                    return;
                }
                device.incarnation = incarnation;
                device.sequence = 0;
                device.reconnect_until = None;
                self.incarnations[slot] = incarnation;
                self.publish();
            }
        }
        if self.devices[slot]
            .as_ref()
            .is_none_or(|d| d.incarnation != incarnation)
        {
            if let Some(old) = self.devices[slot].take() {
                self.stop_rumble(slot, old.incarnation);
                drop(old);
            }
            match Device::new(&self.backend, slot, incarnation, self.session) {
                Ok(device) => self.devices[slot] = Some(device),
                Err(error) => {
                    self.error = backend_error(&error);
                    self.supported = false;
                    self.sync();
                    self.publish();
                    return;
                }
            }
            self.incarnations[slot] = incarnation;
            self.publish();
        }
        let device = self.devices[slot].as_mut().unwrap();
        if let Err(error) = device.write(sample) {
            self.error = backend_error(&error);
            self.supported = false;
            self.sync();
            self.publish();
            return;
        }
        device.sequence = sequence;
    }
    pub fn poll(&mut self) -> Vec<Value> {
        let now = Instant::now();
        for slot in 0..self.devices.len() {
            if let Some(device) = self.devices[slot]
                .as_ref()
                .filter(|d| d.reconnect_until.is_some_and(|until| now >= until))
            {
                self.detach(slot, device.incarnation);
                continue;
            }
            let feedback = self.devices[slot].as_mut().map(|d| {
                (
                    d.incarnation,
                    d.feedback(now, self.target.is_some() && d.reconnect_until.is_none()),
                )
            });
            if let Some((incarnation, result)) = feedback {
                match result {
                    Ok(Some((strong, weak))) => self.rumble(slot, incarnation, strong, weak),
                    Ok(None) => {}
                    Err(error) => {
                        self.error = backend_error(&error);
                        self.supported = false;
                        self.sync();
                        self.publish();
                        break;
                    }
                }
            }
        }
        std::mem::take(&mut self.outgoing)
    }
    pub fn disconnect(&mut self) {
        self.clear_devices();
        self.target = None;
        self.client_focus = 0;
        self.server_focus = None;
        self.allowed = false;
        self.initialized = false;
        self.available = false;
        self.requested = false;
        self.reconnect = false;
        self.connection_generation = 0;
        self.generation = self.generation.wrapping_add(1).max(1);
        self.outgoing.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn input(generation: u64, incarnation: u64, sequence: u64) -> Value {
        json!({"type":"gamepad","generation":generation,"id":7,"slot":0,
               "connected":true,"device_generation":incarnation,"sequence":sequence,
               "buttons":0x1001,"lx":12345,"ly":-32768,"rx":-30000,"ry":32767,
               "lt":123,"rt":255})
    }
    #[test]
    fn strict_samples_and_linux_axis_direction() {
        let valid = input(1, 1, 1);
        let sample = Sample::parse(&valid).unwrap();
        assert_eq!(sample.axes, [12345, 32767, -30000, -32767, 123, 255, 0, -1]);
        let mut guide = valid.clone();
        guide["guide"] = json!(true);
        assert_eq!(
            Sample::parse(&guide).unwrap().buttons,
            sample.buttons | GUIDE_BUTTON
        );
        guide["guide"] = json!(false);
        assert_eq!(Sample::parse(&guide).unwrap(), sample);
        for (field, bad) in [
            ("lx", json!(32768)),
            ("ly", json!(-32769)),
            ("rt", json!(256)),
            ("lt", json!(-1)),
            ("buttons", json!(0x0400)),
            ("buttons", json!(1.5)),
            ("guide", json!(1)),
        ] {
            let mut malformed = valid.clone();
            malformed[field] = bad;
            assert!(Sample::parse(&malformed).is_none(), "{field}: {malformed}");
        }
        let mut missing = valid;
        missing.as_object_mut().unwrap().remove("ry");
        assert!(Sample::parse(&missing).is_none());
    }
    #[test]
    fn missing_backend_and_opt_in_fail_closed() {
        let mut pads = Gamepads::with_backend(PathBuf::from("/waybridge-test-no-such-uinput"));
        pads.policy(Some(7), true);
        pads.focus(7);
        pads.negotiate(true, false, false);
        let status = pads.poll().pop().unwrap();
        assert_eq!(status["requested"], false);
        assert_eq!(status["supported"], false);
        assert!(!status["error"].as_str().unwrap().is_empty());
        pads.set_enabled(true);
        pads.input(&input(pads.generation, 1, 1));
        assert!(pads.devices.iter().all(Option::is_none));
        assert!(pads.target.is_none());
        pads.disconnect();
        assert!(pads.poll().is_empty());
    }
    #[test]
    fn denied_backend_is_a_feature_diagnostic() {
        use std::os::unix::fs::OpenOptionsExt;
        if unsafe { libc::geteuid() } == 0 {
            return;
        }
        let path = std::env::temp_dir().join(format!(
            "waybridge-gamepad-denied-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o0)
            .open(&path)
            .unwrap();
        let mut pads = Gamepads::with_backend(path.clone());
        pads.negotiate(true, true, false);
        pads.focus(7);
        pads.policy(Some(7), true);
        let status = pads.poll().pop().unwrap();
        drop(file);
        std::fs::remove_file(path).unwrap();
        assert_eq!(status["supported"], false);
        assert_eq!(status["enabled"], false);
        assert!(status["error"]
            .as_str()
            .unwrap()
            .contains("cannot access /dev/uinput"));
        assert!(pads.devices.iter().all(Option::is_none));
    }
    #[test]
    fn invalid_input_cannot_touch_backend_or_bypass_focus() {
        // /dev/null opens successfully but is not a uinput backend. Malformed,
        // stale, off and unfocused input must be rejected before the first ioctl.
        let mut pads = Gamepads::with_backend(PathBuf::from("/dev/null"));
        pads.negotiate(true, false, false);
        assert!(!pads.supported); // Opening a character device is insufficient.
                                  // Controlled healthy-probe fact: guard tests below must never reach
                                  // /dev/null's failing device-creation ioctl until a valid live snapshot.
        pads.supported = true;
        pads.error.clear();
        pads.focus(7);
        pads.policy(Some(7), true);
        pads.input(&input(pads.generation, 1, 1));
        assert!(pads.error.is_empty());
        pads.set_enabled(true);
        pads.supported = true;
        pads.error.clear();
        pads.sync();
        let generation = pads.generation;
        let mut bad = input(generation, 1, 1);
        bad["slot"] = json!(4);
        pads.input(&bad);
        bad = input(generation, 1, 1);
        bad["buttons"] = json!(65536);
        pads.input(&bad);
        pads.input(&input(generation - 1, 1, 1));
        pads.input(&input(generation, 0, 1));
        pads.input(&input(generation, 1, 0));
        assert!(pads.error.is_empty());
        pads.policy(Some(8), true);
        pads.input(&input(generation, 1, 1));
        assert!(pads.error.is_empty());
        pads.policy(Some(7), true);
        pads.input(&input(pads.generation, 1, 1));
        assert!(!pads.supported && pads.target.is_none());
        assert!(!pads.error.is_empty()); // ENOTTY feature failure, no panic.
    }
    fn own_evdev(device: &Device) -> io::Result<File> {
        let deadline = Instant::now() + Duration::from_secs(3);
        loop {
            let directory = PathBuf::from("/sys/devices/virtual/input").join(&device.sysname);
            if let Ok(entries) = std::fs::read_dir(directory) {
                for entry in entries.flatten() {
                    if entry.file_name().as_encoded_bytes().starts_with(b"event") {
                        let path = PathBuf::from("/dev/input").join(entry.file_name());
                        if let Ok(file) = OpenOptions::new()
                            .read(true)
                            .write(true)
                            .custom_flags(libc::O_NONBLOCK | libc::O_CLOEXEC)
                            .open(path)
                        {
                            return Ok(file);
                        }
                    }
                }
            }
            if Instant::now() >= deadline {
                return Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    "own evdev node unavailable",
                ));
            }
            std::thread::sleep(Duration::from_millis(10));
        }
    }
    fn drain(file: &mut File) -> Vec<(u16, u16, i32)> {
        let mut result = Vec::new();
        loop {
            let mut event: libc::input_event = unsafe { zeroed() };
            let bytes = unsafe {
                std::slice::from_raw_parts_mut(
                    (&mut event as *mut libc::input_event).cast::<u8>(),
                    size_of::<libc::input_event>(),
                )
            };
            match file.read(bytes) {
                Ok(n) if n == bytes.len() => result.push((event.type_, event.code, event.value)),
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                Err(error) => panic!("own evdev read: {error}"),
                _ => break,
            }
        }
        result
    }
    fn play_effect(file: &mut File, id: i16) {
        let play = event(EV_FF, id as u16, 1);
        let bytes = unsafe {
            std::slice::from_raw_parts(
                (&play as *const libc::input_event).cast::<u8>(),
                size_of::<libc::input_event>(),
            )
        };
        file.write_all(bytes).unwrap();
    }
    #[test]
    #[ignore = "only operates temporary uniquely named uinput pads"]
    fn uinput_short_reconnect_preserves_player_handles() {
        let mut pads = Gamepads::default();
        pads.negotiate(true, true, true);
        assert!(pads.supported, "{}", pads.error);
        pads.focus(7);
        pads.policy(Some(7), true);
        let generation = pads.generation;
        pads.input(&input(generation, 41, 1));
        let mut second = input(generation, 9, 1);
        second["slot"] = json!(1);
        pads.input(&second);
        let first = pads.devices[0].as_ref().unwrap();
        let sysname = first.sysname.clone();
        let mut reader = own_evdev(first).unwrap();
        let second_sysname = pads.devices[1].as_ref().unwrap().sysname.clone();
        let mut suspend = input(generation, 41, 2);
        suspend["connected"] = json!(false);
        suspend["suspended"] = json!(true);
        pads.input(&suspend);
        let paused = pads.devices[0].as_ref().unwrap();
        assert_eq!(paused.sysname, sysname);
        assert_eq!(paused.sample, Sample::default());
        assert!(paused.reconnect_until.is_some());
        assert!(drain(&mut reader).contains(&(EV_KEY, 0x130, 0)));
        pads.input(&input(generation, 41, 3));
        assert_eq!(pads.devices[0].as_ref().unwrap().sample, Sample::default());

        let mut resume = input(generation, 42, 1);
        resume["reconnect_from"] = json!(41);
        pads.input(&resume);
        let first = pads.devices[0].as_ref().unwrap();
        assert_eq!(first.sysname, sysname);
        assert_eq!(first.incarnation, 42);
        assert!(first.reconnect_until.is_none());
        assert!(drain(&mut reader).contains(&(EV_KEY, 0x130, 1)));
        assert_eq!(pads.devices[1].as_ref().unwrap().sysname, second_sysname);

        suspend["device_generation"] = json!(42);
        pads.input(&suspend);
        pads.devices[0].as_mut().unwrap().reconnect_until = Some(Instant::now());
        pads.poll();
        assert!(pads.devices[0].is_none());
        assert_eq!(pads.devices[1].as_ref().unwrap().sysname, second_sysname);
    }

    #[test]
    #[ignore = "creates only a uniquely named disposable uinput gamepad; requires uinput and own-node access"]
    fn uinput_roundtrip_and_real_rumble() {
        let mut pads = Gamepads::default();
        pads.negotiate(true, true, false);
        assert!(pads.supported, "{}", pads.error);
        pads.focus(7);
        pads.policy(Some(7), true);
        let generation = pads.generation;
        pads.input(&input(generation, 41, 1));
        let device = pads.devices[0].as_mut().expect("virtual device creation");
        let sysname = device.sysname.clone();
        let mut reader = own_evdev(device).expect("read only our uniquely identified event node");
        // Confirm identity before interacting: never use a physical input node.
        let name = std::fs::read_to_string(
            PathBuf::from("/sys/devices/virtual/input")
                .join(&sysname)
                .join("name"),
        )
        .unwrap();
        assert_eq!(name.trim(), device.name);
        let phys = std::fs::read_to_string(
            PathBuf::from("/sys/devices/virtual/input")
                .join(&sysname)
                .join("phys"),
        )
        .unwrap();
        assert!(phys.starts_with(&format!("waybridge/{}/", std::process::id())));
        device.write(Sample::default()).unwrap();
        drain(&mut reader);
        let mut pressed = input(generation, 41, 2);
        pressed["guide"] = json!(true);
        device.write(Sample::parse(&pressed).unwrap()).unwrap();
        let seen = drain(&mut reader);
        assert!(seen.contains(&(EV_KEY, 0x130, 1)));
        assert!(seen.contains(&(EV_KEY, 0x13c, 1)));
        assert!(seen.contains(&(EV_ABS, 0, 12345)));
        assert!(seen.contains(&(EV_ABS, 1, 32767)));
        assert!(seen.contains(&(EV_ABS, 17, -1)));
        assert_eq!(seen.last(), Some(&(EV_SYN, 0, 0)));
        let upload = std::thread::spawn(move || {
            let mut effect: libc::ff_effect = unsafe { zeroed() };
            effect.type_ = FF_RUMBLE;
            effect.id = -1;
            effect.replay.length = 250;
            let rumble = libc::ff_rumble_effect {
                strong_magnitude: 0x1234,
                weak_magnitude: 0xabcd,
            };
            unsafe {
                std::ptr::write_unaligned(
                    effect.u.as_mut_ptr().cast::<libc::ff_rumble_effect>(),
                    rumble,
                );
            }
            let result = unsafe {
                libc::ioctl(
                    reader.as_raw_fd(),
                    ioctl_code(1, b'E', 0x80, size_of::<libc::ff_effect>()),
                    &mut effect,
                )
            };
            assert_eq!(result, 0, "{}", io::Error::last_os_error());
            play_effect(&mut reader, effect.id);
            (reader, effect.id)
        });
        let deadline = Instant::now() + Duration::from_secs(3);
        let mut feedback = Vec::new();
        while Instant::now() < deadline {
            feedback.extend(pads.poll());
            if feedback.iter().any(|v| {
                v["type"] == "gamepad_rumble" && v["strong"] == 0x1234 && v["weak"] == 0xabcd
            }) {
                break;
            }
            std::thread::sleep(Duration::from_millis(2));
        }
        assert!(
            upload.is_finished(),
            "kernel FF upload acknowledgement timed out"
        );
        let (mut reader, effect_id) = upload.join().unwrap();
        let rumble = feedback
            .iter()
            .find(|v| v["type"] == "gamepad_rumble" && v["strong"] == 0x1234)
            .expect("real FF_RUMBLE feedback");
        assert_eq!(rumble["slot"], 0);
        assert_eq!(rumble["generation"], generation);
        assert_eq!(rumble["device_generation"], 41);
        let deadline = Instant::now() + Duration::from_secs(1);
        let mut stopped = false;
        while Instant::now() < deadline {
            if pads
                .poll()
                .iter()
                .any(|v| v["type"] == "gamepad_rumble" && v["strong"] == 0 && v["weak"] == 0)
            {
                stopped = true;
                break;
            }
            std::thread::sleep(Duration::from_millis(2));
        }
        assert!(stopped, "finite FF_RUMBLE effect did not expire");
        // Stale/replayed or malformed snapshots cannot change the held state.
        let original = pads.devices[0].as_ref().unwrap().sample;
        pads.input(&input(generation - 1, 42, 2));
        let mut replay = input(generation, 41, 1);
        replay["buttons"] = json!(0);
        pads.input(&replay);
        assert_eq!(pads.devices[0].as_ref().unwrap().sample, original);
        play_effect(&mut reader, effect_id);
        assert!(pads
            .poll()
            .iter()
            .any(|v| v["type"] == "gamepad_rumble" && v["strong"] == 0x1234));
        pads.focus(0);
        assert_eq!(pads.devices[0].as_ref().unwrap().sysname, sysname);
        assert_eq!(pads.devices[0].as_ref().unwrap().sample, Sample::default());
        let neutral = drain(&mut reader);
        assert!(neutral.contains(&(EV_KEY, 0x130, 0)));
        assert!(neutral.contains(&(EV_ABS, 0, 0)));
        assert!(pads
            .poll()
            .iter()
            .any(|v| v["type"] == "gamepad_rumble" && v["strong"] == 0 && v["weak"] == 0));
        play_effect(&mut reader, effect_id);
        assert!(pads.poll().is_empty(), "rumble played while unfocused");
        // Queue an effect immediately before regrant to exercise the handoff
        // drain as well as the regular inactive polling path.
        play_effect(&mut reader, effect_id);
        pads.focus(7);
        assert!(pads.poll().iter().all(|v| v["type"] != "gamepad_rumble"));
        let newer = pads.generation;
        assert!(newer > generation);
        pads.input(&input(newer, 41, 2));
        assert_eq!(pads.devices[0].as_ref().unwrap().sysname, sysname);
        play_effect(&mut reader, effect_id);
        assert!(pads.poll().iter().any(|v| v["type"] == "gamepad_rumble"
            && v["strong"] == 0x1234
            && v["generation"] == newer));
        pads.focus(0);
        let mut unplug = input(newer, 41, 3);
        unplug["connected"] = json!(false);
        pads.input(&unplug);
        pads.focus(7);
        let generation = pads.generation;
        pads.input(&input(generation, 41, 999));
        assert!(
            pads.devices[0].is_none(),
            "stale connected snapshot resurrected unplugged device"
        );
        pads.input(&input(generation, 42, 1));
        assert_eq!(pads.devices[0].as_ref().unwrap().incarnation, 42);
        let replacement = pads.devices[0].as_ref().unwrap().sysname.clone();
        pads.input(&unplug);
        assert_eq!(pads.devices[0].as_ref().unwrap().sysname, replacement);
        pads.focus(0);
        assert!(pads.devices[0].is_some());
        pads.set_enabled(false);
        assert!(pads.devices.iter().all(Option::is_none));
        assert!(!PathBuf::from("/sys/devices/virtual/input")
            .join(sysname)
            .exists());
    }
}
