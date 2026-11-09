//! Keep remote applications off the host desktop's input-method and D-Bus session.
use crate::audio;
use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use std::{
    collections::BTreeMap,
    fs::{self, OpenOptions},
    io::{BufRead, BufReader, Read},
    os::unix::{
        fs::{MetadataExt, OpenOptionsExt, PermissionsExt},
        process::CommandExt,
    },
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
};

#[derive(Serialize, Deserialize)]
struct Saved {
    version: u8,
    pid: u32,
    started: String,
    environment: BTreeMap<String, Option<String>>,
}
fn started(pid: u32) -> Result<String> {
    let stat = fs::read_to_string(format!("/proc/{pid}/stat"))?;
    stat.rsplit_once(')')
        .and_then(|(_, tail)| tail.split_whitespace().nth(19))
        .map(str::to_owned)
        .context("reading session process start time")
}
fn descriptor(socket: &str) -> Result<PathBuf> {
    let path = Path::new(socket);
    let name = path
        .file_name()
        .context("invalid Wayland socket name")?
        .to_string_lossy();
    Ok(if path.is_absolute() {
        path.with_file_name(format!("{name}.session.json"))
    } else {
        let runtime = std::env::var_os("XDG_RUNTIME_DIR")
            .unwrap_or_else(|| format!("/run/user/{}", unsafe { libc::getuid() }).into());
        PathBuf::from(runtime).join(format!("{name}.session.json"))
    })
}

/// Join a running local session without shell evaluation or changing the host environment.
pub fn attach(socket: &str, args: &[String]) -> Result<()> {
    let program = args
        .first()
        .context("--launch requires a command after --")?;
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW)
        .open(descriptor(socket)?)
        .context("reading session environment; start the server first")?;
    let metadata = file.metadata()?;
    if metadata.uid() != unsafe { libc::getuid() } || metadata.permissions().mode() & 0o077 != 0 {
        bail!("session environment must be owned by this user with mode 0600");
    }
    let mut bytes = Vec::new();
    file.take(32769).read_to_end(&mut bytes)?;
    if bytes.len() > 32768 {
        bail!("session environment is too large");
    }
    let saved: Saved = serde_json::from_slice(&bytes)?;
    if saved.version != 1 || started(saved.pid).ok().as_ref() != Some(&saved.started) {
        bail!("session environment is stale; the compositor is no longer running");
    }
    let mut command = Command::new(program);
    command.args(&args[1..]);
    for (key, value) in saved.environment {
        if let Some(value) = value {
            command.env(key, value);
        } else {
            command.env_remove(key);
        }
    }
    Err(command.exec()).with_context(|| format!("launching {program}"))
}

pub fn available(program: &str) -> bool {
    std::env::var_os("PATH").is_some_and(|paths| {
        std::env::split_paths(&paths).any(|path| {
            path.join(program)
                .metadata()
                .is_ok_and(|m| m.is_file() && m.permissions().mode() & 0o111 != 0)
        })
    })
}

pub struct Session {
    socket: String,
    display: Option<u32>,
    bus: Option<Child>,
    address: Option<String>,
    saved: Option<(PathBuf, u64, u64)>,
    keyring: Option<crate::keyring::Bridge>,
}
impl Session {
    pub fn new(socket: &str, display: Option<u32>, audio: Option<&audio::Session>) -> Result<Self> {
        let mut session = Self {
            socket: socket.into(),
            display,
            bus: None,
            address: None,
            saved: None,
            keyring: None,
        };
        if available("dbus-daemon") {
            let mut command = Command::new("dbus-daemon");
            session.configure(&mut command, audio);
            let mut child = command
                .args(["--session", "--nofork", "--print-address=1"])
                .stdin(Stdio::null())
                .stdout(Stdio::piped())
                .spawn()
                .context("starting private application D-Bus session")?;
            let stdout = child.stdout.take().unwrap();
            session.bus = Some(child);
            let mut address = String::new();
            BufReader::new(stdout.take(4096)).read_line(&mut address)?;
            if !address.starts_with("unix:") {
                bail!("private D-Bus daemon returned no Unix address");
            }
            session.address = Some(address.trim().into());
            match crate::keyring::Bridge::new(session.address.as_deref().unwrap()) {
                Ok(bridge) => session.keyring = bridge,
                Err(error) => tracing::warn!(%error, "host keyring bridge unavailable"),
            }
        } else {
            tracing::warn!(
                "dbus-daemon unavailable; applications cannot share a private session bus"
            );
        }
        let path = descriptor(socket)?;
        let mut command = Command::new("env");
        session.configure(&mut command, audio);
        let saved = Saved {
            version: 1,
            pid: std::process::id(),
            started: started(std::process::id())?,
            environment: command
                .get_envs()
                .map(|(k, v)| {
                    (
                        k.to_string_lossy().into_owned(),
                        v.map(|v| v.to_string_lossy().into_owned()),
                    )
                })
                .collect(),
        };
        let mut file = tempfile::NamedTempFile::new_in(path.parent().unwrap())?;
        serde_json::to_writer(file.as_file_mut(), &saved)?;
        let metadata = file.as_file().metadata()?;
        file.persist(&path).map_err(|e| e.error)?;
        session.saved = Some((path.clone(), metadata.dev(), metadata.ino()));
        tracing::info!(path=%path.display(), "application environment ready; use --launch -- <application> to join");
        Ok(session)
    }
    pub fn configure(&self, command: &mut Command, audio: Option<&audio::Session>) {
        command
            .env("WAYLAND_DISPLAY", &self.socket)
            .env("XDG_SESSION_TYPE", "wayland")
            .env(
                "XDG_CURRENT_DESKTOP",
                self.keyring.as_ref().map_or("Waybridge", |b| b.desktop()),
            )
            .env("QT_QPA_PLATFORM", "wayland;xcb")
            .env("GDK_BACKEND", "wayland,x11");
        if let Some(version) = self.keyring.as_ref().and_then(|b| b.kde_version()) {
            command.env("KDE_SESSION_VERSION", version);
        }
        for name in [
            "XDG_RUNTIME_DIR",
            "XDG_CONFIG_HOME",
            "XDG_CACHE_HOME",
            "XDG_DATA_HOME",
            "XDG_STATE_HOME",
            "PULSE_SERVER",
            "PIPEWIRE_RUNTIME_DIR",
            "PIPEWIRE_REMOTE",
        ] {
            if let Some(value) = std::env::var_os(name) {
                command.env(name, value);
            }
        }
        // Host fcitx/ibus overrides otherwise prevent enabling text-input-v3.
        for name in [
            "QT_IM_MODULE",
            "GTK_IM_MODULE",
            "XMODIFIERS",
            "SDL_IM_MODULE",
            "GLFW_IM_MODULE",
            "DBUS_SESSION_BUS_ADDRESS",
            "DBUS_STARTER_ADDRESS",
            "DBUS_STARTER_BUS_TYPE",
        ] {
            command.env_remove(name);
        }
        if let Some(number) = self.display {
            command.env("DISPLAY", format!(":{number}"));
        } else {
            command.env_remove("DISPLAY");
        }
        if let Some(address) = &self.address {
            command.env("DBUS_SESSION_BUS_ADDRESS", address);
        }
        if let Some(audio) = audio {
            audio.configure_command(command);
        }
    }
}
impl Drop for Session {
    fn drop(&mut self) {
        self.keyring.take();
        if let Some((path, dev, ino)) = &self.saved {
            if fs::symlink_metadata(path).is_ok_and(|m| m.dev() == *dev && m.ino() == *ino) {
                let _ = fs::remove_file(path);
            }
        }
        if let Some(child) = self.bus.as_mut() {
            let _ = child.kill();
            let _ = child.wait();
        }
    }
}
