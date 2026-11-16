//! StatusNotifierItem/DBusMenu desktop adapter. All bus and icon work stays on a
//! bounded worker; compositor callers only enqueue commands. XEmbed is not SNI.
use crate::{network::Peer, session_services::connect_bus};
use anyhow::{bail, Context, Result};
use base64::{engine::general_purpose::STANDARD as B64, Engine};
use image::{codecs::png::PngEncoder, ImageEncoder};
use serde_json::{json, Value};
use std::{
    collections::{BTreeMap, BTreeSet, HashMap},
    sync::{
        mpsc::{sync_channel, Receiver, SyncSender},
        Arc, Mutex,
    },
    thread,
    time::{Duration, Instant},
};
use zbus::{
    blocking::{Connection, Proxy},
    message::Header,
    object_server::SignalEmitter,
    zvariant::{OwnedValue, Structure},
};

const WATCHER: &str = "org.kde.StatusNotifierWatcher";
const WATCHER_PATH: &str = "/StatusNotifierWatcher";
const ITEM_IFACES: [&str; 2] = [
    "org.kde.StatusNotifierItem",
    "org.freedesktop.StatusNotifierItem",
];
const MAX_ITEMS: usize = 64;
const MAX_MENU: usize = 256;
type Properties = HashMap<String, OwnedValue>;
type Layout = (i32, Properties, Vec<OwnedValue>);

#[derive(Clone, Default)]
struct Watcher(Arc<Mutex<BTreeSet<String>>>);
#[zbus::interface(name = "org.kde.StatusNotifierWatcher")]
impl Watcher {
    async fn register_status_notifier_item(
        &self,
        service: &str,
        #[zbus(header)] header: Header<'_>,
        #[zbus(connection)] connection: &zbus::Connection,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
    ) -> zbus::fdo::Result<()> {
        let sender = header
            .sender()
            .ok_or_else(|| zbus::fdo::Error::InvalidArgs("sender missing".into()))?;
        let item = if service.starts_with('/') {
            format!("{sender}{service}")
        } else {
            format!("{service}/StatusNotifierItem")
        };
        let (name, path) =
            split_item(&item).map_err(|e| zbus::fdo::Error::InvalidArgs(e.to_string()))?;
        let proxy = zbus::fdo::DBusProxy::new(connection).await?;
        let registered_owner = proxy
            .get_name_owner(
                zbus::names::BusName::try_from(name)
                    .map_err(|e| zbus::fdo::Error::InvalidArgs(e.to_string()))?,
            )
            .await?;
        if registered_owner.as_str() != sender.as_str() {
            return Err(zbus::fdo::Error::AccessDenied(
                "Tray registration belongs to another bus owner".into(),
            ));
        }
        let _ = path;
        let added = {
            let mut items = self.0.lock().unwrap();
            if items.len() >= MAX_ITEMS && !items.contains(&item) {
                return Err(zbus::fdo::Error::LimitsExceeded(
                    "too many tray items".into(),
                ));
            }
            items.insert(item.clone())
        };
        if added {
            Self::status_notifier_item_registered(&emitter, &item).await?;
        }
        Ok(())
    }
    async fn register_status_notifier_host(
        &self,
        _service: &str,
        #[zbus(signal_emitter)] emitter: SignalEmitter<'_>,
    ) -> zbus::fdo::Result<()> {
        Self::status_notifier_host_registered(&emitter).await?;
        Ok(())
    }
    #[zbus(property)]
    fn registered_status_notifier_items(&self) -> Vec<String> {
        self.0.lock().unwrap().iter().cloned().collect()
    }
    #[zbus(property)]
    fn is_status_notifier_host_registered(&self) -> bool {
        true
    }
    #[zbus(property)]
    fn protocol_version(&self) -> i32 {
        0
    }
    #[zbus(signal)]
    async fn status_notifier_item_registered(
        emitter: &SignalEmitter<'_>,
        item: &str,
    ) -> zbus::Result<()>;
    #[zbus(signal)]
    async fn status_notifier_item_unregistered(
        emitter: &SignalEmitter<'_>,
        item: &str,
    ) -> zbus::Result<()>;
    #[zbus(signal)]
    async fn status_notifier_host_registered(emitter: &SignalEmitter<'_>) -> zbus::Result<()>;
    #[zbus(signal)]
    async fn status_notifier_host_unregistered(emitter: &SignalEmitter<'_>) -> zbus::Result<()>;
}

enum Command {
    Action(Arc<Peer>, Value, BTreeSet<String>),
}
#[derive(Default)]
struct Desired {
    peer: Option<Arc<Peer>>,
    revision: u64,
}
type DesiredPeer = Arc<Mutex<Desired>>;
pub struct Tray {
    sender: SyncSender<Command>,
    desired: DesiredPeer,
}
impl Tray {
    pub fn new(address: String) -> Self {
        let (sender, receiver) = sync_channel(64);
        let desired = DesiredPeer::default();
        let state = desired.clone();
        thread::spawn(move || {
            if let Err(error) = run(&address, &receiver, &state) {
                tracing::warn!(%error, "tray adapter unavailable");
                let mut revision = 0;
                loop {
                    let state = state.lock().unwrap();
                    if state.revision != revision {
                        revision = state.revision;
                        if let Some(peer) = state.peer.as_ref().filter(|p| p.tray_enabled()) {
                            peer.control(json!({"type":"tray_status","available":false,
                                "message":format!("StatusNotifier unavailable: {error}"),"xembed":false}));
                        }
                    }
                    drop(state);
                    if matches!(
                        receiver.recv_timeout(Duration::from_millis(100)),
                        Err(std::sync::mpsc::RecvTimeoutError::Disconnected)
                    ) {
                        break;
                    }
                }
            }
        });
        Self { sender, desired }
    }
    pub fn set_peer(&self, peer: Option<Arc<Peer>>) {
        // Lifecycle cannot be dropped behind queued actions. This lock only
        // exchanges an Arc/revision; the worker never holds it across D-Bus.
        let mut state = self.desired.lock().unwrap();
        state.peer = peer;
        state.revision = state.revision.wrapping_add(1);
    }
    pub fn action(&self, peer: Arc<Peer>, value: Value, windows: BTreeSet<String>) {
        if !peer.tray_enabled() || !peer.desktop_input() {
            return;
        }
        let fields = [
            "type",
            "id",
            "epoch",
            "generation",
            "action",
            "delta",
            "orientation",
            "node",
            "revision",
            "menu_token",
            "timestamp",
        ];
        if !value.as_object().is_some_and(|o| {
            o.len() <= fields.len()
                && o.iter().all(|(key, v)| {
                    fields.contains(&key.as_str())
                        && (v.is_number() || v.as_str().is_some_and(|s| s.len() <= 1024))
                })
        }) {
            peer.control(json!({"type":"tray_error","message":"Invalid tray action fields"}));
            return;
        }
        if self
            .sender
            .try_send(Command::Action(peer.clone(), value, windows))
            .is_err()
        {
            peer.control(json!({"type":"tray_error","message":"Tray action queue full"}));
        }
    }
}

struct Item {
    key: String,
    owner: String,
    path: String,
    interface: String,
    generation: u64,
    app_id: String,
    menu: Option<String>,
    menu_revision: u32,
    menu_token: u64,
    enabled_actions: BTreeSet<i32>,
    info: Value,
    png: Option<String>,
    icon_revision: u64,
}
fn split_item(item: &str) -> Result<(&str, &str)> {
    let index = item.find('/').unwrap_or(item.len());
    let (name, path) = item.split_at(index);
    zbus::names::BusName::try_from(name)?;
    let path = if path.is_empty() {
        "/StatusNotifierItem"
    } else {
        path
    };
    zbus::zvariant::ObjectPath::try_from(path)?;
    if item.len() > 1024 {
        bail!("tray identifier too long");
    }
    Ok((name, path))
}
fn owner(bus: &Connection, name: &str) -> Result<String> {
    Ok(Proxy::new(
        bus,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
    )?
    .call("GetNameOwner", &(name,))?)
}
fn text(properties: &Properties, key: &str, limit: usize) -> String {
    properties
        .get(key)
        .and_then(|v| <&str>::try_from(v).ok())
        .unwrap_or("")
        .chars()
        .filter(|c| !c.is_control())
        .take(limit)
        .collect()
}
fn boolean(properties: &Properties, key: &str, fallback: bool) -> bool {
    properties
        .get(key)
        .and_then(|v| bool::try_from(v).ok())
        .unwrap_or(fallback)
}
fn number(properties: &Properties, key: &str) -> i32 {
    properties
        .get(key)
        .and_then(|v| i32::try_from(v).ok())
        .unwrap_or(0)
}
fn properties(bus: &Connection, name: &str, path: &str, interface: &str) -> Result<Properties> {
    Ok(
        Proxy::new(bus, name, path, "org.freedesktop.DBus.Properties")?
            .call("GetAll", &(interface,))?,
    )
}
fn png(properties: &Properties, attention: bool) -> Option<String> {
    let field = if attention {
        "AttentionIconPixmap"
    } else {
        "IconPixmap"
    };
    if let Some(value) = properties.get(field).and_then(|v| v.try_clone().ok()) {
        if let Ok(pixmaps) = Vec::<(i32, i32, Vec<u8>)>::try_from(value) {
            if let Some((w, h, bytes)) = pixmaps
                .into_iter()
                .filter(|(w, h, b)| {
                    *w > 0
                        && *h > 0
                        && *w <= 512
                        && *h <= 512
                        && b.len() == *w as usize * *h as usize * 4
                })
                .min_by_key(|(w, h, _)| (w - 128).abs() + (h - 128).abs())
            {
                let rgba: Vec<u8> = bytes
                    .chunks_exact(4)
                    .flat_map(|p| [p[1], p[2], p[3], p[0]])
                    .collect();
                let source = image::RgbaImage::from_raw(w as u32, h as u32, rgba)?;
                let image = image::imageops::resize(
                    &source,
                    128,
                    128,
                    image::imageops::FilterType::Lanczos3,
                );
                let mut output = Vec::new();
                PngEncoder::new(&mut output)
                    .write_image(&image, 128, 128, image::ExtendedColorType::Rgba8)
                    .ok()?;
                if output.len() <= 96 * 1024 {
                    return Some(B64.encode(output));
                }
            }
        }
    }
    let name = text(
        properties,
        if attention {
            "AttentionIconName"
        } else {
            "IconName"
        },
        4096,
    );
    crate::app_icon::tray_icon(&name, &text(properties, "IconThemePath", 4096))
        .map(|v| B64.encode(v))
        .or_else(|| {
            if attention {
                png(properties, false)
            } else {
                None
            }
        })
}
fn item(bus: &Connection, key: String, generation: u64) -> Result<Item> {
    let (name, path) = split_item(&key)?;
    let source_owner = owner(bus, name)?;
    let (interface, properties) = ITEM_IFACES
        .iter()
        .find_map(|iface| {
            properties(bus, &source_owner, path, iface)
                .ok()
                .map(|p| ((*iface).to_owned(), p))
        })
        .context("StatusNotifierItem interface missing")?;
    let menu = properties
        .get("Menu")
        .and_then(|v| <&zbus::zvariant::ObjectPath>::try_from(v).ok())
        .map(|p| p.as_str().to_owned())
        .filter(|p| p != "/");
    let status = text(&properties, "Status", 32);
    let app_id = text(&properties, "DesktopEntry", 255);
    let app_id = if app_id.is_empty() {
        text(&properties, "Id", 255)
    } else {
        app_id
    };
    let title = text(&properties, "Title", 255);
    let png = png(&properties, status == "NeedsAttention");
    let info = json!({"id":key,"generation":generation,"app_id":app_id,"title":title,
        "status":status,"menu":menu.is_some(),"item_is_menu":boolean(&properties,"ItemIsMenu",false),
        "icon_revision":generation,"width":128,"height":128});
    Ok(Item {
        path: path.to_owned(),
        key,
        owner: source_owner,
        interface,
        generation,
        app_id,
        menu,
        menu_revision: 0,
        menu_token: 0,
        enabled_actions: BTreeSet::new(),
        info,
        png,
        icon_revision: generation,
    })
}
fn layout(
    value: OwnedValue,
    depth: usize,
    budget: &mut usize,
    enabled: &mut BTreeSet<i32>,
    ancestors: bool,
) -> Result<Value> {
    if depth > 8 || *budget == 0 {
        bail!("DBusMenu exceeds item/depth limit");
    }
    *budget -= 1;
    let structure = Structure::try_from(value)?;
    let mut fields = structure.into_fields().into_iter();
    let id = i32::try_from(fields.next().context("missing menu id")?)?;
    let props = Properties::try_from(OwnedValue::try_from(
        fields.next().context("missing menu properties")?,
    )?)?;
    let children = Vec::<OwnedValue>::try_from(OwnedValue::try_from(
        fields.next().context("missing menu children")?,
    )?)?;
    let visible = boolean(&props, "visible", true);
    let active = boolean(&props, "enabled", true);
    let separator = text(&props, "type", 32) == "separator";
    let submenu = text(&props, "children-display", 32) == "submenu" || !children.is_empty();
    let mut result = Vec::new();
    for child in children {
        result.push(layout(
            child,
            depth + 1,
            budget,
            enabled,
            ancestors && visible && active,
        )?);
    }
    if ancestors && visible && active && !separator && !submenu && result.is_empty() {
        enabled.insert(id);
    }
    Ok(
        json!({"id":id,"label":text(&props,"label",512),"enabled":active,"visible":visible,
        "separator":separator,"submenu":submenu,"toggle":text(&props,"toggle-type",32),"checked":number(&props,"toggle-state")==1,"children":result}),
    )
}
fn menu(
    bus: &Connection,
    item: &mut Item,
    epoch: u64,
    peer: &Peer,
    current: &impl Fn() -> Result<()>,
) -> Result<()> {
    let path = item
        .menu
        .as_deref()
        .context("Application supplies no DBusMenu")?;
    let proxy = Proxy::new(bus, &*item.owner, path, "com.canonical.dbusmenu")?;
    // Applications populate dynamic menus in this callback. GetLayout follows it.
    current()?;
    let _: Result<bool, zbus::Error> = proxy.call("AboutToShow", &(0i32,));
    current()?;
    let (revision, root): (u32, Layout) =
        proxy.call("GetLayout", &(0i32, 8i32, Vec::<String>::new()))?;
    let root = OwnedValue::try_from(zbus::zvariant::Value::from(root))?;
    let mut enabled = BTreeSet::new();
    let mut budget = MAX_MENU;
    let nodes = layout(root, 0, &mut budget, &mut enabled, true)?;
    current()?;
    item.menu_revision = revision;
    item.menu_token = item.menu_token.wrapping_add(1).max(1);
    item.enabled_actions = enabled;
    peer.control(
        json!({"type":"tray_menu","epoch":epoch,"id":item.key,"generation":item.generation,
        "revision":revision,"menu_token":item.menu_token,"root":nodes}),
    );
    Ok(())
}
fn action(
    bus: &Connection,
    items: &mut BTreeMap<String, Item>,
    epoch: u64,
    peer: &Peer,
    value: &Value,
    windows: &BTreeSet<String>,
    lifecycle: (&DesiredPeer, u64),
) -> Result<()> {
    let (desired, desired_revision) = lifecycle;
    let current = || -> Result<()> {
        if !peer.desktop_input() || desired.lock().unwrap().revision != desired_revision {
            bail!("Tray action superseded by disconnect, reconnect or read-only policy");
        }
        Ok(())
    };
    current()?;
    if value["epoch"].as_u64() != Some(epoch) {
        bail!("stale tray connection");
    }
    let id = value["id"].as_str().context("missing tray id")?;
    let item = items.get_mut(id).context("tray item no longer exists")?;
    if value["generation"].as_u64() != Some(item.generation)
        || owner(bus, split_item(&item.key)?.0)? != item.owner
    {
        bail!("stale tray application owner");
    }
    current()?;
    let proxy = Proxy::new(
        bus,
        item.owner.clone(),
        item.path.clone(),
        item.interface.clone(),
    )?;
    current()?;
    match value["action"].as_str().unwrap_or("") {
        "activate" => {
            let _: () = proxy.call("Activate", &(0i32, 0i32))?;
        }
        "secondary" => {
            let _: () = proxy.call("SecondaryActivate", &(0i32, 0i32))?;
        }
        "scroll" => {
            let delta = value["delta"]
                .as_i64()
                .filter(|v| v.unsigned_abs() <= 1200)
                .context("invalid scroll amount")? as i32;
            let orientation = value["orientation"]
                .as_str()
                .filter(|v| matches!(*v, "vertical" | "horizontal"))
                .context("invalid scroll orientation")?;
            let _: () = proxy.call("Scroll", &(delta, orientation))?;
        }
        "menu" if item.menu.is_some() => menu(bus, item, epoch, peer, &current)?,
        "menu" => {
            if !windows
                .iter()
                .any(|w| w.trim_end_matches(".desktop") == item.app_id.trim_end_matches(".desktop"))
            {
                bail!("No DBusMenu or matching remote window; application context menu cannot be displayed");
            }
            let _: () = proxy.call("ContextMenu", &(0i32, 0i32))?;
            peer.control(json!({"type":"tray_menu","epoch":epoch,"id":id,"generation":item.generation,"fallback":"remote_window"}));
        }
        "menu_event" => {
            let node = value["node"]
                .as_i64()
                .and_then(|n| i32::try_from(n).ok())
                .context("invalid menu id")?;
            if value["menu_token"].as_u64() != Some(item.menu_token)
                || value["revision"].as_u64() != Some(u64::from(item.menu_revision))
                || !item.enabled_actions.contains(&node)
            {
                bail!("stale or disabled menu action");
            }
            let menu = Proxy::new(
                bus,
                &*item.owner,
                item.menu.as_deref().context("menu unavailable")?,
                "com.canonical.dbusmenu",
            )?;
            // Verify the application has not changed its layout while a Windows menu was open.
            let (revision, _): (u32, Layout) =
                menu.call("GetLayout", &(0i32, 0i32, Vec::<String>::new()))?;
            if revision != item.menu_revision {
                bail!("Application menu changed; reopen it");
            }
            current()?;
            let _: () = menu.call(
                "Event",
                &(
                    node,
                    "clicked",
                    zbus::zvariant::Value::from(0i32),
                    value["timestamp"].as_u64().unwrap_or(0) as u32,
                ),
            )?;
            item.enabled_actions.clear();
        }
        _ => bail!("unsupported tray action"),
    }
    Ok(())
}
fn run(address: &str, receiver: &Receiver<Command>, desired: &DesiredPeer) -> Result<()> {
    let bus = connect_bus(address, Duration::from_millis(500))?;
    let registry = Watcher::default();
    bus.object_server().at(WATCHER_PATH, registry.clone())?;
    let host = format!("org.kde.StatusNotifierHost-{}", std::process::id());
    let freedesktop_host = format!("org.freedesktop.StatusNotifierHost-{}", std::process::id());
    bus.request_name(host.clone())?;
    bus.request_name(freedesktop_host.clone())?;
    let mut watcher_owner = String::new();
    let mut watcher_name = WATCHER.to_owned();
    let mut own_watcher = false;
    let mut peer: Option<Arc<Peer>> = None;
    let mut epoch = 0u64;
    let mut generation = 0u64;
    let mut items = BTreeMap::<String, Item>::new();
    let mut previous = Value::Null;
    let mut sent_icons = BTreeMap::new();
    let mut warned_icon_budget = false;
    let mut next_poll = Instant::now();
    let mut desired_revision = 0;
    let mut last_key = String::new();
    loop {
        {
            let current = desired.lock().unwrap();
            if current.revision != desired_revision {
                desired_revision = current.revision;
                peer = current.peer.clone().filter(|p| p.tray_enabled());
                epoch = epoch.wrapping_add(1).max(1);
                previous = Value::Null;
                sent_icons.clear();
                warned_icon_budget = false;
                watcher_owner.clear();
                next_poll = Instant::now();
                for item in items.values_mut() {
                    item.menu_token = 0;
                    item.enabled_actions.clear();
                }
            }
        }
        match receiver.recv_timeout(
            next_poll
                .saturating_duration_since(Instant::now())
                .min(Duration::from_millis(100)),
        ) {
            Ok(Command::Action(source, value, windows)) => {
                if peer.as_ref().is_some_and(|p| Arc::ptr_eq(p, &source))
                    && source.desktop_input()
                    && desired.lock().unwrap().revision == desired_revision
                {
                    if let Err(error) = action(
                        &bus,
                        &mut items,
                        epoch,
                        &source,
                        &value,
                        &windows,
                        (desired, desired_revision),
                    ) {
                        source.control(json!({"type":"tray_error","id":value["id"],"message":error.to_string()}));
                    }
                }
            }
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => return Ok(()),
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => (),
        }
        if Instant::now() < next_poll {
            continue;
        }
        next_poll = Instant::now() + Duration::from_millis(750);
        let watcher = [WATCHER, "org.freedesktop.StatusNotifierWatcher"]
            .into_iter()
            .find_map(|name| owner(&bus, name).ok().map(|o| (name.to_owned(), o)));
        let (name, current_owner) = if let Some(watcher) = watcher {
            watcher
        } else {
            // DoNotQueue + no ReplaceExisting: a racing host wins without being displaced.
            match bus
                .request_name_with_flags(WATCHER, zbus::fdo::RequestNameFlags::DoNotQueue.into())
            {
                Ok(
                    zbus::fdo::RequestNameReply::PrimaryOwner
                    | zbus::fdo::RequestNameReply::AlreadyOwner,
                ) => {
                    own_watcher = true;
                    (WATCHER.to_owned(), owner(&bus, WATCHER)?)
                }
                result => {
                    let error = format!("{result:?}");
                    if let Some(p) = peer.as_ref().filter(|p| !p.is_closed()) {
                        p.control(json!({"type":"tray_status","available":false,"message":format!("Watcher conflict: {error}"),"xembed":false}));
                    }
                    continue;
                }
            }
        };
        if current_owner != watcher_owner || name != watcher_name {
            watcher_owner = current_owner;
            watcher_name = name;
            items.clear();
            previous = Value::Null;
            let watcher = Proxy::new(&bus, &*watcher_owner, WATCHER_PATH, &*watcher_name)?;
            let host_name = if watcher_name == WATCHER {
                &host
            } else {
                &freedesktop_host
            };
            let registered: Result<(), _> =
                watcher.call("RegisterStatusNotifierHost", &(host_name,));
            if let Some(p) = peer.as_ref().filter(|p| !p.is_closed()) {
                p.control(json!({"type":"tray_status","available":registered.is_ok(),
                "mode":if own_watcher {"watcher_host"}else{"existing_watcher_host"},"xembed":false,
                "message":registered.err().map(|e|e.to_string())}));
            }
        }
        let watcher = Proxy::new(
            &bus,
            watcher_owner.clone(),
            WATCHER_PATH,
            watcher_name.clone(),
        )?;
        let keys: Vec<String> = match watcher.get_property("RegisteredStatusNotifierItems") {
            Ok(v) => v,
            Err(_) => {
                watcher_owner.clear();
                continue;
            }
        };
        if keys.len() > MAX_ITEMS {
            if let Some(p) = peer.as_ref().filter(|p| !p.is_closed()) {
                p.control(json!({"type":"tray_warning","message":"More than 64 tray items; remaining applications are not forwarded"}));
            }
        }
        let keys: BTreeSet<_> = keys.into_iter().take(MAX_ITEMS).collect();
        items.retain(|key, _| keys.contains(key));
        let selected: Vec<_> = keys
            .range((
                std::ops::Bound::Excluded(last_key.clone()),
                std::ops::Bound::Unbounded,
            ))
            .chain(keys.range(..=last_key.clone()))
            .take(8)
            .cloned()
            .collect();
        let deadline = Instant::now() + Duration::from_millis(750);
        for (index, key) in selected.into_iter().enumerate() {
            if index > 0
                && (Instant::now() > deadline
                    || desired.lock().unwrap().revision != desired_revision)
            {
                break;
            }
            last_key = key.clone();
            generation = generation.wrapping_add(1).max(1);
            if let Ok(mut fresh) = item(&bus, key.clone(), generation) {
                if let Some(old) = items.remove(&key).filter(|old| old.owner == fresh.owner) {
                    if fresh.png == old.png {
                        fresh.icon_revision = old.icon_revision;
                    }
                    fresh.info["icon_revision"] = json!(fresh.icon_revision);
                    fresh.generation = old.generation;
                    fresh.info["generation"] = json!(old.generation);
                    fresh.menu_revision = old.menu_revision;
                    fresh.menu_token = old.menu_token;
                    fresh.enabled_actions = old.enabled_actions;
                }
                items.insert(key, fresh);
            } else if own_watcher
                && split_item(&key).is_ok_and(|(name, _)| owner(&bus, name).is_err())
            {
                items.remove(&key);
                registry.0.lock().unwrap().remove(&key);
                let _ = bus.emit_signal(
                    None::<&str>,
                    WATCHER_PATH,
                    WATCHER,
                    "StatusNotifierItemUnregistered",
                    &(key,),
                );
            }
        }
        let snapshot = json!({"type":"tray_snapshot","epoch":epoch,"items":items.values().map(|v|&v.info).collect::<Vec<_>>()});
        if snapshot != previous {
            if let Some(peer) = peer.as_ref().filter(|p| !p.is_closed()) {
                if serde_json::to_vec(&snapshot)?.len() > 256 * 1024 {
                    bail!("Tray metadata exceeds 256KiB budget");
                }
                peer.control(snapshot.clone());
                previous = snapshot;
            }
        }
        sent_icons.retain(|key, _| items.contains_key(key));
        if let Some(peer) = peer.as_ref().filter(|p| !p.is_closed()) {
            for (key, item) in &items {
                let version = (item.generation, item.icon_revision);
                if sent_icons.get(key) == Some(&version) {
                    continue;
                }
                if peer.tray_icon(key,json!({"type":"tray_icon","epoch":epoch,"id":key,
                    "generation":item.generation,"revision":item.icon_revision,"png":item.png,"width":128,"height":128})) {
                    sent_icons.insert(key.clone(),version);
                } else if !warned_icon_budget {
                    peer.control(json!({"type":"tray_warning","message":"Tray icon queue budget reached; placeholders remain until pending icons can be sent"}));
                    warned_icon_budget=true;
                }
            }
        }
    }
}
