//! One configuration contract for the compositor and authenticated entry point.
//! No credential, environment dump, or runtime session identity belongs here.
use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    collections::BTreeMap,
    ffi::OsStr,
    fs,
    path::{Path, PathBuf},
};

#[derive(Clone, Debug, Default, Deserialize, Serialize)]
#[serde(default, deny_unknown_fields)]
pub struct Config {
    pub service: Service,
    pub session: Session,
    pub limits: Limits,
    pub media_budget: MediaBudget,
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(default, deny_unknown_fields)]
pub struct Service {
    pub listen: String,
    pub compositor: String,
    pub python: String,
}

impl Default for Service {
    fn default() -> Self {
        Self {
            listen: "0.0.0.0:49092".into(),
            compositor: "waybridge".into(),
            python: "python3".into(),
        }
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(default, deny_unknown_fields)]
pub struct Session {
    pub listen: String,
    pub socket: String,
    pub fps: u32,
    /// Kilobits per second, per video stream.
    pub bitrate: u32,
    pub codec: String,
    pub gpu_input: String,
    pub hdr: String,
    pub layout: String,
}

impl Default for Session {
    fn default() -> Self {
        Self {
            listen: "127.0.0.1:49090".into(),
            socket: "waybridge-0".into(),
            fps: 60,
            bitrate: 30_000,
            codec: "auto".into(),
            gpu_input: "cpu".into(),
            hdr: "off".into(),
            layout: "us".into(),
        }
    }
}

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(default, deny_unknown_fields)]
pub struct Limits {
    pub max_fps: u32,
    pub max_bitrate: u32,
    /// Codec families after resolving auto, not encoder implementation names.
    pub allowed_codecs: Vec<String>,
}

impl Default for Limits {
    fn default() -> Self {
        Self {
            max_fps: 120,
            max_bitrate: 200_000,
            allowed_codecs: ["png", "h264", "hevc", "av1"].map(str::to_owned).to_vec(),
        }
    }
}

/// Per-session resource admission; GPU values are allocation estimates, not driver VRAM readings.
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(default, deny_unknown_fields)]
pub struct MediaBudget {
    pub cpu_mib: u32,
    pub gpu_mib: u32,
    pub window_cpu_mib: u32,
    pub window_gpu_mib: u32,
    pub encoders: u32,
}
impl Default for MediaBudget {
    fn default() -> Self {
        Self {
            cpu_mib: 512,
            gpu_mib: 1024,
            window_cpu_mib: 384,
            window_gpu_mib: 512,
            encoders: 8,
        }
    }
}

/// Map both startup and wire codec names to the same family before checking policy.
pub fn codec_family(codec: &str, hdr: bool) -> Result<&'static str> {
    Ok(match codec {
        "auto" if hdr => "hevc",
        "auto" | "h264" | "h264-nvenc" => "h264",
        "png" => "png",
        "hevc" | "hevc-nvenc" => "hevc",
        "av1" | "av1-nvenc" => "av1",
        _ => bail!("unsupported codec (expected auto, png, h264, hevc or av1)"),
    })
}

impl Limits {
    pub fn validate_stream(&self, fps: u64, bitrate: u64, codec: &str, hdr: bool) -> Result<()> {
        if !(1..=u64::from(self.max_fps)).contains(&fps) {
            bail!("fps must be between 1 and {} (server limit)", self.max_fps);
        }
        if !(1000..=u64::from(self.max_bitrate)).contains(&bitrate) {
            bail!(
                "bitrate must be between 1000 and {} kbps (server limit)",
                self.max_bitrate
            );
        }
        let family = codec_family(codec, hdr)?;
        if !self.allowed_codecs.iter().any(|allowed| allowed == family) {
            bail!("codec {family} is not allowed by the server");
        }
        Ok(())
    }
}

impl Config {
    pub fn validate(&self) -> Result<()> {
        let media = &self.media_budget;
        for (name, value) in [
            ("cpu_mib", media.cpu_mib),
            ("gpu_mib", media.gpu_mib),
            ("window_cpu_mib", media.window_cpu_mib),
            ("window_gpu_mib", media.window_gpu_mib),
        ] {
            if !(4..=32768).contains(&value) {
                bail!("media_budget.{name} must be between 4 and 32768 MiB");
            }
        }
        if media.window_cpu_mib > media.cpu_mib || media.window_gpu_mib > media.gpu_mib {
            bail!("media_budget window limits cannot exceed session limits");
        }
        if !(1..=64).contains(&media.encoders) {
            bail!("media_budget.encoders must be between 1 and 64");
        }
        for (name, value) in [
            ("service.listen", &self.service.listen),
            ("session.listen", &self.session.listen),
        ] {
            // Do not resolve DNS or bind while checking a file. Both existing
            // listeners also accept host names through ToSocketAddrs.
            if value.parse::<std::net::SocketAddr>().is_err()
                && !value.rsplit_once(':').is_some_and(|(host, port)| {
                    !host.is_empty()
                        && !host
                            .chars()
                            .any(|c| c.is_whitespace() || ":/[]\0".contains(c))
                        && port.parse::<u16>().is_ok()
                })
            {
                bail!("{name} must be an IP address or host name and port");
            }
        }
        for (name, value) in [
            ("service.compositor", &self.service.compositor),
            ("service.python", &self.service.python),
            ("session.layout", &self.session.layout),
        ] {
            if value.is_empty() || value.contains('\0') {
                bail!("{name} must be nonempty and contain no NUL");
            }
        }
        let socket = Path::new(&self.session.socket);
        if self.session.socket.is_empty()
            || self.session.socket.contains('\0')
            || socket.file_name().is_none()
            || (!socket.is_absolute() && self.session.socket.contains('/'))
        {
            bail!("session.socket must be a socket name or an absolute path with a basename");
        }
        if !["cpu", "cuda"].contains(&self.session.gpu_input.as_str()) {
            bail!("session.gpu_input must be cpu or cuda");
        }
        if !["off", "experimental-scrgb"].contains(&self.session.hdr.as_str()) {
            bail!("session.hdr must be off or experimental-scrgb");
        }
        if !(1..=120).contains(&self.limits.max_fps) {
            bail!("limits.max_fps must be between 1 and 120");
        }
        if !(1000..=200_000).contains(&self.limits.max_bitrate) {
            bail!("limits.max_bitrate must be between 1000 and 200000");
        }
        if self.limits.allowed_codecs.is_empty()
            || self
                .limits
                .allowed_codecs
                .iter()
                .any(|codec| !["png", "h264", "hevc", "av1"].contains(&codec.as_str()))
        {
            bail!("limits.allowed_codecs must contain one or more of png, h264, hevc, av1");
        }
        self.limits
            .validate_stream(
                self.session.fps.into(),
                self.session.bitrate.into(),
                &self.session.codec,
                self.session.hdr == "experimental-scrgb",
            )
            .context("invalid session defaults")
    }
}

#[derive(Default)]
pub struct Overrides {
    pub service_listen: Option<String>,
    pub compositor: Option<String>,
    pub python: Option<String>,
    pub listen: Option<String>,
    pub socket: Option<String>,
    pub fps: Option<u32>,
    pub bitrate: Option<u32>,
    pub codec: Option<String>,
    pub gpu_input: Option<String>,
    pub hdr: Option<String>,
    pub layout: Option<String>,
}

#[derive(Clone, Debug)]
pub struct Loaded {
    pub config: Config,
    pub path: Option<PathBuf>,
    pub explicit: bool,
    pub file_loaded: bool,
    pub sources: BTreeMap<String, Value>,
}

/// Empty or relative XDG values do not replace the standard HOME fallback.
pub fn user_config_path(xdg: Option<&OsStr>, home: Option<&OsStr>) -> Option<PathBuf> {
    xdg.map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .or_else(|| {
            home.map(PathBuf::from)
                .filter(|p| p.is_absolute())
                .map(|p| p.join(".config"))
        })
        .map(|p| p.join("waybridge/config.toml"))
}

impl Loaded {
    pub fn selected_path(explicit: Option<&Path>) -> Option<PathBuf> {
        explicit.map(Path::to_path_buf).or_else(|| {
            user_config_path(
                std::env::var_os("XDG_CONFIG_HOME").as_deref(),
                std::env::var_os("HOME").as_deref(),
            )
        })
    }

    pub fn load(explicit: Option<&Path>) -> Result<Self> {
        let path = Self::selected_path(explicit);
        Self::load_path(path.as_deref(), explicit.is_some())
    }

    pub fn load_path(path: Option<&Path>, explicit: bool) -> Result<Self> {
        let path = path
            .map(|p| {
                if p.is_absolute() {
                    Ok(p.to_path_buf())
                } else {
                    std::env::current_dir().map(|cwd| cwd.join(p))
                }
            })
            .transpose()
            .context("resolving configuration path")?;
        let document = match &path {
            Some(path) => match fs::read_to_string(path) {
                Ok(text) => Some(text),
                Err(error) if error.kind() == std::io::ErrorKind::NotFound && !explicit => None,
                Err(error) => {
                    return Err(error)
                        .with_context(|| format!("reading configuration {}", path.display()))
                }
            },
            None if explicit => bail!("explicit configuration path is missing"),
            None => None,
        };
        Self::from_document(path, explicit, document.as_deref())
    }

    /// Parse a proposed document using the same defaults, validation and diagnostics as startup.
    pub fn from_document(
        path: Option<PathBuf>,
        explicit: bool,
        text: Option<&str>,
    ) -> Result<Self> {
        let parsed = Self::parse_document(path, explicit, text)?;
        parsed
            .config
            .validate()
            .context("validating configuration")?;
        Ok(parsed)
    }

    /// Read configuration fields for a draft editor, including values that
    /// fail semantic validation. Use from_document before applying them.
    pub fn parse_document(
        path: Option<PathBuf>,
        explicit: bool,
        text: Option<&str>,
    ) -> Result<Self> {
        let mut sources = BTreeMap::new();
        let mut config = Config::default();
        collect_sources(
            "",
            &serde_json::to_value(&config)?,
            &json!({"kind":"default"}),
            &mut sources,
        );
        if let Some(text) = text {
            let path = path
                .as_deref()
                .context("configuration document has no path")?;
            // TOML diagnostics may include values, so report only the location and error kind.
            config = toml::from_str(text).map_err(|error| safe_parse_error(text, path, error))?;
            let document: toml::Value =
                toml::from_str(text).map_err(|error| safe_parse_error(text, path, error))?;
            collect_sources(
                "",
                &serde_json::to_value(document)?,
                &json!({"kind":"file","path":path}),
                &mut sources,
            );
        } else if explicit {
            bail!("explicit configuration document is missing");
        }
        Ok(Self {
            config,
            path,
            explicit,
            file_loaded: text.is_some(),
            sources,
        })
    }

    pub fn apply(&mut self, overrides: Overrides) -> Result<()> {
        macro_rules! apply {
            ($field:ident, $section:ident, $name:ident) => {
                if let Some(value) = overrides.$field {
                    self.config.$section.$name = value;
                    self.sources.insert(concat!(stringify!($section), ".", stringify!($name)).into(), json!({"kind":"cli"}));
                }
            }
        }
        apply!(service_listen, service, listen);
        apply!(compositor, service, compositor);
        apply!(python, service, python);
        apply!(listen, session, listen);
        apply!(socket, session, socket);
        apply!(fps, session, fps);
        apply!(bitrate, session, bitrate);
        apply!(codec, session, codec);
        apply!(gpu_input, session, gpu_input);
        apply!(hdr, session, hdr);
        apply!(layout, session, layout);
        self.config.validate().context("validating startup options")
    }

    pub fn report(&self) -> Value {
        let apply: BTreeMap<_, _> = self.sources.keys().map(|key| {
            let phase = if key.starts_with("service.") { "service_restart" } else { "new_session" };
            (key.clone(), json!({"phase":phase,"runtime_request":matches!(key.as_str(), "session.fps" | "session.bitrate" | "session.codec" | "session.hdr")}))
        }).collect();
        json!({"valid":self.config.validate().is_ok(),"config_file":{"path":self.path,"explicit":self.explicit,"loaded":self.file_loaded},
            "values":self.config,"sources":self.sources,"apply":apply,
            "notes":["allowed_codecs limits codec requests after resolving auto; PNG safety fallback remains available when session video is disabled",
                "service-managed sessions allocate their own loopback listener and socket name",
                "runtime media requests do not modify this file or its defaults"]})
    }
}

fn safe_parse_error(text: &str, path: &Path, error: toml::de::Error) -> anyhow::Error {
    let offset = error
        .span()
        .map(|span| span.start)
        .unwrap_or(0)
        .min(text.len());
    let line = text.as_bytes()[..offset]
        .iter()
        .filter(|&&byte| byte == b'\n')
        .count()
        + 1;
    let unknown = error
        .message()
        .strip_prefix("unknown field `")
        .and_then(|rest| rest.split_once('`'))
        .map(|(field, _)| field)
        .filter(|field| {
            field
                .bytes()
                .all(|byte| byte.is_ascii_alphanumeric() || b"_-".contains(&byte))
        });
    let reason = unknown
        .map(|field| format!("unknown field {field:?}"))
        .unwrap_or_else(|| "invalid TOML syntax or field type".into());
    anyhow::anyhow!(
        "invalid configuration {} at line {line}: {reason}",
        path.display()
    )
}

fn collect_sources(
    prefix: &str,
    value: &Value,
    source: &Value,
    output: &mut BTreeMap<String, Value>,
) {
    if let Value::Object(map) = value {
        for (key, value) in map {
            let name = if prefix.is_empty() {
                key.clone()
            } else {
                format!("{prefix}.{key}")
            };
            collect_sources(&name, value, source, output);
        }
    } else {
        output.insert(prefix.into(), source.clone());
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn config(text: &str) -> (tempfile::TempDir, PathBuf) {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("config.toml");
        fs::write(&path, text).unwrap();
        (dir, path)
    }
    #[test]
    fn default_file_and_explicit_cli_provenance() {
        let (_dir, path) =
            config("[session]\nfps = 45\ncodec = 'png'\n[service]\nlisten = '127.0.0.1:49999'\n");
        let mut loaded = Loaded::load_path(Some(&path), true).unwrap();
        assert_eq!(loaded.config.session.fps, 45);
        assert_eq!(loaded.sources["session.fps"]["kind"], "file");
        assert_eq!(loaded.sources["session.bitrate"]["kind"], "default");
        loaded
            .apply(Overrides {
                fps: Some(50),
                ..Default::default()
            })
            .unwrap();
        assert_eq!(loaded.config.session.fps, 50);
        assert_eq!(loaded.sources["session.fps"]["kind"], "cli");
        assert_eq!(loaded.config.session.codec, "png");
        assert_eq!(
            loaded.report()["apply"]["service.listen"]["phase"],
            "service_restart"
        );
    }
    #[test]
    fn unknown_malformed_and_invalid_values_fail() {
        for text in [
            "unknown = 1",
            "[session]\nunknown = 1",
            "[session]\nfps = '60'",
            "[session]\nfps = 0",
            "[limits]\nmax_fps = 30",
            "[limits]\nallowed_codecs = ['oops']",
            "[service]\nlisten = 'oops'",
            "not toml",
        ] {
            let (_dir, path) = config(text);
            assert!(
                Loaded::load_path(Some(&path), true).is_err(),
                "accepted {text}"
            );
        }
    }
    #[test]
    fn missing_default_optional_explicit_required() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("missing");
        assert!(!Loaded::load_path(Some(&path), false).unwrap().file_loaded);
        assert!(Loaded::load_path(Some(&path), true).is_err());
    }
    #[test]
    fn parse_errors_never_echo_field_values() {
        for text in [
            "[session]\nsecret = 'credential-must-not-appear'",
            "[session]\nfps = 'credential-must-not-appear'",
        ] {
            let (_dir, path) = config(text);
            let error = Loaded::load_path(Some(&path), true).unwrap_err();
            assert!(!format!("{error:#}").contains("credential-must-not-appear"));
            assert!(error.to_string().contains("line 2"));
        }
    }
    #[test]
    fn xdg_fallback_never_uses_cwd() {
        assert_eq!(
            user_config_path(Some(OsStr::new("/settings")), Some(OsStr::new("/home/me"))),
            Some(PathBuf::from("/settings/waybridge/config.toml"))
        );
        for value in [None, Some(OsStr::new("")), Some(OsStr::new("relative"))] {
            assert_eq!(
                user_config_path(value, Some(OsStr::new("/home/me"))),
                Some(PathBuf::from("/home/me/.config/waybridge/config.toml"))
            );
        }
        assert_eq!(user_config_path(None, None), None);
    }
    #[test]
    fn startup_and_runtime_share_codec_and_limit_rules() {
        let limits = Limits {
            max_fps: 75,
            max_bitrate: 40_000,
            allowed_codecs: vec!["h264".into()],
        };
        assert!(limits.validate_stream(75, 40_000, "auto", false).is_ok());
        assert!(limits
            .validate_stream(75, 40_000, "h264-nvenc", false)
            .is_ok());
        assert!(limits.validate_stream(75, 40_000, "h264", false).is_ok());
        assert!(limits.validate_stream(76, 40_000, "auto", false).is_err());
        assert!(limits.validate_stream(75, 40_001, "auto", false).is_err());
        assert!(limits.validate_stream(75, 40_000, "auto", true).is_err());
    }

    #[test]
    fn supported_listen_and_absolute_socket_forms_remain_valid() {
        for listen in ["localhost:49090", "127.0.0.1:0", "[::1]:49090"] {
            let mut config = Config::default();
            config.session.listen = listen.into();
            config.session.socket = "/tmp/waybridge-test/socket".into();
            config.validate().unwrap();
        }
    }
}

#[cfg(test)]
mod media_budget_tests {
    use super::*;
    #[test]
    fn media_limits_are_validated_and_part_of_effective_configuration() {
        let mut config = Config::default();
        assert!(config.validate().is_ok());
        assert_eq!(
            serde_json::to_value(&config).unwrap()["media_budget"]["encoders"],
            8
        );
        config.media_budget.encoders = 0;
        assert!(config.validate().is_err());
        config.media_budget.encoders = 8;
        config.media_budget.window_cpu_mib = config.media_budget.cpu_mib + 1;
        assert!(config.validate().is_err());
        config.media_budget.window_cpu_mib = 4;
        config.media_budget.gpu_mib = 3;
        assert!(config.validate().is_err());
    }
}
