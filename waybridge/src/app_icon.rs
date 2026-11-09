//! Bounded background desktop/icon-theme lookup. No filesystem or SVG work on the compositor.
use crate::network::Event;
use base64::{engine::general_purpose::STANDARD as B64, Engine};
use image::{codecs::png::PngEncoder, ImageEncoder, RgbaImage};
use serde_json::{json, Value};
use std::{
    collections::{BTreeMap, HashSet, VecDeque},
    fs,
    io::{Read, Seek, SeekFrom},
    os::unix::process::CommandExt,
    path::{Component, Path, PathBuf},
    process::{Command, Stdio},
    sync::{mpsc::SyncSender, Arc, Condvar, Mutex},
    thread,
    time::{Duration, Instant},
};

pub const EDGE: u32 = 128;
pub const MAX_PNG: usize = 96 * 1024;
const MAX_INPUT: u64 = 2 * 1024 * 1024;
type Icon = Option<Arc<Vec<u8>>>;
type Ini = BTreeMap<String, BTreeMap<String, String>>;

#[derive(Default)]
pub struct WindowIcon {
    pub app_id: String,
    pub revision: u64,
    pub requested: bool,
    pub ready: bool,
    pub sent: bool,
    pub png: Icon,
}
impl WindowIcon {
    pub fn observe(&mut self, app_id: &str) {
        let app_id = if identifier(app_id) { app_id } else { "" };
        if self.revision == 0 || self.app_id != app_id {
            self.app_id = app_id.to_owned();
            self.revision += 1;
            self.requested = false;
            self.ready = app_id.is_empty();
            self.sent = false;
            self.png = None;
        }
    }
    pub fn complete(&mut self, revision: u64, app_id: &str, png: Icon) {
        if self.revision == revision && self.app_id == app_id {
            self.png = png;
            self.ready = true;
            self.sent = false;
        }
    }
    pub fn message(&self, id: u32) -> Value {
        json!({"type":"window_icon","id":id,"app_id":self.app_id,"revision":self.revision,
            "width":EDGE,"height":EDGE,"png":self.png.as_ref().map(|png| B64.encode(png.as_ref()))})
    }
}

struct Request {
    id: u32,
    revision: u64,
    app_id: String,
}
#[derive(Default)]
struct Jobs {
    requests: BTreeMap<u32, Request>,
    last_id: u32,
}
pub struct Icons {
    queue: Arc<(Mutex<Jobs>, Condvar)>,
}
impl Icons {
    pub fn new(events: SyncSender<Event>) -> Self {
        let queue = Arc::new((Mutex::new(Jobs::default()), Condvar::new()));
        let worker = queue.clone();
        thread::spawn(move || {
            let resolver = Resolver::environment();
            let mut cache = VecDeque::<(String, Instant, Icon)>::new();
            loop {
                let request = {
                    let (lock, wake) = &*worker;
                    let mut jobs = lock.lock().unwrap();
                    while jobs.requests.is_empty() {
                        jobs = wake.wait(jobs).unwrap();
                    }
                    let id = *jobs
                        .requests
                        .range((
                            std::ops::Bound::Excluded(jobs.last_id),
                            std::ops::Bound::Unbounded,
                        ))
                        .next()
                        .or_else(|| jobs.requests.first_key_value())
                        .unwrap()
                        .0;
                    jobs.last_id = id;
                    jobs.requests.remove(&id).unwrap()
                };
                let cached = cache.iter().position(|(key, time, _)| {
                    key == &request.app_id && time.elapsed() < Duration::from_secs(60)
                });
                let png = if let Some(index) = cached {
                    let entry = cache.remove(index).unwrap();
                    let png = entry.2.clone();
                    cache.push_back(entry);
                    png
                } else {
                    let png = resolver
                        .resolve(&request.app_id)
                        .and_then(|path| load(&path))
                        .map(Arc::new);
                    cache.retain(|(key, _, _)| key != &request.app_id);
                    cache.push_back((request.app_id.clone(), Instant::now(), png.clone()));
                    while cache.len() > 64 {
                        cache.pop_front();
                    }
                    png
                };
                if events
                    .send(Event::Icon {
                        id: request.id,
                        revision: request.revision,
                        app_id: request.app_id,
                        png,
                    })
                    .is_err()
                {
                    return;
                }
            }
        });
        Self { queue }
    }
    pub fn request(&self, id: u32, revision: u64, app_id: &str) -> bool {
        let (lock, wake) = &*self.queue;
        let mut jobs = lock.lock().unwrap();
        if jobs.requests.len() >= 128 && !jobs.requests.contains_key(&id) {
            return false;
        }
        jobs.requests.insert(
            id,
            Request {
                id,
                revision,
                app_id: app_id.to_owned(),
            },
        );
        wake.notify_one();
        true
    }
    pub fn forget(&self, id: u32) {
        self.queue.0.lock().unwrap().requests.remove(&id);
    }
}

fn identifier(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 255
        && s != "."
        && s != ".."
        && s.bytes()
            .all(|c| c.is_ascii_alphanumeric() || b"._-".contains(&c))
}
fn relative(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 255
        && Path::new(s)
            .components()
            .all(|p| matches!(p, Component::Normal(_)))
}
fn ini(path: &Path) -> Option<Ini> {
    if !fs::metadata(path).ok()?.is_file() {
        return None;
    }
    let mut bytes = Vec::new();
    fs::File::open(path)
        .ok()?
        .take(256 * 1024 + 1)
        .read_to_end(&mut bytes)
        .ok()?;
    if bytes.len() > 256 * 1024 {
        return None;
    }
    let text = std::str::from_utf8(&bytes).ok()?;
    let mut result = Ini::new();
    let mut section = String::new();
    for line in text.lines() {
        let line = line.trim();
        if line.starts_with('#') || line.is_empty() {
            continue;
        }
        if line.starts_with('[') && line.ends_with(']') {
            section = line[1..line.len() - 1].to_owned();
        } else if let Some((key, value)) = line.split_once('=') {
            result
                .entry(section.clone())
                .or_default()
                .insert(key.trim().to_owned(), value.trim().to_owned());
        }
    }
    Some(result)
}
fn unescape(value: &str) -> String {
    let mut out = String::new();
    let mut chars = value.chars();
    while let Some(c) = chars.next() {
        if c == '\\' {
            out.push(match chars.next() {
                Some('s') => ' ',
                Some('n') => '\n',
                Some('t') => '\t',
                Some('r') => '\r',
                Some(c) => c,
                None => '\\',
            });
        } else {
            out.push(c);
        }
    }
    out
}

struct Resolver {
    data: Vec<PathBuf>,
    roots: Vec<PathBuf>,
    theme: String,
}
impl Resolver {
    fn environment() -> Self {
        let home = std::env::var_os("HOME")
            .map(PathBuf::from)
            .unwrap_or_default();
        let data_home = std::env::var_os("XDG_DATA_HOME")
            .map(PathBuf::from)
            .filter(|p| p.is_absolute())
            .unwrap_or_else(|| home.join(".local/share"));
        let mut data = vec![data_home];
        data.extend(
            std::env::split_paths(
                &std::env::var_os("XDG_DATA_DIRS")
                    .unwrap_or_else(|| "/usr/local/share:/usr/share".into()),
            )
            .filter(|p| p.is_absolute())
            .take(31),
        );
        let mut roots = vec![home.join(".icons")];
        roots.extend(data.iter().map(|p| p.join("icons")));
        let config = std::env::var_os("XDG_CONFIG_HOME")
            .map(PathBuf::from)
            .filter(|p| p.is_absolute())
            .unwrap_or_else(|| home.join(".config"));
        let kde =
            || ini(&config.join("kdeglobals")).and_then(|i| i.get("Icons")?.get("Theme").cloned());
        let gtk = || {
            ini(&config.join("gtk-3.0/settings.ini"))
                .and_then(|i| i.get("Settings")?.get("gtk-icon-theme-name").cloned())
        };
        let selected = if std::env::var("XDG_CURRENT_DESKTOP")
            .unwrap_or_default()
            .contains("KDE")
        {
            kde().or_else(gtk)
        } else {
            gtk().or_else(kde)
        };
        let theme = std::env::var("WAYBRIDGE_ICON_THEME")
            .ok()
            .or(selected)
            .filter(|s| identifier(s))
            .unwrap_or_else(|| "Adwaita".into());
        Self { data, roots, theme }
    }
    fn desktop(&self, app_id: &str) -> Option<PathBuf> {
        if !identifier(app_id) {
            return None;
        }
        let filename = format!(
            "{}.desktop",
            app_id.strip_suffix(".desktop").unwrap_or(app_id)
        );
        for data in &self.data {
            let base = data.join("applications");
            let direct = base.join(&filename);
            if direct.is_file() {
                return Some(direct);
            }
            // Desktop-file IDs replace nested directory separators with '-'.
            let mut pending = vec![(base.clone(), 0usize)];
            let mut budget = 4096;
            while let Some((directory, depth)) = pending.pop() {
                let Ok(entries) = fs::read_dir(directory) else {
                    continue;
                };
                for entry in entries.flatten() {
                    if budget == 0 {
                        break;
                    }
                    budget -= 1;
                    let path = entry.path();
                    let Ok(kind) = entry.file_type() else {
                        continue;
                    };
                    if kind.is_dir() && depth < 8 {
                        pending.push((path, depth + 1));
                    } else if kind.is_file()
                        && path
                            .strip_prefix(&base)
                            .ok()?
                            .to_string_lossy()
                            .replace('/', "-")
                            == filename
                    {
                        return Some(path);
                    }
                }
                if budget == 0 {
                    break;
                }
            }
        }
        None
    }
    fn resolve(&self, app_id: &str) -> Option<PathBuf> {
        let entry = ini(&self.desktop(app_id)?)?;
        let entry = entry.get("Desktop Entry")?;
        if entry.get("Hidden").is_some_and(|s| s == "true") {
            return None;
        }
        let name = unescape(entry.get("Icon")?);
        if name.len() > 4096 || name.chars().any(char::is_control) {
            return None;
        }
        if Path::new(&name).is_absolute() {
            return regular_image(PathBuf::from(name));
        }
        if !relative(&name) || name.contains('/') || name.contains('\\') {
            return None;
        }
        let mut seen = HashSet::new();
        if let Some(path) = self.theme_icon(&self.theme, &name, &mut seen) {
            return Some(path);
        }
        if let Some(path) = self.theme_icon("hicolor", &name, &mut seen) {
            return Some(path);
        }
        for root in &self.roots {
            if let Some(path) = candidate(root, &name) {
                return Some(path);
            }
        }
        for data in &self.data {
            if let Some(path) = candidate(&data.join("pixmaps"), &name) {
                return Some(path);
            }
        }
        None
    }
    fn theme_icon(&self, theme: &str, name: &str, seen: &mut HashSet<String>) -> Option<PathBuf> {
        if !identifier(theme) || seen.len() >= 16 || !seen.insert(theme.to_owned()) {
            return None;
        }
        let index = self
            .roots
            .iter()
            .find_map(|p| ini(&p.join(theme).join("index.theme")))?;
        let theme_section = index.get("Icon Theme")?;
        let mut dirs = theme_section
            .get("Directories")
            .into_iter()
            .chain(theme_section.get("ScaledDirectories"))
            .flat_map(|s| s.split(','))
            .map(str::trim)
            .filter(|s| relative(s))
            .take(2048)
            .filter_map(|s| index.get(s).map(|section| (distance(section), s)))
            .collect::<Vec<_>>();
        dirs.sort_by_key(|(distance, _)| *distance);
        for (_, directory) in dirs {
            for root in &self.roots {
                if let Some(path) = candidate(&root.join(theme).join(directory), name) {
                    return Some(path);
                }
            }
        }
        for inherit in theme_section
            .get("Inherits")
            .into_iter()
            .flat_map(|s| s.split(','))
            .map(str::trim)
        {
            if let Some(path) = self.theme_icon(inherit, name, seen) {
                return Some(path);
            }
        }
        None
    }
}
fn distance(section: &BTreeMap<String, String>) -> u32 {
    let number = |key: &str, fallback: u32| {
        section
            .get(key)
            .and_then(|s| s.parse::<u32>().ok())
            .unwrap_or(fallback)
            .min(4096)
    };
    let size = number("Size", 48);
    let scale = number("Scale", 1).clamp(1, 4);
    let (min, max) = match section
        .get("Type")
        .map(String::as_str)
        .unwrap_or("Threshold")
    {
        "Scalable" => (number("MinSize", size), number("MaxSize", size)),
        "Fixed" => (size, size),
        _ => (
            size.saturating_sub(number("Threshold", 2)),
            size.saturating_add(number("Threshold", 2)),
        ),
    };
    EDGE.saturating_sub(max * scale)
        .max((min * scale).saturating_sub(EDGE))
}
fn regular_image(path: PathBuf) -> Option<PathBuf> {
    let metadata = fs::metadata(&path).ok()?;
    (metadata.is_file()
        && metadata.len() <= MAX_INPUT
        && matches!(path.extension()?.to_str()?, "png" | "svg"))
    .then_some(path)
}
fn candidate(directory: &Path, name: &str) -> Option<PathBuf> {
    if name.ends_with(".png") || name.ends_with(".svg") {
        return regular_image(directory.join(name));
    }
    ["png", "svg"]
        .iter()
        .find_map(|ext| regular_image(directory.join(format!("{name}.{ext}"))))
}
fn svg(path: &Path) -> Option<Vec<u8>> {
    let mut file = tempfile::tempfile().ok()?;
    let mut command = Command::new("rsvg-convert");
    command
        .args([
            "--format=png",
            "--width=128",
            "--height=128",
            "--keep-aspect-ratio",
        ])
        .arg(path)
        .stdin(Stdio::null())
        .stdout(Stdio::from(file.try_clone().ok()?))
        .stderr(Stdio::null());
    // The converter is optional and isolated from the compositor's memory/CPU.
    unsafe {
        command.pre_exec(|| {
            if libc::prctl(libc::PR_SET_PDEATHSIG, libc::SIGKILL) != 0 {
                return Err(std::io::Error::last_os_error());
            }
            for (resource, amount) in [
                (libc::RLIMIT_AS, 512 * 1024 * 1024),
                (libc::RLIMIT_CPU, 2),
                (libc::RLIMIT_FSIZE, MAX_PNG as u64),
            ] {
                let limit = libc::rlimit {
                    rlim_cur: amount as libc::rlim_t,
                    rlim_max: amount as libc::rlim_t,
                };
                if libc::setrlimit(resource, &limit) != 0 {
                    return Err(std::io::Error::last_os_error());
                }
            }
            Ok(())
        });
    }
    let mut child = command.spawn().ok()?;
    let started = Instant::now();
    loop {
        match child.try_wait() {
            Ok(Some(status)) if status.success() => break,
            Ok(Some(_)) => return None,
            Err(_) => {
                let _ = child.kill();
                let _ = child.wait();
                return None;
            }
            Ok(None) if started.elapsed() >= Duration::from_secs(2) => {
                let _ = child.kill();
                let _ = child.wait();
                return None;
            }
            Ok(None) => thread::sleep(Duration::from_millis(5)),
        }
    }
    file.seek(SeekFrom::Start(0)).ok()?;
    let mut bytes = Vec::new();
    file.take(MAX_PNG as u64 + 1).read_to_end(&mut bytes).ok()?;
    (bytes.len() <= MAX_PNG).then_some(bytes)
}
fn load(path: &Path) -> Option<Vec<u8>> {
    regular_image(path.to_owned())?;
    let bytes = if path.extension()? == "svg" {
        svg(path)?
    } else {
        let mut bytes = Vec::new();
        fs::File::open(path)
            .ok()?
            .take(MAX_INPUT + 1)
            .read_to_end(&mut bytes)
            .ok()?;
        if bytes.len() as u64 > MAX_INPUT {
            return None;
        }
        bytes
    };
    let mut reader =
        image::ImageReader::with_format(std::io::Cursor::new(bytes), image::ImageFormat::Png);
    let mut limits = image::Limits::default();
    limits.max_image_width = Some(1024);
    limits.max_image_height = Some(1024);
    limits.max_alloc = Some(16 * 1024 * 1024);
    reader.limits(limits);
    let image = reader.decode().ok()?;
    if image.width() == 0 || image.height() == 0 {
        return None;
    }
    let resized = image
        .resize(EDGE, EDGE, image::imageops::FilterType::Lanczos3)
        .to_rgba8();
    let mut canvas = RgbaImage::new(EDGE, EDGE);
    image::imageops::overlay(
        &mut canvas,
        &resized,
        i64::from((EDGE - resized.width()) / 2),
        i64::from((EDGE - resized.height()) / 2),
    );
    let mut png = Vec::new();
    PngEncoder::new(&mut png)
        .write_image(canvas.as_raw(), EDGE, EDGE, image::ExtendedColorType::Rgba8)
        .ok()?;
    (png.len() <= MAX_PNG).then_some(png)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn reject_stale_response_and_bound_identifiers() {
        let mut icon = WindowIcon::default();
        icon.observe("app.one");
        let old = icon.revision;
        icon.observe("app.two");
        icon.complete(old, "app.one", Some(Arc::new(vec![1])));
        assert!(!icon.ready && icon.png.is_none());
        for bad in ["../app", "/app", "..", "a/b", "a\\b"] {
            assert!(!identifier(bad));
        }
        assert!(!relative("../../icons"));
        assert!(identifier("org.example.App.desktop"));
    }
}
