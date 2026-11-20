//! Session-owned implementation of the XDG Settings portal color scheme.
use anyhow::Result;
use std::{
    collections::HashMap,
    sync::{
        atomic::{AtomicU32, Ordering},
        mpsc, Arc,
    },
    thread,
    time::Duration,
};
use zbus::{fdo::RequestNameFlags, zvariant::OwnedValue};

const BUS_NAME: &str = "org.freedesktop.impl.portal.desktop.waybridge";
const INTERFACE: &str = "org.freedesktop.impl.portal.Settings";
const PATH: &str = "/org/freedesktop/portal/desktop";
const NAMESPACE: &str = "org.freedesktop.appearance";
const KEY: &str = "color-scheme";

#[derive(Debug, zbus::DBusError)]
#[zbus(prefix = "org.freedesktop.portal.Error")]
enum SettingsError {
    NotFound(String),
    #[zbus(error)]
    ZBus(zbus::Error),
}

struct Settings(Arc<AtomicU32>);

#[zbus::interface(name = "org.freedesktop.impl.portal.Settings")]
impl Settings {
    #[zbus(property)]
    fn version(&self) -> u32 {
        2
    }

    fn read_all(&self, namespaces: Vec<String>) -> HashMap<String, HashMap<String, OwnedValue>> {
        let included = namespaces.is_empty()
            || namespaces.iter().any(|name| {
                name.is_empty()
                    || name == NAMESPACE
                    || name
                        .strip_suffix('*')
                        .is_some_and(|prefix| NAMESPACE.starts_with(prefix))
            });
        if !included {
            return HashMap::new();
        }
        HashMap::from([(
            NAMESPACE.into(),
            HashMap::from([(KEY.into(), self.scheme())]),
        )])
    }

    fn read(&self, namespace: &str, key: &str) -> Result<OwnedValue, SettingsError> {
        if namespace == NAMESPACE && key == KEY {
            Ok(self.scheme())
        } else {
            Err(SettingsError::NotFound(format!(
                "Unknown setting {namespace}.{key}"
            )))
        }
    }
}

impl Settings {
    fn scheme(&self) -> OwnedValue {
        self.0.load(Ordering::Relaxed).into()
    }
}

pub struct Bridge {
    scheme: Arc<AtomicU32>,
    wake: mpsc::SyncSender<()>,
}

impl Bridge {
    pub fn start(address: &str) -> Result<Self> {
        let scheme = Arc::new(AtomicU32::new(0));
        let bus = crate::session_services::connect_bus(address, Duration::from_secs(2))?;
        bus.object_server().at(PATH, Settings(scheme.clone()))?;
        bus.request_name_with_flags(BUS_NAME, RequestNameFlags::DoNotQueue.into())?;
        let (wake, changes) = mpsc::sync_channel(1);
        let state = scheme.clone();
        thread::spawn(move || {
            if let Err(error) = Self::run(bus, state, changes) {
                tracing::warn!(%error, "Waybridge Settings portal backend unavailable");
            }
        });
        Ok(Self { scheme, wake })
    }

    fn run(
        bus: zbus::blocking::Connection,
        state: Arc<AtomicU32>,
        changes: mpsc::Receiver<()>,
    ) -> Result<()> {
        while changes.recv().is_ok() {
            let value = OwnedValue::from(state.load(Ordering::Relaxed));
            bus.emit_signal(
                None::<&str>,
                PATH,
                INTERFACE,
                "SettingChanged",
                &(NAMESPACE, KEY, value),
            )?;
        }
        Ok(())
    }

    pub fn update(&self, scheme: &str) {
        let value = match scheme {
            "dark" => 1,
            "light" => 2,
            "default" => 0,
            _ => return,
        };
        if self.scheme.swap(value, Ordering::Relaxed) != value {
            let _ = self.wake.try_send(());
        }
    }
}
