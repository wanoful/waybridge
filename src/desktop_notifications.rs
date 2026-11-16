//! Explicit, non-replacing freedesktop notification provider and private session bridge.
//! The provider is a separate opt-in user service, never an observer of another server.
use anyhow::{bail, Context, Result};
use base64::{engine::general_purpose::STANDARD as B64, Engine};
use image::ImageEncoder;
use serde_json::{json, Value};
use std::{
    collections::{BTreeMap, HashMap},
    fs,
    os::unix::{
        fs::{MetadataExt, PermissionsExt},
        net::UnixDatagram,
    },
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};
use zbus::{blocking::Connection, fdo, message::Header, zvariant::OwnedValue};

const NAME: &str = "org.freedesktop.Notifications";
const OBJECT: &str = "/org/freedesktop/Notifications";
const MAX_PACKET: usize = 60 * 1024;
const MAX_ACTIVE: usize = 64;
const MAX_ICON: usize = 32 * 1024;

fn nonce() -> String {
    use ring::rand::{SecureRandom, SystemRandom};
    let mut bytes = [0u8; 16];
    SystemRandom::new()
        .fill(&mut bytes)
        .expect("system randomness");
    bytes.iter().map(|b| format!("{b:02x}")).collect()
}
fn private_runtime(path: &Path) -> Result<()> {
    let metadata = fs::symlink_metadata(path)?;
    if !metadata.is_dir()
        || metadata.uid() != unsafe { libc::geteuid() }
        || metadata.permissions().mode() & 0o077 != 0
    {
        bail!("notification runtime must be a private directory owned by the session user");
    }
    Ok(())
}
fn send(socket: &UnixDatagram, path: &Path, value: &Value) -> Result<()> {
    let bytes = serde_json::to_vec(value)?;
    if bytes.len() > MAX_PACKET {
        bail!("notification message exceeds limit");
    }
    socket.send_to(&bytes, path)?;
    Ok(())
}

/// Nonblocking IPC endpoint. All D-Bus, icon lookup and notification lifetime work
/// happens in the provider process, away from Wayland dispatch and rendering.
pub struct Bridge {
    socket: UnixDatagram,
    path: PathBuf,
    provider: Option<(PathBuf, String)>,
    binding: String,
    enabled: bool,
    provider_seen: Instant,
    diagnosis: String,
    diagnostics: std::sync::mpsc::Receiver<String>,
    stopped: Arc<std::sync::atomic::AtomicBool>,
}
impl Bridge {
    pub fn new(runtime: &Path, address: &str) -> Result<Self> {
        private_runtime(runtime)?;
        let path = runtime.join("notifications.sock");
        let socket = UnixDatagram::bind(&path)?;
        fs::set_permissions(&path, fs::Permissions::from_mode(0o600))?;
        socket.set_nonblocking(true)?;
        let (updates, diagnostics) = std::sync::mpsc::sync_channel(1);
        let stopped = Arc::new(std::sync::atomic::AtomicBool::new(false));
        let worker_stop = stopped.clone();
        let address = address.to_owned();
        std::thread::spawn(move || {
            let bus = crate::session_services::connect_bus(&address, Duration::from_secs(1));
            while !worker_stop.load(std::sync::atomic::Ordering::Relaxed) {
                let diagnosis = if let Ok(bus) = &bus {
                    match bus.call_method(
                        Some("org.freedesktop.DBus"),
                        "/org/freedesktop/DBus",
                        Some("org.freedesktop.DBus"),
                        "GetNameOwner",
                        &(NAME,),
                    ) {
                        Ok(_) => "provider_conflict",
                        Err(zbus::Error::MethodError(name, _, _))
                            if name.as_str() == "org.freedesktop.DBus.Error.NameHasNoOwner" =>
                        {
                            "provider_not_selected"
                        }
                        Err(_) => "provider_diagnostic_unavailable",
                    }
                } else {
                    "provider_diagnostic_unavailable"
                };
                let _ = updates.try_send(diagnosis.to_owned());
                std::thread::sleep(Duration::from_secs(1));
            }
        });
        Ok(Self {
            socket,
            path,
            provider: None,
            binding: nonce(),
            enabled: false,
            provider_seen: Instant::now(),
            diagnosis: "provider_not_selected".into(),
            diagnostics,
            stopped,
        })
    }
    fn state(&self) -> Value {
        json!({"type":"notification_state", "binding":self.binding,"enabled":self.enabled})
    }
    pub fn connection(&mut self, enabled: bool) {
        self.binding = nonce();
        self.enabled = enabled;
        if let Some((path, _)) = &self.provider {
            let _ = send(&self.socket, path, &self.state());
        }
    }
    pub fn enabled(&mut self, enabled: bool) {
        if self.enabled != enabled {
            self.connection(enabled);
        }
    }
    pub fn status(&self) -> Value {
        let available = self.provider.is_some();
        let message = if available {
            "Waybridge 标准通知服务已接入"
        } else {
            match self.diagnosis.as_str() {
                "provider_conflict" => "用户总线已有通知服务。请在会话组件配置中明确选择通知服务；Waybridge 不会抢占其他服务。",
                "provider_lost" => "Waybridge 通知服务已断开；旧通知及动作已失效。请检查 waybridge-notifications.service。",
                "provider_diagnostic_unavailable" => "无法检查通知服务；请检查用户总线与 waybridge-notifications.service。",
                _ => "尚未选用 Waybridge 标准通知服务；请显式启用 waybridge-notifications.service。",
            }
        };
        json!({"type":"notification_status","available":available,"reason":if available {"provider_connected"} else {&self.diagnosis},"message":message})
    }
    pub fn receive(&mut self, mut publish: impl FnMut(Value)) {
        while let Ok(diagnosis) = self.diagnostics.try_recv() {
            if self.provider.is_none() && self.diagnosis != diagnosis {
                self.diagnosis = diagnosis;
                publish(self.status());
            }
        }
        if self.provider.is_some() && self.provider_seen.elapsed() > Duration::from_secs(3) {
            self.provider = None;
            self.diagnosis = "provider_lost".into();
            publish(json!({"type":"notification_reset"}));
            publish(self.status());
        }
        let mut bytes = [0u8; MAX_PACKET + 1];
        for _ in 0..64 {
            let Ok((size, address)) = self.socket.recv_from(&mut bytes) else {
                break;
            };
            let Some(path) = address.as_pathname() else {
                continue;
            };
            if size > MAX_PACKET || path.parent() != self.path.parent() {
                continue;
            }
            let Ok(metadata) = fs::symlink_metadata(path) else {
                continue;
            };
            if metadata.uid() != unsafe { libc::geteuid() } {
                continue;
            }
            let Ok(message) = serde_json::from_slice::<Value>(&bytes[..size]) else {
                continue;
            };
            if message["type"] == "notification_register" {
                let Some(instance) = message["instance"].as_str().filter(|s| s.len() == 32) else {
                    continue;
                };
                if self
                    .provider
                    .as_ref()
                    .is_some_and(|p| p.0 != path || p.1 != instance)
                {
                    publish(json!({"type":"notification_reset"}));
                }
                let changed = self
                    .provider
                    .as_ref()
                    .is_none_or(|p| p.0 != path || p.1 != instance);
                self.provider = Some((path.into(), instance.into()));
                self.provider_seen = Instant::now();
                if changed {
                    publish(self.status());
                }
                let _ = send(&self.socket, path, &self.state());
            } else if self
                .provider
                .as_ref()
                .is_some_and(|p| p.0 == path && message["instance"] == p.1)
                && message["binding"] == self.binding
                && matches!(
                    message["type"].as_str(),
                    Some("notification" | "notification_close")
                )
            {
                if self.enabled {
                    publish(message);
                }
            }
        }
    }
    pub fn feedback(&mut self, value: &Value) {
        if !self.enabled || value["binding"] != self.binding {
            return;
        }
        if let Some((path, instance)) = &self.provider {
            if value["instance"] == *instance {
                let _ = send(&self.socket, path, value);
            }
        }
    }
}
impl Drop for Bridge {
    fn drop(&mut self) {
        self.stopped
            .store(true, std::sync::atomic::Ordering::Relaxed);
        let _ = fs::remove_file(&self.path);
    }
}

struct Entry {
    owner: String,
    revision: u64,
    actions: Vec<String>,
    deadline: Option<Instant>,
}
struct Provider {
    socket: UnixDatagram,
    bridge: PathBuf,
    instance: String,
    binding: String,
    enabled: bool,
    last_state: Instant,
    next: u32,
    revision: u64,
    entries: BTreeMap<u32, Entry>,
    rate: (Instant, u32),
}
impl Provider {
    fn envelope(&self, kind: &str, id: u32, revision: u64) -> Value {
        json!({"type":kind,"instance":self.instance,"binding":self.binding,"id":id,"revision":revision})
    }
    fn close(&mut self, bus: &Connection, id: u32, reason: u32) {
        if let Some(entry) = self.entries.remove(&id) {
            let _ = send(
                &self.socket,
                &self.bridge,
                &self.envelope("notification_close", id, entry.revision),
            );
            // Signals go only to the originating unique owner; never another app
            // which subsequently acquires the same well-known application name.
            let _ = bus.emit_signal(
                Some(entry.owner.as_str()),
                OBJECT,
                NAME,
                "NotificationClosed",
                &(id, reason),
            );
        }
    }
    fn reset(&mut self, bus: &Connection) {
        for id in self.entries.keys().copied().collect::<Vec<_>>() {
            self.close(bus, id, 4);
        }
    }
}
struct Notifications {
    state: Arc<Mutex<Provider>>,
    bus: Connection,
}

fn invalid(message: &str) -> fdo::Error {
    fdo::Error::InvalidArgs(message.into())
}
fn owner(header: &Header<'_>) -> fdo::Result<String> {
    header
        .sender()
        .map(ToString::to_string)
        .ok_or_else(|| invalid("missing notification sender"))
}
fn text(value: &str, maximum: usize) -> bool {
    value.len() <= maximum && !value.contains('\0')
}
fn image_data(hints: &HashMap<String, OwnedValue>) -> Option<String> {
    let raw = hints
        .get("image-data")
        .or_else(|| hints.get("image_data"))?;
    let (width, height, stride, alpha, bits, channels, data): (
        i32,
        i32,
        i32,
        bool,
        i32,
        i32,
        Vec<u8>,
    ) = raw.try_clone().ok()?.try_into().ok()?;
    if !(1..=256).contains(&width)
        || !(1..=256).contains(&height)
        || bits != 8
        || channels != if alpha { 4 } else { 3 }
        || stride < width.checked_mul(channels)?
        || stride > 4096
        || data.len() != usize::try_from(stride.checked_mul(height)?).ok()?
    {
        return None;
    }
    let mut pixels = Vec::with_capacity(width as usize * height as usize * 4);
    for row in data.chunks_exact(stride as usize) {
        for pixel in row[..width as usize * channels as usize].chunks_exact(channels as usize) {
            pixels.extend_from_slice(&pixel[..3]);
            pixels.push(if alpha { pixel[3] } else { 255 });
        }
    }
    let image = image::RgbaImage::from_raw(width as u32, height as u32, pixels)?;
    let thumb = image::imageops::thumbnail(&image, 128, 128);
    let mut image = image::RgbaImage::new(128, 128);
    image::imageops::overlay(
        &mut image,
        &thumb,
        i64::from((128 - thumb.width()) / 2),
        i64::from((128 - thumb.height()) / 2),
    );
    let mut png = Vec::new();
    image::codecs::png::PngEncoder::new(&mut png)
        .write_image(
            image.as_raw(),
            image.width(),
            image.height(),
            image::ExtendedColorType::Rgba8,
        )
        .ok()?;
    (png.len() <= MAX_ICON).then(|| B64.encode(png))
}

#[zbus::interface(name = "org.freedesktop.Notifications")]
impl Notifications {
    fn get_capabilities(&self) -> Vec<&str> {
        vec!["body", "actions", "icon-static"]
    }
    fn get_server_information(&self) -> (&str, &str, &str, &str) {
        ("Waybridge", "Waybridge", env!("CARGO_PKG_VERSION"), "1.2")
    }
    #[allow(clippy::too_many_arguments)]
    fn notify(
        &self,
        app_name: &str,
        replaces_id: u32,
        app_icon: &str,
        summary: &str,
        body: &str,
        actions: Vec<String>,
        hints: HashMap<String, OwnedValue>,
        expire_timeout: i32,
        #[zbus(header)] header: Header<'_>,
    ) -> fdo::Result<u32> {
        if !text(app_name, 256)
            || !text(summary, 1024)
            || !text(body, 8192)
            || !text(app_icon, 4096)
            || actions.len() > 10
            || actions.len() % 2 != 0
            || actions.iter().any(|a| !text(a, 256))
            || hints.len() > 32
        {
            return Err(invalid("notification exceeds text, action or hint limits"));
        }
        let action_pairs: Vec<_> = actions
            .chunks_exact(2)
            .map(|a| json!({"key":a[0],"label":a[1]}))
            .collect();
        let action_keys: Vec<_> = actions.chunks_exact(2).map(|a| a[0].clone()).collect();
        if action_keys
            .iter()
            .enumerate()
            .any(|(i, key)| key.is_empty() || action_keys[..i].contains(key))
            || action_keys
                .iter()
                .filter(|key| key.as_str() != "default")
                .count()
                > 4
        {
            return Err(invalid(
                "at most four buttons and one default action are supported",
            ));
        }
        let sender = owner(&header)?;
        let admitted_binding = {
            let mut state = self.state.lock().unwrap();
            if !state.enabled || state.last_state.elapsed() > Duration::from_secs(3) {
                return Err(fdo::Error::Failed(
                    "Waybridge notification forwarding has no enabled, capable viewer".into(),
                ));
            }
            if state.rate.0.elapsed() >= Duration::from_secs(1) {
                state.rate = (Instant::now(), 0);
            }
            if state.rate.1 >= 16 {
                return Err(fdo::Error::LimitsExceeded(
                    "notification update rate exceeded".into(),
                ));
            }
            state.rate.1 += 1;
            if replaces_id != 0
                && state
                    .entries
                    .get(&replaces_id)
                    .is_some_and(|entry| entry.owner != sender)
            {
                return Err(fdo::Error::AccessDenied(
                    "notification belongs to another unique bus owner".into(),
                ));
            }
            if state.entries.len() >= MAX_ACTIVE && !state.entries.contains_key(&replaces_id) {
                return Err(fdo::Error::LimitsExceeded(
                    "too many active notifications".into(),
                ));
            }
            state.binding.clone()
        };
        // Icon decoding and theme lookup run only in this explicit helper.
        // Admission is cheap and precedes I/O. Do not hold the state lock here:
        // expiry, viewer detach and the provider heartbeat must keep progressing.
        let desktop = hints
            .get("desktop-entry")
            .and_then(|v| <&str>::try_from(v).ok())
            .unwrap_or("");
        let png = image_data(&hints)
            .or_else(|| {
                hints
                    .get("image-path")
                    .and_then(|value| <&str>::try_from(value).ok())
                    .filter(|value| text(value, 4096))
                    .and_then(|value| {
                        if let Some(path) = value.strip_prefix("file://") {
                            let path = path
                                .strip_prefix("localhost/")
                                .map(|p| format!("/{p}"))
                                .unwrap_or_else(|| path.to_owned());
                            if !path.starts_with('/') {
                                return None;
                            }
                            let decoded = percent_encoding::percent_decode_str(&path)
                                .decode_utf8()
                                .ok()?;
                            crate::app_icon::notification_icon(&decoded, "")
                        } else {
                            crate::app_icon::notification_icon(value, "")
                        }
                    })
                    .filter(|bytes| bytes.len() <= MAX_ICON)
                    .map(|bytes| B64.encode(bytes))
            })
            .or_else(|| {
                crate::app_icon::notification_icon(app_icon, desktop)
                    .filter(|bytes| bytes.len() <= MAX_ICON)
                    .map(|bytes| B64.encode(bytes))
            });
        let mut state = self.state.lock().unwrap();
        if !state.enabled
            || state.binding != admitted_binding
            || state.last_state.elapsed() > Duration::from_secs(3)
        {
            return Err(fdo::Error::Failed(
                "Waybridge notification forwarding has no enabled, capable viewer".into(),
            ));
        }
        if replaces_id != 0
            && state
                .entries
                .get(&replaces_id)
                .is_some_and(|entry| entry.owner != sender)
        {
            return Err(fdo::Error::AccessDenied(
                "notification belongs to another unique bus owner".into(),
            ));
        }
        let id = if replaces_id != 0 && state.entries.contains_key(&replaces_id) {
            replaces_id
        } else {
            if state.entries.len() >= MAX_ACTIVE {
                return Err(fdo::Error::LimitsExceeded(
                    "too many active notifications".into(),
                ));
            }
            loop {
                state.next = state.next.wrapping_add(1).max(1);
                if !state.entries.contains_key(&state.next) {
                    break state.next;
                }
            }
        };
        state.revision += 1;
        let revision = state.revision;
        // The server chooses a short default; explicit zero means resident until
        // application/user closure. Active count is bounded in both cases.
        let timeout = if expire_timeout < 0 {
            10000
        } else {
            expire_timeout.clamp(0, 86400000)
        };
        let deadline =
            (timeout > 0).then(|| Instant::now() + Duration::from_millis(timeout as u64));
        let mut message = state.envelope("notification", id, revision);
        message["app_name"] = json!(app_name);
        message["summary"] = json!(summary);
        message["body"] = json!(body);
        message["actions"] = json!(action_pairs);
        message["timeout_ms"] = json!(timeout);
        message["png"] = json!(png);
        send(&state.socket, &state.bridge, &message)
            .map_err(|_| fdo::Error::Failed("notification session bridge unavailable".into()))?;
        state.entries.insert(
            id,
            Entry {
                owner: sender,
                revision,
                actions: action_keys,
                deadline,
            },
        );
        Ok(id)
    }
    fn close_notification(&self, id: u32, #[zbus(header)] header: Header<'_>) -> fdo::Result<()> {
        let sender = owner(&header)?;
        let mut state = self.state.lock().unwrap();
        if !state.entries.contains_key(&id) {
            return Err(invalid("notification ID does not exist"));
        }
        if state
            .entries
            .get(&id)
            .is_some_and(|entry| entry.owner != sender)
        {
            return Err(fdo::Error::AccessDenied(
                "notification belongs to another unique bus owner".into(),
            ));
        }
        state.close(&self.bus, id, 3);
        Ok(())
    }
    #[zbus(signal)]
    async fn action_invoked(
        emitter: &zbus::object_server::SignalEmitter<'_>,
        id: u32,
        action_key: &str,
    ) -> zbus::Result<()>;
    #[zbus(signal)]
    async fn notification_closed(
        emitter: &zbus::object_server::SignalEmitter<'_>,
        id: u32,
        reason: u32,
    ) -> zbus::Result<()>;
}

/// Invoked only by `--notification-provider`, normally from the explicitly
/// selected PartOf user unit. Never sets ReplaceExisting or AllowReplacement.
pub fn run_provider() -> Result<()> {
    let runtime = std::env::var_os("WAYBRIDGE_SESSION_ID")
        .map(PathBuf::from)
        .context("notification provider requires an active WAYBRIDGE_SESSION_ID")?;
    private_runtime(&runtime)?;
    let address = std::env::var("DBUS_SESSION_BUS_ADDRESS")
        .context("notification provider requires the user bus")?;
    let bus = crate::session_services::connect_bus(&address, Duration::from_secs(2))?;
    let reply =
        bus.request_name_with_flags(NAME, zbus::fdo::RequestNameFlags::DoNotQueue.into())?;
    if reply != zbus::fdo::RequestNameReply::PrimaryOwner {
        bail!("org.freedesktop.Notifications already has a provider; select Waybridge explicitly in your session service configuration, without running another notification server");
    }
    let instance = nonce();
    let path = runtime.join(format!("notify-{}.sock", &instance[..12]));
    let socket = UnixDatagram::bind(&path)?;
    fs::set_permissions(&path, fs::Permissions::from_mode(0o600))?;
    socket.set_nonblocking(true)?;
    struct SocketFile(PathBuf);
    impl Drop for SocketFile {
        fn drop(&mut self) {
            let _ = fs::remove_file(&self.0);
        }
    }
    let _cleanup = SocketFile(path);
    let state = Arc::new(Mutex::new(Provider {
        socket,
        bridge: runtime.join("notifications.sock"),
        instance,
        binding: String::new(),
        enabled: false,
        last_state: Instant::now(),
        next: 0,
        revision: 0,
        entries: BTreeMap::new(),
        rate: (Instant::now(), 0),
    }));
    bus.object_server().at(
        OBJECT,
        Notifications {
            state: state.clone(),
            bus: bus.clone(),
        },
    )?;
    crate::shutdown::install();
    let mut last_register = Instant::now() - Duration::from_secs(2);
    let mut bytes = [0u8; MAX_PACKET + 1];
    while !crate::shutdown::requested() {
        {
            let mut state = state.lock().unwrap();
            if last_register.elapsed() >= Duration::from_secs(1) {
                send(
                    &state.socket,
                    &state.bridge,
                    &json!({"type":"notification_register","instance":state.instance}),
                )
                .context("Waybridge notification session bridge closed")?;
                last_register = Instant::now();
            }
            for _ in 0..64 {
                let Ok((size, address)) = state.socket.recv_from(&mut bytes) else {
                    break;
                };
                if size > MAX_PACKET || address.as_pathname() != Some(state.bridge.as_path()) {
                    continue;
                }
                let Ok(value) = serde_json::from_slice::<Value>(&bytes[..size]) else {
                    continue;
                };
                if value["type"] == "notification_state" {
                    let binding = value["binding"].as_str().unwrap_or("");
                    if state.binding != binding {
                        state.reset(&bus);
                        state.binding = binding.into();
                    }
                    state.enabled = value["enabled"] == true;
                    state.last_state = Instant::now();
                } else if value["type"] == "notification_feedback"
                    && value["binding"] == state.binding
                    && value["instance"] == state.instance
                {
                    let id = value["id"]
                        .as_u64()
                        .and_then(|v| u32::try_from(v).ok())
                        .unwrap_or(0);
                    if let Some(entry) = state.entries.get(&id) {
                        if value["revision"] != entry.revision
                            || entry.deadline.is_some_and(|d| d <= Instant::now())
                        {
                            continue;
                        }
                        let alive: bool = bus
                            .call_method(
                                Some("org.freedesktop.DBus"),
                                "/org/freedesktop/DBus",
                                Some("org.freedesktop.DBus"),
                                "NameHasOwner",
                                &(entry.owner.as_str(),),
                            )?
                            .body()
                            .deserialize()?;
                        if !alive {
                            state.close(&bus, id, 4);
                            continue;
                        }
                        if let Some(action) = value["action"]
                            .as_str()
                            .filter(|key| entry.actions.iter().any(|a| a == key))
                        {
                            let _ = bus.emit_signal(
                                Some(entry.owner.as_str()),
                                OBJECT,
                                NAME,
                                "ActionInvoked",
                                &(id, action),
                            );
                            state.close(&bus, id, 2);
                        } else if let Some(reason) =
                            value["reason"].as_u64().filter(|r| matches!(r, 2 | 4))
                        {
                            state.close(&bus, id, reason as u32);
                        }
                    }
                }
            }
            let expired: Vec<_> = state
                .entries
                .iter()
                .filter_map(|(&id, entry)| {
                    entry
                        .deadline
                        .is_some_and(|d| d <= Instant::now())
                        .then_some(id)
                })
                .collect();
            for id in expired {
                state.close(&bus, id, 1);
            }
            if state.last_state.elapsed() > Duration::from_secs(3) {
                state.enabled = false;
                state.reset(&bus);
            }
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    state.lock().unwrap().reset(&bus);
    Ok(())
}
