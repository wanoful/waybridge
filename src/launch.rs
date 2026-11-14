//! Route applications to Waybridge graphics/audio while sharing user data and services.
use crate::audio;
use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use std::{
    collections::BTreeMap,
    fs::{self, OpenOptions},
    io::Read,
    os::unix::{
        fs::{MetadataExt, OpenOptionsExt, PermissionsExt},
        process::CommandExt,
    },
    path::{Path, PathBuf},
    process::Command,
};

#[derive(Serialize, Deserialize)]
struct Saved {
    version: u8,
    pid: u32,
    started: String,
    environment: BTreeMap<String, Option<String>>,
    #[serde(default)]
    connection: Option<ConnectionInfo>,
}

#[derive(Serialize, Deserialize)]
struct ConnectionInfo {
    listen: String,
    token_file: String,
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
pub fn attach(socket: &str, args: &[String], cwd: Option<&Path>) -> Result<()> {
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
    command.current_dir(launch_directory(cwd)?);
    for (key, value) in saved.environment {
        if let Some(value) = value {
            command.env(key, value);
        } else {
            command.env_remove(key);
        }
    }
    Err(command.exec()).with_context(|| format!("launching {program}"))
}

/// Direct application entrypoints start in HOME unless the user chose a cwd.
pub fn launch_directory(cwd: Option<&Path>) -> Result<PathBuf> {
    let path = match cwd {
        Some(path) => path.to_path_buf(),
        None => std::env::var_os("HOME")
            .map(PathBuf::from)
            .context("HOME is required for application launch")?,
    };
    if !path.is_absolute() || !path.is_dir() {
        bail!(
            "application working directory must be an existing absolute directory: {}",
            path.display()
        );
    }
    Ok(path)
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

struct Prepared {
    path: PathBuf,
    file: tempfile::NamedTempFile,
    environment: BTreeMap<String, Option<String>>,
}

pub struct Session {
    socket: String,
    display: Option<u32>,
    address: Option<String>,
    saved: Option<(PathBuf, u64, u64)>,
    prepared: Option<Prepared>,
    services: Option<crate::session_services::Services>,
}
impl Session {
    pub fn new(
        socket: &str,
        display: Option<u32>,
        audio: Option<&audio::Session>,
        listen: &str,
        token_file: &Path,
    ) -> Result<Self> {
        let mut session = Self {
            socket: socket.into(),
            display,
            address: None,
            saved: None,
            prepared: None,
            services: None,
        };
        let descriptor_path = descriptor(socket)?;
        let runtime = descriptor_path
            .parent()
            .context("session descriptor directory")?;
        session.services = Some(crate::session_services::Services::prepare(runtime)?);
        session.address = Some(session.services.as_ref().unwrap().address().into());
        let mut environment = Command::new("env");
        session.configure(&mut environment, audio);
        let environment = environment
            .get_envs()
            .map(|(key, value)| {
                (
                    key.to_string_lossy().into_owned(),
                    value.map(|v| v.to_string_lossy().into_owned()),
                )
            })
            .collect();
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
            connection: Some(ConnectionInfo {
                listen: listen.into(),
                token_file: token_file.to_string_lossy().into_owned(),
            }),
        };
        let mut file = tempfile::NamedTempFile::new_in(path.parent().unwrap())?;
        serde_json::to_writer(file.as_file_mut(), &saved)?;
        session.prepared = Some(Prepared {
            path,
            file,
            environment,
        });
        Ok(session)
    }
    /// Publish activation state only after the caller can dispatch service
    /// shutdown on failure. The descriptor becomes visible after that state.
    pub fn start_services(&mut self) -> Result<()> {
        let prepared = self.prepared.take().context("session already started")?;
        self.services
            .as_mut()
            .unwrap()
            .start(&prepared.environment)?;
        let metadata = prepared.file.as_file().metadata()?;
        prepared.file.persist(&prepared.path).map_err(|e| e.error)?;
        self.saved = Some((prepared.path.clone(), metadata.dev(), metadata.ino()));
        tracing::info!(path=%prepared.path.display(), "application environment ready; use --launch -- <application> to join");
        Ok(())
    }
    /// Keep dispatching the compositor while declared services finish shutdown.
    pub fn stop_services(&mut self, pump: impl FnMut() -> Result<()>) -> Result<()> {
        if let Some(services) = self.services.as_mut() {
            services.stop(pump)?;
        }
        Ok(())
    }
    pub fn configure(&self, command: &mut Command, audio: Option<&audio::Session>) {
        command
            .env("WAYLAND_DISPLAY", &self.socket)
            .env("XDG_SESSION_TYPE", "wayland")
            .env("XDG_CURRENT_DESKTOP", "Waybridge")
            .env("XDG_SESSION_DESKTOP", "Waybridge")
            .env("DESKTOP_SESSION", "Waybridge");
        if let Some(services) = &self.services {
            services.configure(command);
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
            "KDE_SESSION_VERSION",
            "GNOME_DESKTOP_SESSION_ID",
            "SESSION_MANAGER",
            "XDG_ACTIVATION_TOKEN",
            "DESKTOP_STARTUP_ID",
            "WAYLAND_SOCKET",
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
        if let Some((path, dev, ino)) = &self.saved {
            if fs::symlink_metadata(path).is_ok_and(|m| m.dev() == *dev && m.ino() == *ino) {
                let _ = fs::remove_file(path);
            }
        }
        self.services.take();
    }
}
