//! Standard user services and shared application data for the sole graphical session.
use anyhow::{bail, Context, Result};
use serde::Serialize;
use std::{
    collections::BTreeMap,
    fs,
    os::unix::fs::PermissionsExt,
    path::{Path, PathBuf},
    process::Command,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};
use zbus::blocking::Connection;

fn decode_bus_value(value: &str) -> Result<std::ffi::OsString> {
    use std::os::unix::ffi::OsStringExt;
    let mut decoded = Vec::with_capacity(value.len());
    let mut bytes = value.bytes();
    while let Some(byte) = bytes.next() {
        if byte == b'%' {
            let hi = bytes.next().and_then(|v| (v as char).to_digit(16));
            let lo = bytes.next().and_then(|v| (v as char).to_digit(16));
            let (Some(hi), Some(lo)) = (hi, lo) else {
                bail!("invalid percent escape in user D-Bus address");
            };
            let byte = ((hi << 4) | lo) as u8;
            if byte == 0 {
                bail!("NUL in user D-Bus address");
            }
            decoded.push(byte);
        } else if matches!(byte, b'-' | b'0'..=b'9' | b'A'..=b'Z' | b'a'..=b'z' | b'_' | b'/' | b'.' | b'\\' | b'*')
        {
            decoded.push(byte);
        } else {
            bail!("unescaped byte in user D-Bus address");
        }
    }
    Ok(std::ffi::OsString::from_vec(decoded))
}

pub fn connect_bus(address: &str, method_timeout: Duration) -> Result<zbus::blocking::Connection> {
    use zbus::address::{
        transport::{Unix, UnixSocket},
        Address, Transport,
    };
    // zbus 5.19's Unix address parser does not decode %XX path bytes. Construct
    // a typed address directly, preserving the broker GUID and decoding once.
    let options = address
        .strip_prefix("unix:")
        .context("user D-Bus must use a Unix socket")?;
    let mut socket = None;
    let mut guid = None;
    for option in options.split(',') {
        let (key, value) = option
            .split_once('=')
            .context("invalid user D-Bus address option")?;
        match key {
            "path" | "abstract" => {
                if socket.is_some() {
                    bail!("multiple user D-Bus socket paths");
                }
                let value = decode_bus_value(value)?;
                socket = Some(if key == "path" {
                    UnixSocket::File(PathBuf::from(value))
                } else {
                    UnixSocket::Abstract(value)
                });
            }
            "guid" if guid.is_none() => guid = Some(value),
            _ => bail!("unsupported user D-Bus address option"),
        }
    }
    let mut address = Address::new(Transport::Unix(Unix::new(
        socket.context("missing user D-Bus socket path")?,
    )));
    if let Some(guid) = guid {
        address = address.set_guid(zbus::Guid::try_from(guid)?)?;
    }
    Ok(zbus::blocking::connection::Builder::address(address)?
        .method_timeout(method_timeout)
        .build()?)
}

/// Deliberately independent of graphical-session.target and desktop-specific targets.
const TARGET: &str = "waybridge-session.target";
const SHUTDOWN: &str = "waybridge-session-shutdown.target";
const ACTIVATION_KEYS: &[&str] = &[
    "WAYLAND_DISPLAY",
    "DISPLAY",
    "XDG_SESSION_TYPE",
    "XDG_CURRENT_DESKTOP",
    "XDG_SESSION_DESKTOP",
    "DESKTOP_SESSION",
    "XDG_RUNTIME_DIR",
    "DBUS_SESSION_BUS_ADDRESS",
    "WAYBRIDGE_SESSION_ID",
];

#[derive(Clone, Serialize)]
struct State {
    name: &'static str,
    status: &'static str,
    detail: String,
}

pub struct Services {
    runtime: tempfile::TempDir,
    address: String,
    bus: Connection,
    stopped: Arc<AtomicBool>,
    worker: Option<JoinHandle<Result<()>>>,
}

fn update_bus(bus: &Connection, values: &BTreeMap<String, Option<String>>) -> Result<()> {
    // The D-Bus daemon API has no unset operation. Empty strings disable stale
    // display addresses there; systemd below retains genuine unset semantics.
    let values: BTreeMap<_, _> = values
        .iter()
        .map(|(key, value)| (key, value.as_deref().unwrap_or("")))
        .collect();
    bus.call_method(
        Some("org.freedesktop.DBus"),
        "/org/freedesktop/DBus",
        Some("org.freedesktop.DBus"),
        "UpdateActivationEnvironment",
        &(values,),
    )?;
    Ok(())
}

fn manager_environment(bus: &Connection) -> Result<BTreeMap<String, String>> {
    let environment: zbus::zvariant::OwnedValue = bus
        .call_method(
            Some("org.freedesktop.systemd1"),
            "/org/freedesktop/systemd1",
            Some("org.freedesktop.DBus.Properties"),
            "Get",
            &("org.freedesktop.systemd1.Manager", "Environment"),
        )?
        .body()
        .deserialize()?;
    let values: Vec<String> = environment.try_into()?;
    Ok(values
        .into_iter()
        .filter_map(|value| {
            value
                .split_once('=')
                .map(|(key, value)| (key.into(), value.into()))
        })
        .collect())
}

fn manager_call(
    bus: &Connection,
    method: &str,
    body: &(impl serde::Serialize + zbus::zvariant::DynamicType),
) -> Result<zbus::Message> {
    Ok(bus.call_method(
        Some("org.freedesktop.systemd1"),
        "/org/freedesktop/systemd1",
        Some("org.freedesktop.systemd1.Manager"),
        method,
        body,
    )?)
}

fn update_manager(bus: &Connection, values: &BTreeMap<String, Option<String>>) -> Result<()> {
    let set: Vec<_> = values
        .iter()
        .filter_map(|(key, value)| value.as_ref().map(|value| format!("{key}={value}")))
        .collect();
    let unset: Vec<_> = values
        .iter()
        .filter_map(|(key, value)| value.is_none().then_some(key))
        .collect();
    if !set.is_empty() {
        manager_call(bus, "SetEnvironment", &(set,))?;
    }
    if !unset.is_empty() {
        manager_call(bus, "UnsetEnvironment", &(unset,))?;
    }
    Ok(())
}

fn unit_path(bus: &Connection, name: &str) -> Result<zbus::zvariant::OwnedObjectPath> {
    Ok(manager_call(bus, "GetUnit", &(name,))?
        .body()
        .deserialize()?)
}

fn unit_property<T>(bus: &Connection, name: &str, key: &str) -> Result<T>
where
    T: TryFrom<zbus::zvariant::OwnedValue>,
    T::Error: std::error::Error + Send + Sync + 'static,
{
    let path = unit_path(bus, name)?;
    let value: zbus::zvariant::OwnedValue = bus
        .call_method(
            Some("org.freedesktop.systemd1"),
            path.as_str(),
            Some("org.freedesktop.DBus.Properties"),
            "Get",
            &("org.freedesktop.systemd1.Unit", key),
        )?
        .body()
        .deserialize()?;
    Ok(value.try_into()?)
}

fn active(bus: &Connection, name: &str) -> Result<String> {
    match unit_property(bus, name, "ActiveState") {
        Err(error)
            if matches!(error.downcast_ref::<zbus::Error>(), Some(zbus::Error::MethodError(name, _, _))
            if name.as_str() == "org.freedesktop.systemd1.NoSuchUnit") =>
        {
            Ok("inactive".into())
        }
        value => value,
    }
}

// Diagnostics must never prevent service shutdown or activation-environment
// cleanup. In particular, a full/unwritable runtime must not strand services
// pointing at a compositor that has already gone away.
fn report(path: &Path, lifecycle: &str, detail: &str, bus: &Connection) {
    if let Err(error) = write_report(path, lifecycle, detail, bus) {
        tracing::warn!(%error, %lifecycle, path=%path.display(), "cannot write desktop-service status");
    }
}

fn write_report(path: &Path, lifecycle: &str, detail: &str, bus: &Connection) -> Result<()> {
    let names: Vec<String> = bus
        .call_method(
            Some("org.freedesktop.DBus"),
            "/org/freedesktop/DBus",
            Some("org.freedesktop.DBus"),
            "ListNames",
            &(),
        )?
        .body()
        .deserialize()?;
    let mut states = vec![State {
        name: "session-lifecycle",
        status: match lifecycle {
            "ready" => "ready",
            "starting" => "starting",
            "stopping" => "stopping",
            "stopped" => "stopped",
            _ => "unavailable",
        },
        detail: detail.into(),
    }];
    // Read-only observations of standard interfaces; never activate a provider.
    for (name, interface) in [
        ("secret-service", "org.freedesktop.secrets"),
        ("desktop-portal", "org.freedesktop.portal.Desktop"),
    ] {
        states.push(State { name, status: if names.iter().any(|n| n == interface) { "present" } else { "not-running" },
            detail: "D-Bus name observation only; component selection and activation belong to user configuration".into() });
    }
    states.push(State {
        name: "screencast",
        status: "unsupported",
        detail: "Waybridge ScreenCast backend is not implemented".into(),
    });
    states.push(State {
        name: "flatpak-documents",
        status: "unverified",
        detail: "sandbox document access requires application-specific validation".into(),
    });
    let mut file = tempfile::NamedTempFile::new_in(path.parent().context("status parent")?)?;
    serde_json::to_writer_pretty(
        file.as_file_mut(),
        &serde_json::json!({
            "version": 3, "desktop": "Waybridge", "scope": "user", "data_policy": "shared",
            "target": TARGET, "services": states,
            "activation_environment": {"systemd_unset": true,
                "dbus_unset": false, "dbus_unset_behavior": "empty-value; D-Bus UpdateActivationEnvironment cannot remove variables"},
        }),
    )?;
    file.persist(path).map_err(|error| error.error)?;
    Ok(())
}

/// Snapshot only units explicitly declaring PartOf=Waybridge (recursively).
/// Waiting for the target job alone is insufficient: stop ordering can finish
/// that job before all propagated service stops have completed.
fn owned_units(bus: &Connection, target: &str, units: &mut Vec<String>) -> Result<()> {
    if units.iter().any(|unit| unit == target) {
        return Ok(());
    }
    units.push(target.into());
    let children: Vec<String> = unit_property(bus, target, "ConsistsOf")?;
    for child in children {
        owned_units(bus, &child, units)?;
    }
    Ok(())
}

fn previous_display_live(previous: &BTreeMap<String, String>, key: &str) -> bool {
    use std::os::unix::net::UnixStream;
    let wayland = previous
        .get("WAYLAND_DISPLAY")
        .filter(|value| !value.is_empty())
        .and_then(|value| {
            let path = Path::new(value);
            if path.is_absolute() {
                Some(path.to_path_buf())
            } else {
                previous
                    .get("XDG_RUNTIME_DIR")
                    .map(|runtime| Path::new(runtime).join(path))
            }
        });
    let x11 = previous
        .get("DISPLAY")
        .and_then(|value| value.strip_prefix(':'))
        .and_then(|value| value.split('.').next())
        .filter(|number| !number.is_empty() && number.bytes().all(|v| v.is_ascii_digit()))
        .map(|number| PathBuf::from(format!("/tmp/.X11-unix/X{number}")));
    let path = if key == "WAYLAND_DISPLAY" {
        wayland
    } else {
        x11
    };
    path.is_some_and(|path| UnixStream::connect(path).is_ok())
}

fn cleanup_environment(
    bus: &Connection,
    previous: Option<&BTreeMap<String, String>>,
    written: &BTreeMap<String, Option<String>>,
) -> Result<()> {
    let Some(previous) = previous else {
        // The daemon cannot reveal its activation environment. Without manager
        // ownership evidence we cannot safely overwrite a later desktop.
        tracing::warn!("D-Bus activation environment cannot be removed or ownership-checked without a user manager");
        return Ok(());
    };
    let current = manager_environment(bus)?;
    if current.get("WAYBRIDGE_SESSION_ID")
        != written.get("WAYBRIDGE_SESSION_ID").and_then(Option::as_ref)
        || current.get("WAYLAND_DISPLAY") != written.get("WAYLAND_DISPLAY").and_then(Option::as_ref)
    {
        tracing::info!("a later desktop replaced the activation environment; preserving it");
        return Ok(());
    }
    let wayland_live = previous_display_live(previous, "WAYLAND_DISPLAY");
    let x11_live = previous_display_live(previous, "DISPLAY");
    let mut restore = BTreeMap::new();
    for (key, value) in written {
        if current.get(key) != value.as_ref() {
            continue;
        }
        let old = previous.get(key).cloned();
        let display_key = matches!(
            key.as_str(),
            "WAYLAND_DISPLAY"
                | "DISPLAY"
                | "XDG_SESSION_TYPE"
                | "XDG_CURRENT_DESKTOP"
                | "XDG_SESSION_DESKTOP"
                | "DESKTOP_SESSION"
        );
        let display_live = match key.as_str() {
            "WAYLAND_DISPLAY" => wayland_live,
            "DISPLAY" => x11_live,
            _ => wayland_live || x11_live,
        };
        // Never resurrect a stale address even when another old display lives.
        restore.insert(
            key.clone(),
            if display_key && !display_live && old.as_ref().is_some_and(|value| !value.is_empty()) {
                None
            } else {
                old
            },
        );
    }
    update_bus(bus, &restore)?;
    // dbus-daemon --systemd-activation may forward the update to systemd.
    // Apply true unset semantics last so its empty-value fallback cannot leak.
    update_manager(bus, &restore)?;
    Ok(())
}

fn lifecycle(
    bus: Connection,
    status: PathBuf,
    target: bool,
    previous: Option<BTreeMap<String, String>>,
    written: BTreeMap<String, Option<String>>,
    stopped: Arc<AtomicBool>,
) -> Result<()> {
    let mut started = false;
    if target {
        // Nonblocking enqueue: Type=notify/dbus services may need the compositor
        // event loop to make progress before their startup jobs can finish.
        match manager_call(&bus, "StartUnit", &(TARGET, "fail")) {
            Ok(_) => started = true,
            Err(error) => {
                report(&status, "unavailable", &error.to_string(), &bus);
            }
        }
    }
    if started {
        let deadline = Instant::now() + Duration::from_secs(30);
        loop {
            if stopped.load(Ordering::Acquire) {
                break;
            }
            match active(&bus, TARGET).as_deref() {
                Ok("active") => {
                    report(&status, "ready", "Waybridge session target is active", &bus);
                    break;
                }
                Ok("failed") => {
                    report(
                        &status,
                        "unavailable",
                        "session target startup did not complete",
                        &bus,
                    );
                    break;
                }
                _ if Instant::now() >= deadline => {
                    report(
                        &status,
                        "unavailable",
                        "session target startup timed out",
                        &bus,
                    );
                    break;
                }
                _ => thread::sleep(Duration::from_millis(20)),
            }
        }
    }
    while !stopped.load(Ordering::Acquire) {
        thread::sleep(Duration::from_millis(20));
    }
    if started {
        // Do not stop a target that a newer Waybridge session has taken over.
        let current = manager_environment(&bus)?;
        if current.get("WAYBRIDGE_SESSION_ID")
            == written.get("WAYBRIDGE_SESSION_ID").and_then(Option::as_ref)
        {
            report(
                &status,
                "stopping",
                "stopping only units declaring PartOf=waybridge-session.target",
                &bus,
            );
            let mut units = Vec::new();
            owned_units(&bus, TARGET, &mut units)?;
            manager_call(&bus, "StartUnit", &(SHUTDOWN, "replace"))?;
            let deadline = Instant::now() + Duration::from_secs(15);
            loop {
                let pending = units.iter().any(|unit| {
                    !matches!(active(&bus, unit).as_deref(), Ok("inactive" | "failed"))
                });
                if !pending {
                    break;
                }
                if Instant::now() >= deadline {
                    bail!("Waybridge session services did not stop before the shutdown deadline; activation environment was retained");
                }
                thread::sleep(Duration::from_millis(20));
            }
        }
    }
    cleanup_environment(&bus, previous.as_ref(), &written)?;
    report(
        &status,
        "stopped",
        "owned services stopped; systemd environment restored/unset; D-Bus has no unset API",
        &bus,
    );
    Ok(())
}

impl Services {
    pub fn prepare(runtime: &Path) -> Result<Self> {
        let address = std::env::var("DBUS_SESSION_BUS_ADDRESS")
            .ok()
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| {
                format!(
                    "unix:path={}",
                    percent_encoding::percent_encode(
                        std::env::var_os("XDG_RUNTIME_DIR")
                            .map(PathBuf::from)
                            .unwrap_or_else(|| runtime.to_path_buf())
                            .join("bus")
                            .as_os_str()
                            .as_encoded_bytes(),
                        percent_encoding::NON_ALPHANUMERIC
                    )
                )
            });
        let bus = connect_bus(&address, Duration::from_secs(2))
            .context("Cannot connect to the user D-Bus; start the dbus user service before creating a Waybridge session")?;
        let runtime = tempfile::Builder::new()
            .prefix("waybridge-services-")
            .tempdir_in(runtime)?;
        fs::set_permissions(runtime.path(), fs::Permissions::from_mode(0o700))?;
        Ok(Self {
            runtime,
            address,
            bus,
            stopped: Arc::default(),
            worker: None,
        })
    }
    pub fn address(&self) -> &str {
        &self.address
    }
    pub fn status_file(&self) -> PathBuf {
        self.runtime.path().join("status.json")
    }
    pub fn configure(&self, command: &mut Command) {
        command
            .env("WAYBRIDGE_DESKTOP_SERVICES", self.status_file())
            .env("WAYBRIDGE_SESSION_RUNTIME", self.runtime.path());
    }
    pub fn start(&mut self, environment: &BTreeMap<String, Option<String>>) -> Result<()> {
        let previous = manager_environment(&self.bus).ok();
        let target = if previous.is_some() {
            match manager_call(&self.bus, "LoadUnit", &(TARGET,)) {
                Ok(_) => {
                    // An inactive target can still own a queued start job while
                    // a Type=notify dependency is becoming ready.
                    type Job = (
                        u32,
                        String,
                        String,
                        String,
                        zbus::zvariant::OwnedObjectPath,
                        zbus::zvariant::OwnedObjectPath,
                    );
                    let jobs: Vec<Job> = manager_call(&self.bus, "ListJobs", &())?
                        .body()
                        .deserialize()?;
                    if jobs.iter().any(|job| job.1 == TARGET)
                        || !matches!(active(&self.bus, TARGET)?.as_str(), "inactive" | "failed")
                    {
                        bail!("waybridge-session.target is already active or starting; end its owning session first");
                    }
                    true
                }
                Err(error) => {
                    tracing::warn!(%error, "Waybridge user session units are not installed; service lifecycle is unavailable");
                    false
                }
            }
        } else {
            false
        };
        let mut written: BTreeMap<_, _> = environment
            .iter()
            .filter(|(key, _)| ACTIVATION_KEYS.contains(&key.as_str()))
            .map(|(key, value)| (key.clone(), value.clone()))
            .collect();
        written.insert(
            "WAYBRIDGE_SESSION_ID".into(),
            Some(self.runtime.path().to_string_lossy().into_owned()),
        );
        // No toolkit, IME, application data or per-app audio preference is
        // imported into the global user-service environment.
        if let Err(error) = update_bus(&self.bus, &written) {
            if let Some(previous) = &previous {
                let _ = cleanup_environment(&self.bus, Some(previous), &written);
            }
            return Err(error).context("Updating user D-Bus activation environment");
        }
        // The daemon may forward empty values to systemd; genuine unsets win.
        if previous.is_some() {
            if let Err(error) = update_manager(&self.bus, &written) {
                let _ = cleanup_environment(&self.bus, previous.as_ref(), &written);
                return Err(error).context("Updating systemd activation environment");
            }
        }
        report(
            &self.status_file(),
            if target { "starting" } else { "unavailable" },
            if target {
                "Waybridge session target is starting asynchronously"
            } else {
                "install Waybridge user session units and use a systemd user manager for declared service lifecycle"
            },
            &self.bus,
        );
        let bus = self.bus.clone();
        let status = self.status_file();
        let stopped = self.stopped.clone();
        let worker_previous = previous.clone();
        let worker_written = written.clone();
        match thread::Builder::new()
            .name("waybridge-services".into())
            .spawn(move || {
                lifecycle(
                    bus,
                    status,
                    target,
                    worker_previous,
                    worker_written,
                    stopped,
                )
            }) {
            Ok(worker) => self.worker = Some(worker),
            Err(error) => {
                // Environment publication has already succeeded. Do not lose
                // its rollback state if allocating the worker itself fails.
                if let Err(cleanup) = cleanup_environment(&self.bus, previous.as_ref(), &written) {
                    tracing::warn!(%cleanup, "cannot roll back activation environment after worker startup failure");
                }
                return Err(error).context("Starting desktop-service lifecycle worker");
            }
        }
        Ok(())
    }
    pub fn stop(&mut self, mut pump: impl FnMut() -> Result<()>) -> Result<()> {
        self.stopped.store(true, Ordering::Release);
        let mut pump_error = None;
        if let Some(worker) = self.worker.take() {
            while !worker.is_finished() {
                if pump_error.is_none() {
                    if let Err(error) = pump() {
                        pump_error = Some(error);
                    }
                }
                thread::sleep(Duration::from_millis(2));
            }
            worker
                .join()
                .map_err(|_| anyhow::anyhow!("Waybridge session lifecycle worker panicked"))??;
        }
        if let Some(error) = pump_error {
            return Err(error);
        }
        Ok(())
    }
}
impl Drop for Services {
    fn drop(&mut self) {
        if let Err(error) = self.stop(|| Ok(())) {
            tracing::warn!(%error, "cannot finish Waybridge session service shutdown");
        }
    }
}
