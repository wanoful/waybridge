//! One-way admission: existing desktops block creation, never attachment.
use anyhow::{bail, Context, Result};
use std::{
    collections::HashMap,
    fs::{self, File, OpenOptions},
    os::{
        fd::AsRawFd,
        unix::fs::{MetadataExt, OpenOptionsExt, PermissionsExt},
    },
    path::Path,
    time::Duration,
};
use zbus::zvariant::{OwnedObjectPath, OwnedValue};

pub struct Guard {
    _lock: File,
}

fn graphical(properties: &HashMap<String, OwnedValue>) -> Result<bool> {
    let value = |key: &str| -> Result<&str> {
        properties
            .get(key)
            .and_then(|v| <&str>::try_from(v).ok())
            .with_context(|| format!("logind session is missing {key}"))
    };
    let class = value("Class")?;
    Ok(matches!(value("Type")?, "wayland" | "x11")
        && matches!(value("State")?, "active" | "online")
        && (class == "user" || class.starts_with("user-")))
}

pub fn check_desktops() -> Result<()> {
    let bus = zbus::blocking::connection::Builder::system()?
        .method_timeout(Duration::from_secs(3))
        .build()
        .context("Cannot check desktop sessions: system D-Bus/logind is unavailable")?;
    let sessions: Vec<(String, u32, String, String, OwnedObjectPath)> = bus
        .call_method(
            Some("org.freedesktop.login1"),
            "/org/freedesktop/login1",
            Some("org.freedesktop.login1.Manager"),
            "ListSessions",
            &(),
        )
        .context("Cannot check desktop sessions through logind")?
        .body()
        .deserialize()?;
    for (id, uid, _, _, path) in sessions {
        if uid != unsafe { libc::getuid() } {
            continue;
        }
        let reply = bus.call_method(
            Some("org.freedesktop.login1"),
            path.as_str(),
            Some("org.freedesktop.DBus.Properties"),
            "GetAll",
            &("org.freedesktop.login1.Session",),
        );
        let reply = match reply {
            Ok(reply) => reply,
            Err(zbus::Error::MethodError(name, _, _))
                if matches!(
                    name.as_str(),
                    "org.freedesktop.DBus.Error.UnknownObject"
                        | "org.freedesktop.login1.NoSuchSession"
                ) =>
            {
                continue
            }
            Err(error) => {
                return Err(error).context("Cannot inspect desktop session through logind")
            }
        };
        let properties: HashMap<String, OwnedValue> = reply.body().deserialize()?;
        if graphical(&properties)? {
            bail!("This user already has a graphical desktop session ({id}). Log out of that desktop before creating a Waybridge session; locking the screen is not enough.");
        }
    }
    Ok(())
}

impl Guard {
    pub fn acquire(runtime: &Path) -> Result<Self> {
        let metadata = fs::symlink_metadata(runtime)?;
        if !metadata.is_dir()
            || metadata.uid() != unsafe { libc::getuid() }
            || metadata.permissions().mode() & 0o077 != 0
        {
            bail!("XDG_RUNTIME_DIR must be a private directory owned by this user");
        }
        let lock = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
            .open(runtime.join("waybridge-session.lock"))?;
        let metadata = lock.metadata()?;
        if !metadata.is_file()
            || metadata.uid() != unsafe { libc::getuid() }
            || metadata.permissions().mode() & 0o077 != 0
        {
            bail!("Invalid Waybridge session lock");
        }
        if unsafe { libc::flock(lock.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 {
            bail!("A Waybridge session is already running or starting for this user. Reconnect to it, or end it before creating another session.");
        }
        check_desktops()?;
        Ok(Self { _lock: lock })
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn only_live_user_graphics_block_creation() {
        for (kind, class, state, expected) in [
            ("wayland", "user", "active", true),
            ("x11", "user", "online", true),
            ("wayland", "user-light", "online", true),
            ("wayland", "user", "closing", false),
            ("wayland", "greeter", "active", false),
            ("wayland", "lock-screen", "active", false),
            ("tty", "user", "active", false),
            ("unspecified", "manager", "online", false),
        ] {
            let properties = [("Type", kind), ("Class", class), ("State", state)]
                .into_iter()
                .map(|(key, value)| (key.into(), zbus::zvariant::Str::from(value).into()))
                .collect();
            assert_eq!(
                graphical(&properties).unwrap(),
                expected,
                "{kind}/{class}/{state}"
            );
        }
        assert!(graphical(&HashMap::new()).is_err());
    }
}
