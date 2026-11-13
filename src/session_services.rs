//! Standard user services and shared application data for the sole graphical session.
use anyhow::{bail, Context, Result};
use serde::Serialize;
use std::{
    collections::BTreeMap,
    fs,
    path::{Path, PathBuf},
    process::Command,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
    thread::{self, JoinHandle},
    time::Duration,
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
    previous: BTreeMap<String, Option<String>>,
    written: BTreeMap<String, String>,
    manager: bool,
    stopped: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
}

fn update_bus(bus: &Connection, values: &BTreeMap<String, String>) -> Result<()> {
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

fn update_manager(bus: &Connection, values: &BTreeMap<String, Option<String>>) -> Result<()> {
    let set: Vec<String> = values
        .iter()
        .filter_map(|(key, value)| value.as_ref().map(|value| format!("{key}={value}")))
        .collect();
    let unset: Vec<&str> = values
        .iter()
        .filter_map(|(key, value)| value.is_none().then_some(key.as_str()))
        .collect();
    if !set.is_empty() {
        bus.call_method(
            Some("org.freedesktop.systemd1"),
            "/org/freedesktop/systemd1",
            Some("org.freedesktop.systemd1.Manager"),
            "SetEnvironment",
            &(set,),
        )?;
    }
    if !unset.is_empty() {
        bus.call_method(
            Some("org.freedesktop.systemd1"),
            "/org/freedesktop/systemd1",
            Some("org.freedesktop.systemd1.Manager"),
            "UnsetEnvironment",
            &(unset,),
        )?;
    }
    Ok(())
}

fn report(path: &Path, states: &[State]) -> Result<()> {
    let mut file = tempfile::NamedTempFile::new_in(path.parent().context("status parent")?)?;
    serde_json::to_writer_pretty(
        file.as_file_mut(),
        &serde_json::json!({
            "version": 2, "desktop": "Waybridge", "scope": "user", "data_policy": "shared", "services": states,
        }),
    )?;
    file.persist(path).map_err(|error| error.error)?;
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
        let config = runtime.path().join("config/xdg-desktop-portal");
        fs::create_dir_all(&config)?;
        // User XDG_CONFIG_HOME keeps precedence. Only the backend selection is
        // session-local; wallet, portal permissions and documents use user data.
        fs::write(config.join("waybridge-portals.conf"), "[preferred]\ndefault=gtk\norg.freedesktop.impl.portal.ScreenCast=none\norg.freedesktop.impl.portal.Screenshot=none\norg.freedesktop.impl.portal.RemoteDesktop=none\norg.freedesktop.impl.portal.GlobalShortcuts=none\n")?;
        Ok(Self {
            runtime,
            address,
            bus,
            previous: BTreeMap::new(),
            written: BTreeMap::new(),
            manager: false,
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
        let previous = std::env::var("XDG_CONFIG_DIRS")
            .ok()
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| "/etc/xdg".into());
        command
            .env(
                "XDG_CONFIG_DIRS",
                format!(
                    "{}:{previous}",
                    self.runtime.path().join("config").display()
                ),
            )
            .env("WAYBRIDGE_DESKTOP_SERVICES", self.status_file());
    }

    pub fn start(&mut self, environment: &BTreeMap<String, Option<String>>) -> Result<()> {
        let manager = manager_environment(&self.bus).ok();
        self.manager = manager.is_some();
        // WAYBRIDGE_DESKTOP_SERVICES is a diagnostic path for child apps only.
        // Never import the entire process environment into user services.
        for (key, value) in environment {
            if matches!(
                key.as_str(),
                "WAYBRIDGE_DESKTOP_SERVICES"
                    | "WAYLAND_SOCKET"
                    | "DBUS_STARTER_ADDRESS"
                    | "DBUS_STARTER_BUS_TYPE"
            ) {
                continue;
            }
            let old = match &manager {
                Some(manager) => manager.get(key).cloned(),
                None => std::env::var(key).ok(),
            };
            if value.is_none() && old.is_none() {
                continue;
            }
            self.previous.insert(key.clone(), old);
            self.written
                .insert(key.clone(), value.clone().unwrap_or_default());
        }
        update_bus(&self.bus, &self.written)
            .context("Updating user D-Bus activation environment")?;
        if self.manager {
            update_manager(
                &self.bus,
                &self
                    .written
                    .iter()
                    .map(|(k, v)| (k.clone(), Some(v.clone())))
                    .collect(),
            )
            .context("Updating systemd user activation environment")?;
        }
        let providers = [
            ("secret-service", "org.freedesktop.secrets"),
            ("kwallet", "org.kde.kwalletd6"),
            ("gtk-portal", "org.freedesktop.impl.portal.desktop.gtk"),
            ("desktop-portal", "org.freedesktop.portal.Desktop"),
        ];
        let mut states: Vec<State> = providers
            .iter()
            .map(|(name, _)| State {
                name,
                status: "starting",
                detail: "standard user D-Bus activation".into(),
            })
            .collect();
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
        report(&self.status_file(), &states)?;
        let stopped = self.stopped.clone();
        let address = self.address.clone();
        let status_file = self.status_file();
        self.worker = Some(thread::spawn(move || {
            let Ok(bus) = connect_bus(&address, Duration::from_secs(2)) else {
                return;
            };
            for ((_, name), state) in providers.into_iter().zip(states.iter_mut()) {
                if stopped.load(Ordering::Acquire) {
                    return;
                }
                let mut result = bus.call_method(
                    Some("org.freedesktop.DBus"),
                    "/org/freedesktop/DBus",
                    Some("org.freedesktop.DBus"),
                    "StartServiceByName",
                    &(name, 0u32),
                );
                // Some distributions install KSecretD's own activation name
                // without an org.freedesktop.secrets alias. Respect an existing
                // provider first, then use the installed native fallback.
                if name == "org.freedesktop.secrets"
                    && matches!(&result, Err(zbus::Error::MethodError(error, _, _))
                        if error.as_str() == "org.freedesktop.DBus.Error.ServiceUnknown")
                {
                    result = bus.call_method(
                        Some("org.freedesktop.DBus"),
                        "/org/freedesktop/DBus",
                        Some("org.freedesktop.DBus"),
                        "StartServiceByName",
                        &("org.kde.secretservicecompat", 0u32),
                    );
                }
                match result {
                    Ok(_) => {
                        state.status = "ready";
                        state.detail = "user service activated or already running".into();
                    }
                    Err(error) => {
                        state.status = "unavailable";
                        state.detail = error.to_string();
                    }
                }
            }
            let _ = report(&status_file, &states);
            while !stopped.load(Ordering::Acquire) {
                let mut changed = false;
                for ((_, name), state) in providers.iter().zip(states.iter_mut()) {
                    let owns: Option<bool> = bus
                        .call_method(
                            Some("org.freedesktop.DBus"),
                            "/org/freedesktop/DBus",
                            Some("org.freedesktop.DBus"),
                            "NameHasOwner",
                            &(name,),
                        )
                        .ok()
                        .and_then(|reply| reply.body().deserialize().ok());
                    if let Some(owns) = owns {
                        let status = if owns { "ready" } else { "unavailable" };
                        if state.status != status {
                            state.status = status;
                            state.detail = if owns {
                                "user D-Bus name registered"
                            } else {
                                "user service no longer owns its D-Bus name"
                            }
                            .into();
                            changed = true;
                        }
                    }
                }
                if changed {
                    let _ = report(&status_file, &states);
                }
                thread::sleep(Duration::from_millis(200));
            }
        }));
        Ok(())
    }
}

impl Drop for Services {
    fn drop(&mut self) {
        self.stopped.store(true, Ordering::Release);
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
        if self.written.is_empty() {
            return;
        }
        // A later local login is outside our admission policy. If it replaced
        // the display environment, do not restore over that newer desktop.
        if self.manager {
            let Ok(current) = manager_environment(&self.bus) else {
                return;
            };
            if current.get("WAYLAND_DISPLAY") != self.written.get("WAYLAND_DISPLAY") {
                return;
            }
            if let Err(error) = update_manager(&self.bus, &self.previous) {
                tracing::warn!(%error, "cannot restore user service environment");
            }
        }
        let previous = self
            .previous
            .iter()
            .map(|(key, value)| (key.clone(), value.clone().unwrap_or_default()))
            .collect();
        if let Err(error) = update_bus(&self.bus, &previous) {
            tracing::warn!(%error, "cannot restore D-Bus activation environment");
        }
        // Shared wallets and portals follow the user's normal service lifecycle.
    }
}
