//! Native wallet and portal providers on a private application bus.
//! Service-only XDG directories preserve the applications' normal user profiles.
use anyhow::{bail, Context, Result};
use serde::Serialize;
use std::{
    collections::BTreeMap,
    fs::{self, File, OpenOptions},
    io::Write,
    os::unix::{fs::OpenOptionsExt, fs::PermissionsExt, process::CommandExt},
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

#[derive(Clone, Serialize)]
struct State {
    name: String,
    status: String,
    detail: String,
}

struct Provider {
    label: &'static str,
    names: Vec<&'static str>,
    wrapper: PathBuf,
    child: Option<Child>,
}

pub struct Services {
    runtime: tempfile::TempDir,
    bus_config: PathBuf,
    persistent: PathBuf,
    _lock: File,
    providers: Vec<Provider>,
    states: Vec<State>,
    status_file: PathBuf,
    stopped: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
}

fn private_dir(path: &Path) -> Result<()> {
    if !path.exists() {
        fs::create_dir_all(path).with_context(|| format!("creating {}", path.display()))?;
        fs::set_permissions(path, fs::Permissions::from_mode(0o700))?;
    }
    use std::os::unix::fs::MetadataExt;
    let metadata = fs::symlink_metadata(path)?;
    if !metadata.is_dir()
        || metadata.uid() != unsafe { libc::getuid() }
        || metadata.permissions().mode() & 0o077 != 0
    {
        bail!(
            "desktop service directory must be owned by this user with mode 0700: {}",
            path.display()
        );
    }
    Ok(())
}

fn write_private(path: &Path, contents: &str, mode: u32) -> Result<()> {
    let mut file = OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .custom_flags(libc::O_NOFOLLOW)
        .mode(mode)
        .open(path)?;
    file.write_all(contents.as_bytes())?;
    file.set_permissions(fs::Permissions::from_mode(mode))?;
    Ok(())
}

fn shell_quote(value: &str) -> String {
    format!("'{}'", value.replace('\'', "'\\''"))
}

fn exec_quote(path: &Path) -> String {
    format!(
        "\"{}\"",
        path.to_string_lossy()
            .replace('\\', "\\\\")
            .replace('"', "\\\"")
    )
}

fn xml_quote(path: &Path) -> String {
    path.to_string_lossy()
        .replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
        .replace('"', "&quot;")
        .replace('\'', "&apos;")
}

fn bus_address_value(path: &Path) -> String {
    // Address values have their own bytewise %XX grammar, independent of XML.
    // Escaping optional characters too is permitted by the D-Bus specification
    // and preserves non-UTF8 Unix paths without a lossy string conversion.
    percent_encoding::percent_encode(
        path.as_os_str().as_encoded_bytes(),
        percent_encoding::NON_ALPHANUMERIC,
    )
    .to_string()
}

fn decode_bus_value(value: &str) -> Result<std::ffi::OsString> {
    use std::os::unix::ffi::OsStringExt;
    let mut decoded = Vec::with_capacity(value.len());
    let mut bytes = value.bytes();
    while let Some(byte) = bytes.next() {
        if byte == b'%' {
            let hi = bytes.next().and_then(|v| (v as char).to_digit(16));
            let lo = bytes.next().and_then(|v| (v as char).to_digit(16));
            let (Some(hi), Some(lo)) = (hi, lo) else {
                bail!("invalid percent escape in private D-Bus address");
            };
            let byte = ((hi << 4) | lo) as u8;
            if byte == 0 {
                bail!("NUL in private D-Bus address");
            }
            decoded.push(byte);
        } else if matches!(byte, b'-' | b'0'..=b'9' | b'A'..=b'Z' | b'a'..=b'z' | b'_' | b'/' | b'.' | b'\\' | b'*')
        {
            decoded.push(byte);
        } else {
            bail!("unescaped byte in private D-Bus address");
        }
    }
    Ok(std::ffi::OsString::from_vec(decoded))
}

pub fn connect_private_bus(
    address: &str,
    method_timeout: Duration,
) -> Result<zbus::blocking::Connection> {
    use zbus::address::{
        transport::{Unix, UnixSocket},
        Address, Transport,
    };
    // zbus 5.19's Unix address parser does not decode %XX path bytes. Construct
    // a typed address directly, preserving the broker GUID and decoding once.
    let options = address
        .strip_prefix("unix:")
        .context("private D-Bus must use a Unix socket")?;
    let mut socket = None;
    let mut guid = None;
    for option in options.split(',') {
        let (key, value) = option
            .split_once('=')
            .context("invalid private D-Bus address option")?;
        match key {
            "path" | "abstract" => {
                if socket.is_some() {
                    bail!("multiple private D-Bus socket paths");
                }
                let value = decode_bus_value(value)?;
                socket = Some(if key == "path" {
                    UnixSocket::File(PathBuf::from(value))
                } else {
                    UnixSocket::Abstract(value)
                });
            }
            "guid" if guid.is_none() => guid = Some(value),
            _ => bail!("unsupported private D-Bus address option"),
        }
    }
    let mut address = Address::new(Transport::Unix(Unix::new(
        socket.context("missing private D-Bus socket path")?,
    )));
    if let Some(guid) = guid {
        address = address.set_guid(zbus::Guid::try_from(guid)?)?;
    }
    Ok(zbus::blocking::connection::Builder::address(address)?
        .method_timeout(method_timeout)
        .build()?)
}

fn program(candidates: &[&str]) -> Option<PathBuf> {
    candidates.iter().find_map(|candidate| {
        let executable = |path: PathBuf| {
            path.metadata()
                .is_ok_and(|m| m.is_file() && m.permissions().mode() & 0o111 != 0)
                .then_some(path)
        };
        if candidate.contains('/') {
            executable(PathBuf::from(candidate))
        } else {
            std::env::var_os("PATH").and_then(|paths| {
                std::env::split_paths(&paths)
                    .find_map(|directory| executable(directory.join(candidate)))
            })
        }
    })
}

fn save_report(path: &Path, persistent: &Path, states: &[State]) -> Result<()> {
    let mut file = tempfile::NamedTempFile::new_in(path.parent().context("status parent")?)?;
    serde_json::to_writer_pretty(
        file.as_file_mut(),
        &serde_json::json!({
            "version": 1, "desktop": "Waybridge", "persistent": persistent, "services": states
        }),
    )?;
    file.persist(path).map_err(|error| error.error)?;
    Ok(())
}

impl Services {
    pub fn prepare(socket: &str, runtime: &Path) -> Result<Self> {
        let data = std::env::var_os("XDG_DATA_HOME")
            .filter(|value| !value.is_empty())
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("HOME").map(|p| PathBuf::from(p).join(".local/share")))
            .context("HOME or XDG_DATA_HOME is required for persistent session services")?;
        let socket = if Path::new(socket).is_absolute() {
            PathBuf::from(socket)
        } else {
            runtime.join(socket)
        };
        Self::prepare_in(&socket, runtime, &data)
    }

    fn prepare_in(socket: &Path, runtime: &Path, data: &Path) -> Result<Self> {
        let digest =
            ring::digest::digest(&ring::digest::SHA256, socket.as_os_str().as_encoded_bytes());
        let profile: String = digest.as_ref()[..16]
            .iter()
            .map(|v| format!("{v:02x}"))
            .collect();
        let root = data.join("waybridge");
        private_dir(&root)?;
        let profiles = root.join("sessions");
        private_dir(&profiles)?;
        let persistent = profiles.join(profile);
        private_dir(&persistent)?;
        let lock = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW)
            .open(persistent.join("services.lock"))?;
        if unsafe {
            libc::flock(
                std::os::fd::AsRawFd::as_raw_fd(&lock),
                libc::LOCK_EX | libc::LOCK_NB,
            )
        } != 0
        {
            bail!(
                "desktop service profile is already in use: {}",
                persistent.display()
            );
        }
        let runtime = tempfile::Builder::new()
            .prefix("waybridge-services-")
            .tempdir_in(runtime)?;
        let catalog = runtime.path().join("dbus-services");
        private_dir(&catalog)?;
        let bus_config = runtime.path().join("bus.conf");
        // Do not use --systemd-activation: that would reuse the host user's
        // global activation environment. Private overrides have first priority.
        write_private(&bus_config, &format!(
            "<busconfig><type>session</type><keep_umask/><listen>unix:tmpdir={}</listen><auth>EXTERNAL</auth><servicedir>{}</servicedir><standard_session_servicedirs/><policy context=\"default\"><allow send_destination=\"*\" eavesdrop=\"true\"/><allow eavesdrop=\"true\"/><allow own=\"*\"/></policy><limit name=\"service_start_timeout\">10000</limit></busconfig>\n",
            bus_address_value(runtime.path()), xml_quote(&catalog)
        ), 0o600)?;
        let status_file = runtime.path().join("status.json");
        let mut services = Self {
            runtime,
            bus_config,
            persistent,
            _lock: lock,
            providers: Vec::new(),
            states: Vec::new(),
            status_file,
            stopped: Arc::default(),
            worker: None,
        };
        let wallet_root = services.persistent.join("wallet");
        for name in ["config", "data", "cache", "state"] {
            private_dir(&wallet_root.join(name))?;
        }
        let wallet_config = wallet_root.join("config/kwalletrc");
        if !wallet_config.exists() {
            write_private(
                &wallet_config,
                "[Wallet]\nEnabled=true\nFirst Use=false\n[KSecretD]\nEnabled=true\n",
                0o600,
            )?;
        }
        let wallet_environment = service_environment(&wallet_root);
        services.add_provider(
            "secret-service",
            &["ksecretd"],
            vec![
                "org.kde.ksecretd",
                "org.kde.secretservicecompat",
                "org.freedesktop.secrets",
                "org.freedesktop.impl.portal.desktop.kwallet",
            ],
            &wallet_environment,
        )?;
        services.add_provider(
            "kwallet",
            &["kwalletd6"],
            vec!["org.kde.kwalletd6"],
            &wallet_environment,
        )?;

        let portal_root = services.persistent.join("portal");
        for name in ["config", "data", "cache", "state"] {
            private_dir(&portal_root.join(name))?;
        }
        private_dir(&portal_root.join("config/xdg-desktop-portal"))?;
        write_private(&portal_root.join("config/xdg-desktop-portal/waybridge-portals.conf"), "[preferred]\ndefault=gtk\norg.freedesktop.impl.portal.Secret=kwallet\norg.freedesktop.impl.portal.ScreenCast=none\norg.freedesktop.impl.portal.Screenshot=none\norg.freedesktop.impl.portal.RemoteDesktop=none\norg.freedesktop.impl.portal.GlobalShortcuts=none\n", 0o600)?;
        let mut portal_environment = service_environment(&portal_root);
        // A private document portal must not replace the host DE's runtime/doc.
        let portal_runtime = services.runtime.path().join("portal-runtime");
        private_dir(&portal_runtime)?;
        portal_environment.insert(
            "XDG_RUNTIME_DIR".into(),
            portal_runtime.to_string_lossy().into(),
        );
        portal_environment.insert("WAYLAND_DISPLAY".into(), socket.to_string_lossy().into());
        services.add_provider(
            "permission-store",
            &[
                "/usr/lib/xdg-permission-store",
                "/usr/libexec/xdg-permission-store",
            ],
            vec!["org.freedesktop.impl.portal.PermissionStore"],
            &portal_environment,
        )?;
        services.add_provider(
            "document-portal",
            &[
                "/usr/lib/xdg-document-portal",
                "/usr/libexec/xdg-document-portal",
            ],
            vec!["org.freedesktop.portal.Documents"],
            &portal_environment,
        )?;
        services.add_provider(
            "gtk-portal",
            &[
                "/usr/lib/xdg-desktop-portal-gtk",
                "/usr/libexec/xdg-desktop-portal-gtk",
            ],
            vec!["org.freedesktop.impl.portal.desktop.gtk"],
            &portal_environment,
        )?;
        services.add_provider(
            "desktop-portal",
            &[
                "/usr/lib/xdg-desktop-portal",
                "/usr/libexec/xdg-desktop-portal",
            ],
            vec!["org.freedesktop.portal.Desktop"],
            &portal_environment,
        )?;
        services.states.push(State {
            name: "screencast".into(),
            status: "unsupported".into(),
            detail: "Waybridge ScreenCast backend is not implemented".into(),
        });
        services.states.push(State {
            name: "flatpak-documents".into(),
            status: "unverified".into(),
            detail: "sandbox document access requires application-specific validation".into(),
        });
        save_report(
            &services.status_file,
            &services.persistent,
            &services.states,
        )?;
        Ok(services)
    }

    fn add_provider(
        &mut self,
        label: &'static str,
        candidates: &[&str],
        names: Vec<&'static str>,
        environment: &BTreeMap<String, String>,
    ) -> Result<()> {
        let Some(executable) = program(candidates) else {
            self.states.push(State {
                name: label.into(),
                status: "unavailable".into(),
                detail: format!("provider not installed: {}", candidates.join(", ")),
            });
            tracing::warn!(
                service = label,
                "optional desktop service provider unavailable"
            );
            return Ok(());
        };
        let wrapper = self.runtime.path().join(format!("start-{label}"));
        let arguments = environment
            .iter()
            .map(|(key, value)| shell_quote(&format!("{key}={value}")))
            .collect::<Vec<_>>()
            .join(" ");
        write_private(
            &wrapper,
            &format!(
                "#!/bin/sh\nexec /usr/bin/env {arguments} {}\n",
                shell_quote(&executable.to_string_lossy())
            ),
            0o700,
        )?;
        for name in &names {
            write_private(
                &self
                    .runtime
                    .path()
                    .join("dbus-services")
                    .join(format!("{name}.service")),
                &format!(
                    "[D-BUS Service]\nName={name}\nExec={}\n",
                    exec_quote(&wrapper)
                ),
                0o600,
            )?;
        }
        self.states.push(State {
            name: label.into(),
            status: "configured".into(),
            detail: executable.to_string_lossy().into(),
        });
        self.providers.push(Provider {
            label,
            names,
            wrapper,
            child: None,
        });
        Ok(())
    }

    pub fn bus_config(&self) -> &Path {
        &self.bus_config
    }
    pub fn status_file(&self) -> &Path {
        &self.status_file
    }

    pub fn start(
        &mut self,
        environment: &BTreeMap<String, Option<String>>,
        group: u32,
        address: &str,
    ) -> Result<()> {
        let group = i32::try_from(group).context("invalid private bus process group")?;
        for provider in &mut self.providers {
            let mut command = Command::new(&provider.wrapper);
            for (key, value) in environment {
                if let Some(value) = value {
                    command.env(key, value);
                } else {
                    command.env_remove(key);
                }
            }
            command.stdin(Stdio::null()).process_group(group);
            match command.spawn() {
                Ok(child) => {
                    tracing::info!(
                        service = provider.label,
                        pid = child.id(),
                        "native session desktop service starting"
                    );
                    provider.child = Some(child);
                    self.states
                        .iter_mut()
                        .find(|s| s.name == provider.label)
                        .unwrap()
                        .status = "starting".into();
                }
                Err(error) => {
                    tracing::warn!(service=provider.label, %error, "optional native desktop service failed to start");
                    let state = self
                        .states
                        .iter_mut()
                        .find(|s| s.name == provider.label)
                        .unwrap();
                    state.status = "failed".into();
                    state.detail = error.to_string();
                }
            }
        }
        save_report(&self.status_file, &self.persistent, &self.states)?;
        let mut providers = std::mem::take(&mut self.providers);
        let mut states = self.states.clone();
        let stopped = self.stopped.clone();
        let address = address.to_string();
        let status_file = self.status_file.clone();
        let persistent = self.persistent.clone();
        // Startup needs the compositor event loop to run. Service readiness and
        // exit observation therefore never wait on the compositor/UI thread.
        self.worker = Some(thread::spawn(move || {
            let started = Instant::now();
            let connection = connect_private_bus(&address, Duration::from_millis(150));
            while !stopped.load(Ordering::Acquire) {
                let names: Vec<String> = connection
                    .as_ref()
                    .ok()
                    .and_then(|c| {
                        c.call_method(
                            Some("org.freedesktop.DBus"),
                            "/org/freedesktop/DBus",
                            Some("org.freedesktop.DBus"),
                            "ListNames",
                            &(),
                        )
                        .ok()
                    })
                    .and_then(|r| r.body().deserialize().ok())
                    .unwrap_or_default();
                let mut changed = false;
                for provider in &mut providers {
                    let state = states
                        .iter_mut()
                        .find(|s| s.name == provider.label)
                        .unwrap();
                    let ready = provider
                        .names
                        .iter()
                        .any(|n| names.iter().any(|owner| owner == n));
                    if ready && state.status != "ready" {
                        state.status = "ready".into();
                        state.detail = "private D-Bus name registered".into();
                        changed = true;
                        tracing::info!(
                            service = provider.label,
                            "native session desktop service owns its private bus name"
                        );
                    } else if !ready {
                        if state.status == "ready"
                            || (state.status == "starting"
                                && started.elapsed() > Duration::from_secs(15))
                        {
                            state.status = "unavailable".into();
                            state.detail =
                                "provider does not own its required private D-Bus name".into();
                            changed = true;
                            tracing::warn!(
                                service = provider.label,
                                "native session desktop service bus name unavailable"
                            );
                        }
                        if let Some(child) = provider.child.as_mut() {
                            if let Ok(Some(status)) = child.try_wait() {
                                if state.status != "failed" {
                                    state.status = "failed".into();
                                    state.detail = format!("provider exited: {status}");
                                    changed = true;
                                    tracing::warn!(service=provider.label, %status, "native session desktop service unavailable");
                                }
                            }
                        }
                    }
                }
                if changed {
                    if let Err(error) = save_report(&status_file, &persistent, &states) {
                        tracing::warn!(%error, "cannot update desktop service diagnostics");
                    }
                }
                thread::sleep(Duration::from_millis(200));
            }
            stop_children(&mut providers);
        }));
        Ok(())
    }
}

fn service_environment(root: &Path) -> BTreeMap<String, String> {
    [
        ("XDG_CONFIG_HOME", "config"),
        ("XDG_DATA_HOME", "data"),
        ("XDG_CACHE_HOME", "cache"),
        ("XDG_STATE_HOME", "state"),
    ]
    .into_iter()
    .map(|(key, name)| (key.into(), root.join(name).to_string_lossy().into()))
    .collect()
}

fn stop_children(providers: &mut [Provider]) {
    for provider in providers.iter_mut() {
        if let Some(child) = provider.child.as_mut() {
            if child.try_wait().is_ok_and(|status| status.is_none()) {
                // Give the document portal a chance to release its private FUSE
                // mount instead of immediately preventing cleanup with SIGKILL.
                let _ = unsafe { libc::kill(child.id() as i32, libc::SIGTERM) };
            }
        }
    }
    let deadline = Instant::now() + Duration::from_millis(750);
    loop {
        let running = providers.iter_mut().any(|provider| {
            provider
                .child
                .as_mut()
                .is_some_and(|child| child.try_wait().is_ok_and(|status| status.is_none()))
        });
        if !running || Instant::now() >= deadline {
            break;
        }
        thread::sleep(Duration::from_millis(10));
    }
    for provider in providers {
        if let Some(child) = provider.child.as_mut() {
            let _ = child.kill();
            let _ = child.wait();
        }
    }
}

fn mount_list_contains(contents: &str, target: &Path) -> bool {
    contents.lines().any(|line| {
        line.split_whitespace().nth(4).is_some_and(|field| {
            let path = field
                .replace("\\040", " ")
                .replace("\\011", "\t")
                .replace("\\012", "\n")
                .replace("\\134", "\\");
            path.as_bytes() == target.as_os_str().as_encoded_bytes()
        })
    })
}

fn private_mount_present(target: &Path) -> bool {
    fs::read_to_string("/proc/self/mountinfo")
        .is_ok_and(|contents| mount_list_contains(&contents, target))
}

fn release_private_mount(target: &Path) {
    // Activated providers may have replaced the initially spawned child. Group
    // termination is asynchronous, so wait for their FUSE teardown before the
    // owned runtime TempDir is removed. Only this exact private mount is eligible.
    let deadline = Instant::now() + Duration::from_secs(1);
    while private_mount_present(target) && Instant::now() < deadline {
        thread::sleep(Duration::from_millis(10));
    }
    if !private_mount_present(target) {
        return;
    }
    if let Some(executable) = program(&["fusermount3", "fusermount"]) {
        if let Ok(mut child) = Command::new(executable)
            .args(["-u", "-z", "--"])
            .arg(target)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
        {
            let deadline = Instant::now() + Duration::from_secs(1);
            while child.try_wait().is_ok_and(|status| status.is_none()) && Instant::now() < deadline
            {
                thread::sleep(Duration::from_millis(10));
            }
            let _ = child.kill();
            let _ = child.wait();
        }
    }
    if private_mount_present(target) {
        tracing::warn!(path=%target.display(), "private document portal mount is still present after shutdown");
    }
}

impl Drop for Services {
    fn drop(&mut self) {
        self.stopped.store(true, Ordering::Release);
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
        stop_children(&mut self.providers);
        release_private_mount(&self.runtime.path().join("portal-runtime/doc"));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn private_bus_accepts_runtime_address_metacharacters() {
        use std::io::{BufRead, BufReader};
        struct Bus(Child);
        impl Drop for Bus {
            fn drop(&mut self) {
                let _ = self.0.kill();
                let _ = self.0.wait();
            }
        }
        let temp = tempfile::tempdir().unwrap();
        let runtime = temp.path().join("runtime % quote's $&,=;雪");
        private_dir(&runtime).unwrap();
        let services = Services::prepare_in(
            &runtime.join("display"),
            &runtime,
            &temp.path().join("app-data"),
        )
        .unwrap();
        let mut bus = Bus(Command::new("dbus-daemon")
            .arg("--config-file")
            .arg(services.bus_config())
            .args(["--nofork", "--print-address=1"])
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .spawn()
            .unwrap());
        let mut address = String::new();
        BufReader::new(bus.0.stdout.take().unwrap())
            .read_line(&mut address)
            .unwrap();
        assert!(address.starts_with("unix:"), "private bus did not start");
        let connection = connect_private_bus(address.trim(), Duration::from_secs(2)).unwrap();
        let id: String = connection
            .call_method(
                Some("org.freedesktop.DBus"),
                "/org/freedesktop/DBus",
                Some("org.freedesktop.DBus"),
                "GetId",
                &(),
            )
            .unwrap()
            .body()
            .deserialize()
            .unwrap();
        assert!(!id.is_empty());
        connection.close().unwrap();
    }

    #[test]
    fn private_bus_address_decoder_is_bytewise_and_strict() {
        use std::os::unix::ffi::OsStrExt;
        assert_eq!(
            decode_bus_value("%2ftmp%2f%2520%20%ff").unwrap().as_bytes(),
            b"/tmp/%20 \xff"
        );
        for invalid in [
            "/tmp/unescaped space",
            "/tmp/%",
            "/tmp/%2",
            "/tmp/%GG",
            "/tmp/%00",
        ] {
            assert!(decode_bus_value(invalid).is_err(), "accepted {invalid}");
        }
    }

    #[test]
    fn service_profiles_preserve_application_paths_and_are_exclusive() {
        let temp = tempfile::tempdir().unwrap();
        let socket = temp.path().join("display with 'quotes' & unicode-测试");
        let services =
            Services::prepare_in(&socket, temp.path(), &temp.path().join("app-data")).unwrap();
        assert!(Services::prepare_in(&socket, temp.path(), &temp.path().join("app-data")).is_err());
        let config = fs::read_to_string(services.bus_config()).unwrap();
        assert!(
            config.find("<servicedir>").unwrap()
                < config.find("<standard_session_servicedirs/>").unwrap()
        );
        for provider in &services.providers {
            let wrapper = fs::read_to_string(&provider.wrapper).unwrap();
            assert!(!wrapper.contains("'HOME="));
            assert!(!fs::read_to_string(
                services
                    .runtime
                    .path()
                    .join("dbus-services")
                    .join(format!("{}.service", provider.names[0]))
            )
            .unwrap()
            .contains("SystemdService="));
        }
        let persistent = services.persistent.clone();
        drop(services);
        assert!(persistent.join("wallet/config/kwalletrc").exists());
        assert!(Services::prepare_in(&socket, temp.path(), &temp.path().join("app-data")).is_ok());
    }

    #[test]
    fn private_service_directory_rejects_symlinks() {
        let temp = tempfile::tempdir().unwrap();
        let target = temp.path().join("target");
        fs::create_dir(&target).unwrap();
        let alias = temp.path().join("alias");
        std::os::unix::fs::symlink(&target, &alias).unwrap();
        assert!(private_dir(&alias).is_err());
    }

    #[test]
    fn portal_mount_cleanup_matches_only_the_exact_owned_path() {
        let mounts = "1 0 0:1 / /run/user/1000/doc rw - fuse.portal portal rw\n2 0 0:2 / /run/user/1000/waybridge\\040test/portal-runtime/doc rw - fuse.portal portal rw\n";
        assert!(mount_list_contains(
            mounts,
            Path::new("/run/user/1000/waybridge test/portal-runtime/doc")
        ));
        assert!(!mount_list_contains(
            mounts,
            Path::new("/run/user/1000/waybridge test/portal-runtime")
        ));
        assert!(!mount_list_contains(
            mounts,
            Path::new("/run/user/1000/waybridge test/portal-runtime/doc-other")
        ));
        assert!(!mount_list_contains(
            mounts,
            Path::new("/run/user/1000/other/doc")
        ));
    }
}
