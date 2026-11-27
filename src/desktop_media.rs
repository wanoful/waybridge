//! MPRIS2 session bus adapter. All D-Bus and artwork work stays on a bounded
//! worker thread; compositor callers only update lifecycle state or enqueue commands.
use crate::{network::Peer, session_services::connect_bus};
use anyhow::{bail, Context, Result};
use base64::{engine::general_purpose::STANDARD as B64, Engine};
use image::{codecs::png::PngEncoder, ImageEncoder};
use percent_encoding::percent_decode_str;
use serde_json::{json, Value};
use std::{
    collections::{BTreeMap, BTreeSet, HashMap},
    fs::OpenOptions,
    io::Read,
    os::unix::{
        ffi::OsStringExt,
        fs::{MetadataExt, OpenOptionsExt},
    },
    path::{Path, PathBuf},
    sync::{
        mpsc::{sync_channel, Receiver, SyncSender},
        Arc, Mutex,
    },
    thread,
    time::{Duration, Instant},
};
use zbus::{
    blocking::{Connection, Proxy},
    zvariant::{ObjectPath, OwnedValue},
};

const MPRIS_PREFIX: &str = "org.mpris.MediaPlayer2.";
const MPRIS_PATH: &str = "/org/mpris/MediaPlayer2";
const MPRIS_ROOT_IFACE: &str = "org.mpris.MediaPlayer2";
const MPRIS_PLAYER_IFACE: &str = "org.mpris.MediaPlayer2.Player";
const MAX_PLAYERS: usize = 16;
const POLL_INTERVAL: Duration = Duration::from_millis(350);
const POSITION_DRIFT_US: i64 = 1_500_000;
const MAX_ART_READ_BYTES: u64 = 4 * 1024 * 1024;
const MAX_ART_PASSTHROUGH_BYTES: usize = 256 * 1024;
const MAX_ART_PNG_BYTES: usize = 192 * 1024;
const MAX_ART_DATA_URI_B64: usize = 384 * 1024;
const ART_MAX_EDGE: u32 = 256;
const MAX_CONTROL_QUEUE: usize = 64;

type Properties = HashMap<String, OwnedValue>;

#[derive(Default)]
struct Desired {
    peer: Option<Arc<Peer>>,
    enabled: bool,
    revision: u64,
}
type DesiredPeer = Arc<Mutex<Desired>>;

enum Command {
    Wake,
    Control(Arc<Peer>, Value),
}

pub struct Media {
    sender: SyncSender<Command>,
    desired: DesiredPeer,
}

impl Media {
    pub fn new(address: String) -> Self {
        let (sender, receiver) = sync_channel(MAX_CONTROL_QUEUE);
        let desired = Arc::new(Mutex::new(Desired::default()));
        let worker_desired = desired.clone();
        thread::spawn(move || {
            let mut backoff = Duration::from_millis(250);
            loop {
                match run(&address, &receiver, &worker_desired) {
                    Ok(()) => return,
                    Err(error) => {
                        tracing::debug!(%error, "MPRIS2 media adapter unavailable; retrying");
                        match receiver.recv_timeout(backoff) {
                            Ok(Command::Wake) => backoff = Duration::from_millis(250),
                            Ok(Command::Control(_, _)) => (),
                            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => return,
                            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => {
                                backoff = (backoff * 2).min(Duration::from_secs(5));
                            }
                        }
                    }
                }
            }
        });
        Self { sender, desired }
    }

    pub fn set_peer(&self, peer: Option<Arc<Peer>>, enabled: bool) {
        {
            let mut state = self.desired.lock().unwrap();
            let active = peer.filter(|p| p.media_controls_enabled());
            state.enabled = active.is_some() && enabled;
            state.peer = active;
            state.revision = state.revision.wrapping_add(1);
        }
        let _ = self.sender.try_send(Command::Wake);
    }

    pub fn set_enabled(&self, peer: Option<Arc<Peer>>, enabled: bool) {
        {
            let mut state = self.desired.lock().unwrap();
            let active = peer.filter(|p| p.media_controls_enabled());
            let next_enabled = active.is_some() && enabled;
            let same_peer = match (&state.peer, &active) {
                (Some(a), Some(b)) => Arc::ptr_eq(a, b),
                (None, None) => true,
                _ => false,
            };
            if same_peer && state.enabled == next_enabled {
                return;
            }
            state.peer = active;
            state.enabled = next_enabled;
            state.revision = state.revision.wrapping_add(1);
        }
        let _ = self.sender.try_send(Command::Wake);
    }

    pub fn control(&self, peer: Arc<Peer>, value: Value) {
        if !peer.media_controls_enabled() || !peer.desktop_input() {
            return;
        }
        let fields = [
            "type",
            "epoch",
            "player",
            "generation",
            "action",
            "track_id",
            "position_us",
        ];
        if !value.as_object().is_some_and(|o| {
            o.len() <= fields.len()
                && o.iter().all(|(key, v)| {
                    fields.contains(&key.as_str())
                        && (v.is_number() || v.as_str().is_some_and(|s| s.len() <= 1024))
                })
        }) {
            return;
        }
        let _ = self.sender.try_send(Command::Control(peer, value));
    }
}

#[derive(Clone, PartialEq, Eq)]
struct ArtPayload {
    mime: &'static str,
    b64: String,
}

#[derive(Clone, PartialEq, Eq)]
enum ArtCacheKey {
    File(PathBuf, u64, i64, i64),
    Data(String),
    Fallback(String),
    None,
}

#[derive(Clone)]
struct PlayerState {
    bus_name: String,
    owner: String,
    generation: u64,
    last_activity: u64,
    identity: String,
    desktop_entry: String,
    status: String,
    title: String,
    artists: Vec<String>,
    album: String,
    album_artists: Vec<String>,
    track_id: String,
    length_us: i64,
    position_us: Option<i64>,
    rate: f64,
    can_control: bool,
    can_play: bool,
    can_pause: bool,
    can_go_next: bool,
    can_go_previous: bool,
    can_seek: bool,
    art_key: ArtCacheKey,
    art_payload: Option<ArtPayload>,
    art_revision: u64,
}

impl PlayerState {
    fn is_eligible(&self) -> bool {
        self.status != "Stopped"
            || !self.title.is_empty()
            || !self.artists.is_empty()
            || !self.album.is_empty()
    }

    fn status_rank(&self) -> u8 {
        match self.status.as_str() {
            "Playing" => 3,
            "Paused" => 2,
            _ => 1,
        }
    }

    fn structural_json(&self, epoch: u64) -> Value {
        json!({
            "type": "media_state",
            "epoch": epoch,
            "player": self.bus_name,
            "generation": self.generation,
            "identity": self.identity,
            "desktop_entry": self.desktop_entry,
            "status": self.status,
            "title": self.title,
            "artists": self.artists,
            "album": self.album,
            "album_artists": self.album_artists,
            "track_id": self.track_id,
            "length_us": self.length_us,
            "rate": self.rate,
            "can_control": self.can_control,
            "can_play": self.can_play,
            "can_pause": self.can_pause,
            "can_go_next": self.can_go_next,
            "can_go_previous": self.can_go_previous,
            "can_seek": self.can_seek,
            "art_revision": if self.art_payload.is_some() { self.art_revision } else { 0 },
        })
    }

    fn full_json(&self, epoch: u64) -> Value {
        let mut message = self.structural_json(epoch);
        if let Some(position_us) = self.position_us {
            message["position_us"] = json!(position_us);
        }
        message
    }
}

struct SentState {
    structural: Value,
    position_us: Option<i64>,
    sent_at: Instant,
    status: String,
    rate: f64,
}

fn sanitize_text(value: &str, limit: usize) -> String {
    value
        .chars()
        .filter(|c| !c.is_control())
        .take(limit)
        .collect()
}

fn prop_string(properties: &Properties, key: &str, limit: usize) -> String {
    properties
        .get(key)
        .and_then(|v| <&str>::try_from(v).ok())
        .map(|s| sanitize_text(s, limit))
        .unwrap_or_default()
}

fn prop_bool(properties: &Properties, key: &str) -> bool {
    properties
        .get(key)
        .and_then(|v| bool::try_from(v).ok())
        .unwrap_or(false)
}

fn prop_f64(properties: &Properties, key: &str, fallback: f64) -> f64 {
    properties
        .get(key)
        .and_then(|v| f64::try_from(v).ok())
        .filter(|v| v.is_finite() && *v > 0.0 && *v <= 16.0)
        .unwrap_or(fallback)
}

fn value_to_i64(value: &OwnedValue) -> Option<i64> {
    if let Ok(v) = i64::try_from(value) {
        return Some(v);
    }
    if let Ok(v) = u64::try_from(value) {
        return i64::try_from(v).ok();
    }
    if let Ok(v) = i32::try_from(value) {
        return Some(i64::from(v));
    }
    if let Ok(v) = u32::try_from(value) {
        return Some(i64::from(v));
    }
    if let Ok(v) = f64::try_from(value) {
        if v.is_finite() && v >= 0.0 && v <= i64::MAX as f64 {
            return Some(v as i64);
        }
    }
    None
}

fn metadata_string(metadata: &Properties, key: &str, limit: usize) -> String {
    let Some(value) = metadata.get(key) else {
        return String::new();
    };
    if let Ok(s) = <&str>::try_from(value) {
        return sanitize_text(s, limit);
    }
    if let Ok(path) = <&ObjectPath>::try_from(value) {
        return sanitize_text(path.as_str(), limit);
    }
    String::new()
}

fn metadata_string_list(
    metadata: &Properties,
    key: &str,
    max_items: usize,
    limit: usize,
) -> Vec<String> {
    let Some(value) = metadata.get(key) else {
        return Vec::new();
    };
    if let Ok(cloned) = value.try_clone() {
        if let Ok(items) = Vec::<String>::try_from(cloned) {
            return items
                .into_iter()
                .map(|s| sanitize_text(&s, limit))
                .filter(|s| !s.is_empty())
                .take(max_items)
                .collect();
        }
    }
    if let Ok(single) = <&str>::try_from(value) {
        let cleaned = sanitize_text(single, limit);
        if !cleaned.is_empty() {
            return vec![cleaned];
        }
    }
    Vec::new()
}

fn normalize_status(raw: &str) -> &'static str {
    match raw {
        "Playing" => "Playing",
        "Paused" => "Paused",
        _ => "Stopped",
    }
}

fn parse_file_url(url: &str) -> Option<PathBuf> {
    let rest = url.strip_prefix("file://")?;
    let path_part = if let Some(after_localhost) = rest.strip_prefix("localhost/") {
        format!("/{after_localhost}")
    } else if rest.starts_with('/') {
        rest.to_owned()
    } else {
        return None;
    };
    let path_part = path_part.split(['?', '#']).next().unwrap_or(&path_part);
    let decoded = percent_decode_str(path_part).collect::<Vec<u8>>();
    if decoded.is_empty() || decoded.len() > 4096 || decoded.contains(&0) {
        return None;
    }
    let path = PathBuf::from(std::ffi::OsString::from_vec(decoded));
    path.is_absolute().then_some(path)
}

fn sniff_and_encode_art(bytes: &[u8]) -> Option<ArtPayload> {
    if bytes.len() < 12 {
        return None;
    }
    if bytes.starts_with(&[137, 80, 78, 71, 13, 10, 26, 10]) {
        let mut reader =
            image::ImageReader::with_format(std::io::Cursor::new(bytes), image::ImageFormat::Png);
        let mut limits = image::Limits::default();
        limits.max_image_width = Some(2048);
        limits.max_image_height = Some(2048);
        limits.max_alloc = Some(16 * 1024 * 1024);
        reader.limits(limits);
        let image = reader.decode().ok()?;
        if image.width() == 0 || image.height() == 0 {
            return None;
        }
        let rgba = if image.width() > ART_MAX_EDGE || image.height() > ART_MAX_EDGE {
            image
                .resize(
                    ART_MAX_EDGE,
                    ART_MAX_EDGE,
                    image::imageops::FilterType::Lanczos3,
                )
                .to_rgba8()
        } else {
            image.to_rgba8()
        };
        let mut png = Vec::new();
        PngEncoder::new(&mut png)
            .write_image(
                rgba.as_raw(),
                rgba.width(),
                rgba.height(),
                image::ExtendedColorType::Rgba8,
            )
            .ok()?;
        if png.len() <= MAX_ART_PNG_BYTES {
            return Some(ArtPayload {
                mime: "image/png",
                b64: B64.encode(png),
            });
        }
        return None;
    }
    if bytes.starts_with(&[0xFF, 0xD8, 0xFF]) && bytes.len() <= MAX_ART_PASSTHROUGH_BYTES {
        return Some(ArtPayload {
            mime: "image/jpeg",
            b64: B64.encode(bytes),
        });
    }
    if &bytes[0..4] == b"RIFF"
        && &bytes[8..12] == b"WEBP"
        && bytes.len() <= MAX_ART_PASSTHROUGH_BYTES
    {
        return Some(ArtPayload {
            mime: "image/webp",
            b64: B64.encode(bytes),
        });
    }
    None
}

fn decode_data_art_uri(uri: &str) -> Option<ArtPayload> {
    let rest = uri.strip_prefix("data:")?;
    let (header, payload) = rest.split_once(',')?;
    if payload.len() > MAX_ART_DATA_URI_B64 {
        return None;
    }
    let header_lower = header.to_ascii_lowercase();
    if !matches!(
        header_lower.as_str(),
        "image/png;base64" | "image/jpeg;base64" | "image/jpg;base64" | "image/webp;base64"
    ) {
        return None;
    }
    let decoded = B64.decode(payload.trim()).ok()?;
    sniff_and_encode_art(&decoded)
}

fn load_file_art(path: &Path) -> Option<ArtPayload> {
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NONBLOCK)
        .open(path)
        .ok()?;
    let meta = file.metadata().ok()?;
    if !meta.is_file() || meta.len() == 0 || meta.len() > MAX_ART_READ_BYTES {
        return None;
    }
    let mut bytes = Vec::with_capacity(meta.len().min(MAX_ART_READ_BYTES) as usize);
    file.take(MAX_ART_READ_BYTES + 1)
        .read_to_end(&mut bytes)
        .ok()?;
    if bytes.len() as u64 > MAX_ART_READ_BYTES {
        return None;
    }
    sniff_and_encode_art(&bytes)
}

fn classify_art_key(art_url: &str, desktop_entry: &str) -> ArtCacheKey {
    if let Some(path) = parse_file_url(art_url) {
        if let Ok(meta) = std::fs::metadata(&path) {
            if meta.is_file() && meta.len() > 0 && meta.len() <= MAX_ART_READ_BYTES {
                return ArtCacheKey::File(path, meta.len(), meta.mtime(), meta.mtime_nsec());
            }
        }
    } else if art_url.starts_with("data:image/") && art_url.len() <= MAX_ART_DATA_URI_B64 + 64 {
        return ArtCacheKey::Data(art_url.to_owned());
    }
    if !desktop_entry.is_empty() {
        ArtCacheKey::Fallback(desktop_entry.to_owned())
    } else {
        ArtCacheKey::None
    }
}

fn resolve_art(key: &ArtCacheKey, desktop_entry: &str) -> Option<ArtPayload> {
    let primary = match key {
        ArtCacheKey::File(path, _, _, _) => load_file_art(path),
        ArtCacheKey::Data(uri) => decode_data_art_uri(uri),
        ArtCacheKey::Fallback(_) | ArtCacheKey::None => None,
    };
    if primary.is_some() {
        return primary;
    }
    if !desktop_entry.is_empty() {
        return crate::app_icon::notification_icon("", desktop_entry).map(|png| ArtPayload {
            mime: "image/png",
            b64: B64.encode(png),
        });
    }
    None
}

fn position_drifted(sent: &SentState, now: Instant, actual_us: Option<i64>) -> bool {
    let Some(actual) = actual_us else {
        return false;
    };
    let Some(previous) = sent.position_us else {
        return true;
    };
    let expected = if sent.status == "Playing" {
        let elapsed_us = now.saturating_duration_since(sent.sent_at).as_micros() as f64;
        previous.saturating_add((elapsed_us * sent.rate) as i64)
    } else {
        previous
    };
    actual.abs_diff(expected) > POSITION_DRIFT_US as u64
}

fn select_active_player(players: &BTreeMap<String, PlayerState>) -> Option<&PlayerState> {
    players.values().filter(|p| p.is_eligible()).max_by(|a, b| {
        a.status_rank()
            .cmp(&b.status_rank())
            .then_with(|| a.last_activity.cmp(&b.last_activity))
            .then_with(|| b.bus_name.cmp(&a.bus_name))
    })
}

fn dbus_proxy(bus: &Connection) -> Result<Proxy<'_>> {
    Ok(Proxy::new(
        bus,
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
    )?)
}

fn name_owner(bus: &Connection, name: &str) -> Result<String> {
    Ok(dbus_proxy(bus)?.call("GetNameOwner", &(name,))?)
}

fn list_mpris_names(bus: &Connection) -> Result<Vec<String>> {
    let names: Vec<String> = dbus_proxy(bus)?.call("ListNames", &())?;
    let mut filtered: Vec<String> = names
        .into_iter()
        .filter(|n| n.starts_with(MPRIS_PREFIX) && n.len() <= 255)
        .collect();
    filtered.sort();
    filtered.truncate(MAX_PLAYERS);
    Ok(filtered)
}

fn get_all_properties(bus: &Connection, owner: &str, interface: &str) -> Result<Properties> {
    Ok(
        Proxy::new(bus, owner, MPRIS_PATH, "org.freedesktop.DBus.Properties")?
            .call("GetAll", &(interface,))?,
    )
}

fn get_position(bus: &Connection, owner: &str) -> Option<i64> {
    let proxy = Proxy::new(bus, owner, MPRIS_PATH, "org.freedesktop.DBus.Properties").ok()?;
    let value: OwnedValue = proxy.call("Get", &(MPRIS_PLAYER_IFACE, "Position")).ok()?;
    value_to_i64(&value).map(|v| v.max(0))
}

fn poll_player(
    bus: &Connection,
    bus_name: String,
    previous: Option<&PlayerState>,
    generation_counter: &mut u64,
    activity_counter: &mut u64,
    art_revision_counter: &mut u64,
) -> Result<PlayerState> {
    let owner = name_owner(bus, &bus_name)?;
    let player_props = get_all_properties(bus, &owner, MPRIS_PLAYER_IFACE)?;
    let root_props = get_all_properties(bus, &owner, MPRIS_ROOT_IFACE).unwrap_or_default();

    let identity = prop_string(&root_props, "Identity", 256);
    let desktop_entry = prop_string(&root_props, "DesktopEntry", 255);
    let status = normalize_status(&prop_string(&player_props, "PlaybackStatus", 32)).to_owned();
    let rate = prop_f64(&player_props, "Rate", 1.0);
    let can_control = prop_bool(&player_props, "CanControl");
    let can_play = can_control && prop_bool(&player_props, "CanPlay");
    let can_pause = can_control && prop_bool(&player_props, "CanPause");
    let can_go_next = can_control && prop_bool(&player_props, "CanGoNext");
    let can_go_previous = can_control && prop_bool(&player_props, "CanGoPrevious");
    let can_seek = can_control && prop_bool(&player_props, "CanSeek");

    let metadata: Properties = player_props
        .get("Metadata")
        .and_then(|v| v.try_clone().ok())
        .and_then(|v| Properties::try_from(v).ok())
        .unwrap_or_default();

    let track_id = metadata_string(&metadata, "mpris:trackid", 512);
    let title = metadata_string(&metadata, "xesam:title", 512);
    let artists = metadata_string_list(&metadata, "xesam:artist", 8, 256);
    let album = metadata_string(&metadata, "xesam:album", 512);
    let album_artists = metadata_string_list(&metadata, "xesam:albumArtist", 8, 256);
    let art_url = metadata_string(&metadata, "mpris:artUrl", 4096);
    let length_us = metadata
        .get("mpris:length")
        .and_then(value_to_i64)
        .filter(|v| *v > 0 && *v <= 86_400_000_000_000)
        .unwrap_or(0);
    let position_us = player_props
        .get("Position")
        .and_then(value_to_i64)
        .map(|v| v.max(0))
        .or_else(|| get_position(bus, &owner));

    let same_owner = previous.filter(|old| old.owner == owner);
    let generation = match same_owner {
        Some(old) => old.generation,
        None => {
            *generation_counter = generation_counter.wrapping_add(1).max(1);
            *generation_counter
        }
    };

    let mut last_activity = match same_owner {
        Some(old) => old.last_activity,
        None => {
            *activity_counter = activity_counter.wrapping_add(1).max(1);
            *activity_counter
        }
    };
    if let Some(old) = same_owner {
        let became_playing = old.status != "Playing" && status == "Playing";
        let track_changed = old.track_id != track_id || old.title != title;
        if became_playing || track_changed {
            *activity_counter = activity_counter.wrapping_add(1).max(1);
            last_activity = *activity_counter;
        }
    }

    let art_key = classify_art_key(&art_url, &desktop_entry);
    let (art_payload, art_revision) = match same_owner {
        Some(old) if old.art_key == art_key => (old.art_payload.clone(), old.art_revision),
        _ => {
            let payload = resolve_art(&art_key, &desktop_entry);
            match (same_owner, payload) {
                (Some(old), payload) if old.art_payload == payload => (payload, old.art_revision),
                (_, Some(payload)) => {
                    *art_revision_counter = art_revision_counter.wrapping_add(1).max(1);
                    (Some(payload), *art_revision_counter)
                }
                (_, None) => (None, 0),
            }
        }
    };

    Ok(PlayerState {
        bus_name,
        owner,
        generation,
        last_activity,
        identity,
        desktop_entry,
        status,
        title,
        artists,
        album,
        album_artists,
        track_id,
        length_us,
        position_us,
        rate,
        can_control,
        can_play,
        can_pause,
        can_go_next,
        can_go_previous,
        can_seek,
        art_key,
        art_payload,
        art_revision,
    })
}

fn execute_control(
    bus: &Connection,
    players: &mut BTreeMap<String, PlayerState>,
    epoch: u64,
    peer: &Arc<Peer>,
    value: &Value,
    lifecycle: (&DesiredPeer, u64),
    activity_counter: &mut u64,
) -> Result<()> {
    let (desired, desired_revision) = lifecycle;
    let check_current = || -> Result<()> {
        let state = desired.lock().unwrap();
        if !peer.desktop_input() || !state.enabled || state.revision != desired_revision {
            bail!("media control superseded by disconnect, disable or read-only policy");
        }
        Ok(())
    };
    check_current()?;
    if value["epoch"].as_u64() != Some(epoch) {
        bail!("stale media control epoch");
    }
    let bus_name = value["player"].as_str().context("missing player")?;
    if !bus_name.starts_with(MPRIS_PREFIX) {
        bail!("invalid MPRIS2 bus name");
    }
    let player = players
        .get_mut(bus_name)
        .context("media player no longer active")?;
    if value["generation"].as_u64() != Some(player.generation) {
        bail!("stale media player generation");
    }
    if name_owner(bus, bus_name)? != player.owner {
        bail!("stale media player owner");
    }
    if !player.can_control {
        bail!("player does not allow remote control");
    }
    check_current()?;
    let proxy = Proxy::new(bus, player.owner.clone(), MPRIS_PATH, MPRIS_PLAYER_IFACE)?;
    match value["action"].as_str().unwrap_or("") {
        "play" => {
            if !player.can_play {
                bail!("player does not allow Play");
            }
            let _: () = proxy.call("Play", &())?;
        }
        "pause" => {
            if !player.can_pause {
                bail!("player does not allow Pause");
            }
            let _: () = proxy.call("Pause", &())?;
        }
        "play_pause" => {
            if !(player.can_play || player.can_pause) {
                bail!("player does not allow PlayPause");
            }
            let _: () = proxy.call("PlayPause", &())?;
        }
        "stop" => {
            let _: () = proxy.call("Stop", &())?;
        }
        "next" => {
            if !player.can_go_next {
                bail!("player does not allow Next");
            }
            let _: () = proxy.call("Next", &())?;
        }
        "previous" => {
            if !player.can_go_previous {
                bail!("player does not allow Previous");
            }
            let _: () = proxy.call("Previous", &())?;
        }
        "set_position" => {
            if !player.can_seek {
                bail!("player does not allow SetPosition");
            }
            let track_id = value["track_id"].as_str().context("missing track_id")?;
            if track_id.is_empty() || track_id != player.track_id {
                bail!("stale track_id for seek");
            }
            let track_path = ObjectPath::try_from(track_id).context("invalid track_id path")?;
            let position_us = value["position_us"]
                .as_i64()
                .context("missing position_us")?;
            if position_us < 0 || (player.length_us > 0 && position_us > player.length_us) {
                bail!("seek position out of range");
            }
            let _: () = proxy.call("SetPosition", &(track_path, position_us))?;
        }
        _ => bail!("unsupported media action"),
    }
    *activity_counter = activity_counter.wrapping_add(1).max(1);
    player.last_activity = *activity_counter;
    Ok(())
}

fn run(address: &str, receiver: &Receiver<Command>, desired: &DesiredPeer) -> Result<()> {
    let bus = connect_bus(address, Duration::from_millis(500))?;
    let mut peer: Option<Arc<Peer>> = None;
    let mut enabled = false;
    let mut desired_revision = 0u64;
    let mut epoch = 0u64;
    let mut generation_counter = 0u64;
    let mut activity_counter = 0u64;
    let mut art_revision_counter = 0u64;
    let mut players = BTreeMap::<String, PlayerState>::new();
    let mut sent_state: Option<SentState> = None;
    let mut sent_art: Option<(String, u64, u64)> = None;
    let mut next_poll = Instant::now();

    loop {
        {
            let state = desired.lock().unwrap();
            if state.revision != desired_revision {
                let previous_peer = peer.clone();
                let was_active = previous_peer.is_some() && enabled;
                desired_revision = state.revision;
                peer = state.peer.clone();
                enabled = state.enabled && peer.is_some();
                epoch = epoch.wrapping_add(1).max(1);
                sent_state = None;
                sent_art = None;
                next_poll = Instant::now();
                if was_active && !enabled {
                    if let Some(p) = peer
                        .as_ref()
                        .or(previous_peer.as_ref())
                        .filter(|p| !p.is_closed())
                    {
                        p.control(json!({"type": "media_reset", "epoch": epoch}));
                    }
                }
                if !enabled {
                    players.clear();
                }
            }
        }

        if !enabled {
            match receiver.recv() {
                Ok(Command::Wake) | Ok(Command::Control(_, _)) => continue,
                Err(_) => return Ok(()),
            }
        }

        let wait = next_poll
            .saturating_duration_since(Instant::now())
            .min(Duration::from_millis(100));
        match receiver.recv_timeout(wait) {
            Ok(Command::Wake) => continue,
            Ok(Command::Control(source, value)) => {
                if peer.as_ref().is_some_and(|p| Arc::ptr_eq(p, &source))
                    && source.desktop_input()
                    && desired.lock().unwrap().revision == desired_revision
                {
                    match execute_control(
                        &bus,
                        &mut players,
                        epoch,
                        &source,
                        &value,
                        (desired, desired_revision),
                        &mut activity_counter,
                    ) {
                        Ok(()) => {
                            // Refresh shortly after player handles the command.
                            next_poll = Instant::now();
                        }
                        Err(error) => {
                            tracing::debug!(
                                action = value["action"].as_str().unwrap_or(""),
                                epoch,
                                %error,
                                "media control rejected"
                            );
                        }
                    }
                }
                continue;
            }
            Err(std::sync::mpsc::RecvTimeoutError::Disconnected) => return Ok(()),
            Err(std::sync::mpsc::RecvTimeoutError::Timeout) => (),
        }

        if Instant::now() < next_poll {
            continue;
        }
        next_poll = Instant::now() + POLL_INTERVAL;

        let names = list_mpris_names(&bus)?;
        let active_names: BTreeSet<String> = names.iter().cloned().collect();
        players.retain(|name, _| active_names.contains(name));

        let deadline = Instant::now() + Duration::from_millis(500);
        for (index, name) in names.into_iter().enumerate() {
            if index > 0
                && (Instant::now() > deadline
                    || desired.lock().unwrap().revision != desired_revision)
            {
                break;
            }
            let previous = players.get(&name);
            match poll_player(
                &bus,
                name.clone(),
                previous,
                &mut generation_counter,
                &mut activity_counter,
                &mut art_revision_counter,
            ) {
                Ok(fresh) => {
                    players.insert(name, fresh);
                }
                Err(_) => {
                    if name_owner(&bus, &name).is_err() {
                        players.remove(&name);
                    }
                }
            }
        }

        let Some(active_peer) = peer.as_ref().filter(|p| !p.is_closed()) else {
            continue;
        };
        if desired.lock().unwrap().revision != desired_revision {
            continue;
        }

        if let Some(selected) = select_active_player(&players) {
            let structural = selected.structural_json(epoch);
            let now = Instant::now();
            let should_send_state = match &sent_state {
                None => true,
                Some(prev) => {
                    prev.structural != structural
                        || position_drifted(prev, now, selected.position_us)
                }
            };
            if should_send_state {
                active_peer.control(selected.full_json(epoch));
                sent_state = Some(SentState {
                    structural,
                    position_us: selected.position_us,
                    sent_at: now,
                    status: selected.status.clone(),
                    rate: selected.rate,
                });
            }
            if let Some(art) = &selected.art_payload {
                let art_version = (
                    selected.bus_name.clone(),
                    selected.generation,
                    selected.art_revision,
                );
                if sent_art.as_ref() != Some(&art_version)
                    && active_peer.media_art(json!({
                        "type": "media_art",
                        "epoch": epoch,
                        "player": selected.bus_name,
                        "generation": selected.generation,
                        "revision": selected.art_revision,
                        "mime": art.mime,
                        "data": art.b64,
                    }))
                {
                    sent_art = Some(art_version);
                }
            } else {
                sent_art = None;
            }
        } else if sent_state.is_some() {
            active_peer.control(json!({"type": "media_reset", "epoch": epoch}));
            sent_state = None;
            sent_art = None;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample_player(bus_name: &str, status: &str, activity: u64, title: &str) -> PlayerState {
        PlayerState {
            bus_name: bus_name.into(),
            owner: ":1.1".into(),
            generation: 1,
            last_activity: activity,
            identity: "Player".into(),
            desktop_entry: String::new(),
            status: status.into(),
            title: title.into(),
            artists: vec![],
            album: String::new(),
            album_artists: vec![],
            track_id: "/track/1".into(),
            length_us: 120_000_000,
            position_us: Some(10_000_000),
            rate: 1.0,
            can_control: true,
            can_play: true,
            can_pause: true,
            can_go_next: true,
            can_go_previous: true,
            can_seek: true,
            art_key: ArtCacheKey::None,
            art_payload: None,
            art_revision: 0,
        }
    }

    #[test]
    fn player_priority_prefers_playing_then_recent_activity() {
        let mut players = BTreeMap::new();
        players.insert(
            "org.mpris.MediaPlayer2.alpha".into(),
            sample_player("org.mpris.MediaPlayer2.alpha", "Paused", 10, "A"),
        );
        players.insert(
            "org.mpris.MediaPlayer2.beta".into(),
            sample_player("org.mpris.MediaPlayer2.beta", "Playing", 5, "B"),
        );
        players.insert(
            "org.mpris.MediaPlayer2.empty".into(),
            sample_player("org.mpris.MediaPlayer2.empty", "Stopped", 99, ""),
        );
        assert_eq!(
            select_active_player(&players).unwrap().bus_name,
            "org.mpris.MediaPlayer2.beta"
        );
        players
            .get_mut("org.mpris.MediaPlayer2.beta")
            .unwrap()
            .status = "Paused".into();
        assert_eq!(
            select_active_player(&players).unwrap().bus_name,
            "org.mpris.MediaPlayer2.alpha"
        );
        players.remove("org.mpris.MediaPlayer2.alpha");
        players.remove("org.mpris.MediaPlayer2.beta");
        assert!(select_active_player(&players).is_none());
    }

    #[test]
    fn file_url_parsing_rejects_remote_hosts_and_nuls() {
        assert_eq!(
            parse_file_url("file:///tmp/my%20cover.jpg"),
            Some(PathBuf::from("/tmp/my cover.jpg"))
        );
        assert_eq!(
            parse_file_url("file://localhost/tmp/art.png?v=1"),
            Some(PathBuf::from("/tmp/art.png"))
        );
        assert_eq!(parse_file_url("file://evil.example/tmp/art.png"), None);
        assert_eq!(parse_file_url("file:///tmp/bad%00.png"), None);
        assert_eq!(parse_file_url("https://example.com/art.png"), None);
    }

    #[test]
    fn art_sniffing_handles_png_jpeg_webp_and_rejects_garbage() {
        let img = image::RgbaImage::from_pixel(320, 160, image::Rgba([255, 0, 0, 255]));
        let mut png = Vec::new();
        PngEncoder::new(&mut png)
            .write_image(img.as_raw(), 320, 160, image::ExtendedColorType::Rgba8)
            .unwrap();
        let encoded = sniff_and_encode_art(&png).unwrap();
        assert_eq!(encoded.mime, "image/png");
        let decoded = image::load_from_memory(&B64.decode(encoded.b64).unwrap()).unwrap();
        assert_eq!((decoded.width(), decoded.height()), (256, 128));

        let fake_jpeg = [
            0xFFu8, 0xD8, 0xFF, 0xE0, 0, 16, b'J', b'F', b'I', b'F', 0, 1,
        ];
        assert_eq!(sniff_and_encode_art(&fake_jpeg).unwrap().mime, "image/jpeg");

        let fake_webp = *b"RIFF\x10\x00\x00\x00WEBPVP8 ";
        assert_eq!(sniff_and_encode_art(&fake_webp).unwrap().mime, "image/webp");

        assert!(sniff_and_encode_art(b"not an image at all").is_none());

        let data_uri = format!("data:image/png;base64,{}", B64.encode(&png));
        assert_eq!(decode_data_art_uri(&data_uri).unwrap().mime, "image/png");
        assert!(decode_data_art_uri("data:text/html;base64,PGh0bWw+").is_none());
    }

    #[test]
    fn position_drift_detects_seeks_without_spamming_steady_playback() {
        let start = Instant::now();
        let sent = SentState {
            structural: json!({}),
            position_us: Some(10_000_000),
            sent_at: start,
            status: "Playing".into(),
            rate: 1.0,
        };
        assert!(!position_drifted(
            &sent,
            start + Duration::from_millis(500),
            Some(10_520_000)
        ));
        assert!(position_drifted(
            &sent,
            start + Duration::from_millis(500),
            Some(25_000_000)
        ));
    }
}
