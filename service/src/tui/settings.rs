use anyhow::{bail, Context, Result};
use serde_json::Value;
use std::{
    fs::{self, OpenOptions},
    io::Read,
    os::{
        fd::AsRawFd,
        unix::fs::{MetadataExt, OpenOptionsExt},
    },
    path::{Path, PathBuf},
};
use toml_edit::{value, Array, DocumentMut, Item, Table};
use waybridge_config::{Loaded, Overrides};

const FILE_LIMIT: usize = 65_536;

/// A draft is based on an exact file snapshot. Cooperating editors serialize
/// saves through a sidecar lock, and external edits are checked before replace.
pub struct FileDraft {
    pub path: PathBuf,
    target: PathBuf,
    pub saved: Option<String>,
}

impl FileDraft {
    pub fn open(path: PathBuf) -> Result<Self> {
        let target = resolve_target(&path)?;
        let saved = read(&target).with_context(|| format!("reading {}", path.display()))?;
        Ok(Self {
            path,
            target,
            saved,
        })
    }

    pub fn save(&mut self, draft: String) -> Result<()> {
        if draft.len() > FILE_LIMIT {
            bail!("Configuration exceeds the edit size limit");
        }
        let parent = self
            .target
            .parent()
            .context("configuration has no parent")?;
        fs::create_dir_all(parent)?;
        let name = self
            .target
            .file_name()
            .context("configuration has no name")?;
        let mut lock_name = name.to_os_string();
        lock_name.push(".waybridge-edit.lock");
        let lock = OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(false)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
            .open(parent.join(lock_name))?;
        owned(&lock.metadata()?)?;
        if unsafe { libc::flock(lock.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 {
            bail!("Configuration is being saved in another editor");
        }
        if resolve_target(&self.path)? != self.target || read(&self.target)? != self.saved {
            bail!("Configuration changed on disk. Reload before saving.");
        }
        crate::storage::atomic_private(&self.target, draft.as_bytes())?;
        self.saved = Some(draft);
        Ok(())
    }
}

fn resolve_target(path: &Path) -> Result<PathBuf> {
    match fs::symlink_metadata(path) {
        Ok(_) => Ok(path.canonicalize()?),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            // Normalize existing parents so an absent file and the same file
            // after creation have the same lock and change detection path.
            let Some(parent) = path.parent().filter(|parent| parent != &path) else {
                return Err(error.into());
            };
            Ok(
                resolve_target(parent)?
                    .join(path.file_name().context("configuration has no name")?),
            )
        }
        Err(error) => Err(error.into()),
    }
}

fn owned(metadata: &fs::Metadata) -> Result<()> {
    if !metadata.is_file() || metadata.uid() != unsafe { libc::getuid() } {
        bail!("Configuration must be an owned regular file to edit it");
    }
    Ok(())
}

fn read(path: &Path) -> Result<Option<String>> {
    let file = match OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
        .open(path)
    {
        Ok(file) => file,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(error) => return Err(error.into()),
    };
    owned(&file.metadata()?)?;
    let mut text = String::new();
    file.take(FILE_LIMIT as u64 + 1).read_to_string(&mut text)?;
    if text.len() > FILE_LIMIT {
        bail!("Configuration exceeds the edit size limit");
    }
    Ok(Some(text))
}

#[derive(Clone, Copy)]
enum Kind {
    Text,
    Number(&'static str),
    Choice(&'static [&'static str], &'static str),
    Codecs,
}

#[derive(Clone, Copy)]
pub struct Field {
    pub section: &'static str,
    pub key: &'static str,
    pub label: &'static str,
    kind: Kind,
}

impl Field {
    pub fn input_hint(self) -> &'static str {
        match self.kind {
            Kind::Text => "Text",
            Kind::Number(unit) => unit,
            Kind::Choice(_, hint) => hint,
            Kind::Codecs => "Comma-separated: png, h264, hevc, av1",
        }
    }

    pub fn path(self) -> String {
        format!("{}.{}", self.section, self.key)
    }

    pub fn value(self, report: &Value) -> String {
        let value = &report["values"][self.section][self.key];
        match value {
            Value::String(value) => value.clone(),
            Value::Null => "—".into(),
            Value::Array(values) => values
                .iter()
                .filter_map(Value::as_str)
                .collect::<Vec<_>>()
                .join(", "),
            other => other.to_string(),
        }
    }

    fn parse(self, input: &str) -> Result<Item> {
        let input = input.trim();
        Ok(match self.kind {
            Kind::Text => value(input),
            Kind::Number(_) => value(i64::from(
                input
                    .parse::<u32>()
                    .context("Enter a whole nonnegative number")?,
            )),
            Kind::Choice(choices, _) => {
                if !choices.contains(&input) {
                    bail!("Choose {}", self.input_hint());
                }
                value(input)
            }
            Kind::Codecs => value(
                input
                    .split(',')
                    .map(str::trim)
                    .filter(|value| !value.is_empty())
                    .collect::<Array>(),
            ),
        })
    }
}

const fn field(section: &'static str, key: &'static str, label: &'static str, kind: Kind) -> Field {
    Field {
        section,
        key,
        label,
        kind,
    }
}

pub const FIELDS: &[Field] = &[
    field("service", "listen", "Service listen address", Kind::Text),
    field("service", "compositor", "Compositor executable", Kind::Text),
    field("service", "python", "Python executable", Kind::Text),
    field("session", "fps", "Default framerate", Kind::Number("FPS")),
    field(
        "session",
        "bitrate",
        "Default video bitrate per window",
        Kind::Number("kbps"),
    ),
    field(
        "session",
        "codec",
        "Default codec",
        Kind::Choice(
            &["auto", "png", "h264-nvenc", "hevc-nvenc", "av1-nvenc"],
            "auto, png, h264-nvenc, hevc-nvenc or av1-nvenc",
        ),
    ),
    field(
        "session",
        "hdr",
        "HDR for new sessions",
        Kind::Choice(&["off", "experimental-scrgb"], "off or experimental-scrgb"),
    ),
    field(
        "session",
        "gpu_input",
        "GPU color input",
        Kind::Choice(&["cpu", "cuda"], "cpu or cuda"),
    ),
    field("session", "layout", "Keyboard layout", Kind::Text),
    field(
        "session",
        "listen",
        "Standalone session listen address",
        Kind::Text,
    ),
    field(
        "session",
        "socket",
        "Standalone session socket name",
        Kind::Text,
    ),
    field("limits", "max_fps", "Framerate limit", Kind::Number("FPS")),
    field(
        "limits",
        "max_bitrate",
        "Video bitrate limit per window",
        Kind::Number("kbps"),
    ),
    field(
        "limits",
        "allowed_codecs",
        "Allowed codec families",
        Kind::Codecs,
    ),
    field(
        "media_budget",
        "cpu_mib",
        "Session CPU media budget",
        Kind::Number("MiB"),
    ),
    field(
        "media_budget",
        "gpu_mib",
        "Session GPU media budget",
        Kind::Number("MiB"),
    ),
    field(
        "media_budget",
        "window_cpu_mib",
        "Window CPU media budget",
        Kind::Number("MiB"),
    ),
    field(
        "media_budget",
        "window_gpu_mib",
        "Window GPU media budget",
        Kind::Number("MiB"),
    ),
    field(
        "media_budget",
        "encoders",
        "Encoder count limit",
        Kind::Number("encoders"),
    ),
];

pub struct Settings {
    pub file: FileDraft,
    document: DocumentMut,
    pub saved: Value,
    pub problem: Option<String>,
    explicit: bool,
}

impl Settings {
    pub fn open(path: PathBuf, explicit: bool) -> Result<Self> {
        let file = FileDraft::open(path)?;
        let parsed = Loaded::parse_document(
            Some(file.path.clone()),
            explicit && file.saved.is_some(),
            file.saved.as_deref(),
        )?;
        let problem = parsed
            .config
            .validate()
            .err()
            .map(|error| error.to_string());
        let saved = parsed.report();
        let document = file
            .saved
            .as_deref()
            .unwrap_or_default()
            .parse::<DocumentMut>()?;
        Ok(Self {
            file,
            document,
            saved,
            problem,
            explicit,
        })
    }

    pub fn dirty(&self) -> bool {
        self.document.to_string() != self.file.saved.as_deref().unwrap_or_default()
    }

    pub fn edited(&self, field: Field) -> Option<String> {
        let item = self.document.get(field.section)?.get(field.key)?;
        Some(match field.kind {
            Kind::Number(_) => item.as_integer()?.to_string(),
            Kind::Codecs => item
                .as_array()?
                .iter()
                .filter_map(toml_edit::Value::as_str)
                .collect::<Vec<_>>()
                .join(", "),
            _ => item.as_str()?.to_string(),
        })
    }

    pub fn set(&mut self, field: Field, input: &str) -> Result<()> {
        let mut entry = field.parse(input)?;
        if self.document.get(field.section).is_none() {
            self.document[field.section] = Item::Table(Table::new());
        }
        let table = &mut self.document[field.section];
        if let (Some(entry), Some(original)) = (
            entry.as_value_mut(),
            table.get(field.key).and_then(Item::as_value),
        ) {
            *entry.decor_mut() = original.decor().clone();
        }
        table[field.key] = entry;
        Ok(())
    }

    pub fn inherit(&mut self, field: Field) {
        if let Some(table) = self
            .document
            .get_mut(field.section)
            .and_then(Item::as_table_like_mut)
        {
            table.remove(field.key);
        }
    }

    pub fn preview(&self, running: &Value) -> Result<(Value, Vec<String>)> {
        let text = self.document.to_string();
        let mut loaded =
            Loaded::from_document(Some(self.file.path.clone()), self.explicit, Some(&text))?;
        let report = loaded.report();
        let cli_value = |field: &str| {
            (running["sources"][format!("service.{field}")]["kind"] == "cli")
                .then(|| {
                    running["values"]["service"][field]
                        .as_str()
                        .map(str::to_owned)
                })
                .flatten()
        };
        loaded.apply(Overrides {
            service_listen: cli_value("listen"),
            compositor: cli_value("compositor"),
            python: cli_value("python"),
            ..Default::default()
        })?;
        let next = loaded.report();
        let changes = FIELDS
            .iter()
            .filter_map(|field| {
                let old = field.value(&self.saved);
                let new = field.value(&report);
                let path = field.path();
                if old == new
                    && self.saved["sources"][&path]["kind"] == report["sources"][&path]["kind"]
                {
                    return None;
                }
                let timing = match (next["sources"][&path]["kind"].as_str(), field.section) {
                    (Some("cli"), _) => "startup option overrides this file",
                    (_, "service") => "after service restart",
                    (_, "session") if ["listen", "socket"].contains(&field.key) => {
                        "standalone launches"
                    }
                    _ => "new sessions",
                };
                Some(format!("{}: {old} → {new} · {timing}", field.label))
            })
            .collect();
        Ok((report, changes))
    }

    pub fn save(&mut self, running: &Value) -> Result<()> {
        let (report, _) = self.preview(running)?;
        self.file.save(self.document.to_string())?;
        self.saved = report;
        self.problem = None;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn edits_preserve_comments_and_detect_outside_writes() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("config.toml");
        fs::write(&path, "# Notes\n[session]\nfps = 60 # Per window\n").unwrap();
        let mut settings = Settings::open(path.clone(), false).unwrap();
        settings.set(FIELDS[3], "45").unwrap();
        settings.save(&Value::Null).unwrap();
        assert!(fs::read_to_string(&path)
            .unwrap()
            .contains("fps = 45 # Per window"));
        settings.set(FIELDS[3], "50").unwrap();
        fs::write(&path, "# changed elsewhere\n").unwrap();
        assert!(settings.save(&Value::Null).is_err());
        assert_eq!(fs::read_to_string(&path).unwrap(), "# changed elsewhere\n");
    }

    #[test]
    fn missing_config_is_created_and_full_candidate_is_validated() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("new/config.toml");
        let mut settings = Settings::open(path.clone(), true).unwrap();
        settings.set(FIELDS[11], "30").unwrap();
        assert!(settings.save(&Value::Null).is_err());
        assert!(!path.exists());
        settings.set(FIELDS[3], "30").unwrap();
        settings.save(&Value::Null).unwrap();
        assert_eq!(Loaded::load(Some(&path)).unwrap().config.session.fps, 30);
    }

    #[test]
    fn invalid_saved_values_can_be_repaired() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("config.toml");
        fs::write(&path, "[session]\nfps = 121\n").unwrap();
        let mut settings = Settings::open(path.clone(), true).unwrap();
        assert!(settings.problem.is_some());
        assert_eq!(settings.saved["valid"], false);
        assert_eq!(settings.edited(FIELDS[3]).as_deref(), Some("121"));
        settings.set(FIELDS[3], "60").unwrap();
        settings.save(&Value::Null).unwrap();
        assert_eq!(Loaded::load(Some(&path)).unwrap().config.session.fps, 60);
    }
}
