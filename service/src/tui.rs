mod editor;
mod settings;
mod view;

use anyhow::{bail, Context, Result};
use crossterm::{
    event::{
        DisableBracketedPaste, EnableBracketedPaste, Event, EventStream, KeyCode, KeyEvent,
        KeyEventKind, KeyModifiers,
    },
    execute,
};
use editor::{Edit, Editor};
use futures_util::StreamExt;
use serde_json::{json, Value};
use settings::{FileDraft, Settings, FIELDS};
use std::{
    io::{self, IsTerminal},
    path::{Path, PathBuf},
    process::Stdio,
    time::{Duration, Instant},
};
use tokio::{
    process::Command,
    time::{interval, timeout, MissedTickBehavior},
};
use waybridge_config::Loaded;

const TABS: &[&str] = &[
    "1 Overview & pairing",
    "2 Sessions & logs",
    "3 Trusted devices",
    "4 Settings",
];
const NOTIFICATION_UNIT: &str = "waybridge-notifications.service";
const PORTAL_TEMPLATE: &str =
    include_str!("../../integration/xdg-desktop-portal/waybridge-portals.conf");
const DRAFT_LIMIT: usize = 32_768;

pub fn config_home() -> Option<PathBuf> {
    let path = Loaded::selected_path(None)?;
    path.parent()?.parent().map(Path::to_path_buf)
}

pub async fn run(runtime: PathBuf, explicit: Option<PathBuf>, user_unit: bool) -> Result<()> {
    if !io::stdin().is_terminal() || !io::stdout().is_terminal() {
        bail!("Waybridge TUI requires a terminal (allocate a TTY for SSH)");
    }
    let mut terminal = ratatui::try_init().inspect_err(|_| ratatui::restore())?;
    let _restore = RestoreTerminal;
    execute!(io::stdout(), EnableBracketedPaste)?;
    let mut app = App::new(runtime, explicit, user_unit);
    let mut events = EventStream::new();
    let mut tick = interval(Duration::from_secs(2));
    tick.set_missed_tick_behavior(MissedTickBehavior::Skip);
    let mut terminate = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())?;
    let mut hangup = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::hangup())?;
    loop {
        terminal.draw(|frame| app.draw(frame))?;
        tokio::select! {
            _ = terminate.recv() => break,
            _ = hangup.recv() => break,
            _ = tick.tick() => app.refresh().await,
            event = events.next() => match event {
                Some(Ok(Event::Key(key))) if matches!(key.kind, KeyEventKind::Press | KeyEventKind::Repeat) => {
                    if key.code == KeyCode::Char('c') && key.modifiers.contains(KeyModifiers::CONTROL) {
                        break;
                    }
                    match app.key(key).await {
                        Ok(true) => break,
                        Ok(false) => {},
                        Err(error) => app.message = format!("{error:#}"),
                    }
                }
                Some(Ok(Event::Paste(text))) => {
                    if let Some(editor) = app.editor.as_mut() { editor.insert(&text); }
                }
                Some(Ok(_)) => {},
                Some(Err(error)) => return Err(error.into()),
                None => break,
            }
        }
    }
    Ok(())
}

struct RestoreTerminal;

impl Drop for RestoreTerminal {
    fn drop(&mut self) {
        let _ = execute!(io::stdout(), DisableBracketedPaste);
        ratatui::restore();
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum SettingsPage {
    Main,
    Applications,
    Components,
}

struct App {
    runtime: PathBuf,
    explicit: Option<PathBuf>,
    user_unit: bool,
    status: Value,
    status_error: String,
    pairing: Option<(String, Instant)>,
    sessions: Vec<Value>,
    clients: Vec<Value>,
    log: String,
    log_pause: bool,
    log_filter: String,
    log_offset: usize,
    page: usize,
    selection: usize,
    settings_page: SettingsPage,
    settings: Option<Settings>,
    applications: Vec<Value>,
    application_context: Value,
    app_search: String,
    override_file: Option<OverrideDraft>,
    portal_file: Option<(FileDraft, String)>,
    components: Vec<String>,
    preflight: Vec<String>,
    editor: Option<Editor>,
    confirm: Option<Confirmation>,
    message: String,
}

struct OverrideDraft {
    app: String,
    context: Value,
    file: FileDraft,
    text: String,
}

enum Decision {
    End(Value),
    Revoke(String),
    SaveSettings,
    SaveOverride,
    SavePortal,
    Restart(u64),
    Notifications(bool),
    Quit,
    Reload,
}

struct Confirmation {
    title: &'static str,
    lines: Vec<String>,
    offset: u16,
    decision: Decision,
}

impl App {
    fn new(runtime: PathBuf, explicit: Option<PathBuf>, user_unit: bool) -> Self {
        let user_unit = user_unit && explicit.is_none();
        Self {
            runtime,
            explicit,
            user_unit,
            status: Value::Null,
            status_error: "Checking service…".into(),
            pairing: None,
            sessions: Vec::new(),
            clients: Vec::new(),
            log: String::new(),
            log_pause: false,
            log_filter: String::new(),
            log_offset: 0,
            page: 0,
            selection: 0,
            settings_page: SettingsPage::Main,
            settings: None,
            applications: Vec::new(),
            application_context: Value::Null,
            app_search: String::new(),
            override_file: None,
            portal_file: None,
            components: Vec::new(),
            preflight: Vec::new(),
            editor: None,
            confirm: None,
            message: String::new(),
        }
    }

    async fn rpc(&self, request: Value) -> Result<Value> {
        let response = timeout(
            Duration::from_secs(3),
            crate::admin_request(&self.runtime, &request),
        )
        .await
        .context("Local service did not respond")??;
        if response["ok"] != true {
            bail!(
                "{}",
                response["error"]["message"]
                    .as_str()
                    .unwrap_or("Administration request failed")
            );
        }
        Ok(response["result"].clone())
    }

    fn online(&self) -> bool {
        !self.status.is_null()
    }

    async fn refresh(&mut self) {
        let pid = self.status["pid"].as_u64();
        match self.rpc(json!({"op":"status"})).await {
            Ok(status) => {
                self.status = status;
                self.status_error.clear();
            }
            Err(error) => {
                self.status = Value::Null;
                self.status_error = error.to_string();
            }
        }
        if self.status["pid"].as_u64() != pid
            || self
                .pairing
                .as_ref()
                .is_some_and(|(_, expires)| Instant::now() >= *expires)
        {
            self.pairing = None;
        }
        if self.online() {
            let result = match self.page {
                1 => self.refresh_sessions().await,
                2 => self.refresh_clients().await,
                _ => Ok(()),
            };
            if let Err(error) = result {
                self.message = error.to_string();
            }
        } else {
            self.sessions.clear();
            self.clients.clear();
        }
    }

    async fn refresh_sessions(&mut self) -> Result<()> {
        let selected = self
            .sessions
            .get(self.selection)
            .and_then(|session| session["name"].as_str())
            .map(str::to_owned);
        self.sessions = entries(self.rpc(json!({"op":"sessions"})).await?);
        if let Some(index) = self
            .sessions
            .iter()
            .position(|session| session["name"].as_str() == selected.as_deref())
        {
            self.selection = index;
        } else {
            self.selection = self.selection.min(self.sessions.len().saturating_sub(1));
            self.log_offset = 0;
            self.log.clear();
        }
        self.refresh_log().await;
        Ok(())
    }

    async fn refresh_log(&mut self) {
        if self.log_pause {
            return;
        }
        if let Some(session) = self.sessions.get(self.selection) {
            self.log = match self
                .rpc(json!({"op":"session_log","session":session["name"]}))
                .await
            {
                Ok(value) => label(&value["log"]).into(),
                Err(error) => format!("Log unavailable: {error}"),
            };
        }
    }

    async fn refresh_clients(&mut self) -> Result<()> {
        let id = self
            .clients
            .get(self.selection)
            .and_then(|device| device["client_id"].as_str())
            .map(str::to_owned);
        self.clients = entries(self.rpc(json!({"op":"clients"})).await?);
        if let Some(index) = self
            .clients
            .iter()
            .position(|device| device["client_id"].as_str() == id.as_deref())
        {
            self.selection = index;
        } else {
            self.selection = self.selection.min(self.clients.len().saturating_sub(1));
        }
        Ok(())
    }

    fn config_path(&self) -> Result<(PathBuf, bool)> {
        if let Some(path) = self.status["configuration"]["config_file"]["path"].as_str() {
            return Ok((
                path.into(),
                self.status["configuration"]["config_file"]["explicit"] == true,
            ));
        }
        let path = Loaded::selected_path(self.explicit.as_deref())
            .context("Set HOME, XDG_CONFIG_HOME or --config")?;
        Ok((
            if path.is_absolute() {
                path
            } else {
                std::env::current_dir()?.join(path)
            },
            self.explicit.is_some(),
        ))
    }

    fn user_config_home(&self) -> Result<PathBuf> {
        self.status["config_home"]
            .as_str()
            .map(PathBuf::from)
            .or_else(config_home)
            .context("Set HOME or XDG_CONFIG_HOME to edit user integration files")
    }

    fn load_settings(&mut self) -> Result<()> {
        let (path, explicit) = self.config_path()?;
        self.settings = Some(Settings::open(path, explicit)?);
        Ok(())
    }

    fn settings(&mut self) -> Result<&mut Settings> {
        let (path, _) = self.config_path()?;
        if self
            .settings
            .as_ref()
            .is_some_and(|settings| settings.file.path != path && settings.dirty())
        {
            bail!("Service config path changed. Reload settings before editing or saving.");
        }
        if self
            .settings
            .as_ref()
            .is_none_or(|settings| settings.file.path != path)
        {
            self.load_settings()?;
        }
        self.settings.as_mut().context("Settings unavailable")
    }

    fn dirty(&self) -> bool {
        self.settings.as_ref().is_some_and(Settings::dirty)
            || self.override_file.as_ref().is_some_and(|draft| {
                draft.file.saved.as_deref().unwrap_or("version = 1\n") != draft.text
            })
            || self.portal_file.as_ref().is_some_and(|(file, draft)| {
                file.saved.as_deref().unwrap_or(PORTAL_TEMPLATE) != draft
            })
    }

    fn confirm(&mut self, title: &'static str, lines: Vec<String>, decision: Decision) {
        self.confirm = Some(Confirmation {
            title,
            lines,
            decision,
            offset: 0,
        });
    }

    async fn key(&mut self, key: KeyEvent) -> Result<bool> {
        if self.confirm.is_some() {
            return self.confirm_key(key).await;
        }
        if let Some(mut editor) = self.editor.take() {
            if key.code == KeyCode::Esc {
                return Ok(false);
            }
            if (!editor.multiline && key.code == KeyCode::Enter)
                || (key.code == KeyCode::Char('s') && key.modifiers.contains(KeyModifiers::CONTROL))
            {
                if let Err(error) = self.finish_edit(&editor) {
                    self.editor = Some(editor);
                    return Err(error);
                }
            } else {
                editor.key(key);
                self.editor = Some(editor);
            }
            return Ok(false);
        }
        match key.code {
            KeyCode::Char('q') => {
                if !self.dirty() {
                    return Ok(true);
                }
                self.confirm(
                    "Discard changes and exit?",
                    vec!["One or more settings pages contain an unsaved draft.".into()],
                    Decision::Quit,
                );
            }
            KeyCode::Char(number @ '1'..='4') => {
                self.go_to((number as usize) - ('1' as usize)).await?
            }
            KeyCode::Tab => self.go_to((self.page + 1) % TABS.len()).await?,
            KeyCode::BackTab => {
                self.go_to((self.page + TABS.len() - 1) % TABS.len())
                    .await?
            }
            KeyCode::Up | KeyCode::Down => {
                if key.code == KeyCode::Up {
                    self.selection = self.selection.saturating_sub(1);
                } else {
                    self.selection = (self.selection + 1).min(self.list_len().saturating_sub(1));
                }
                if self.page == 1 {
                    self.log_offset = 0;
                    self.log_pause = false;
                    self.refresh_log().await;
                }
            }
            KeyCode::Char('r') => {
                self.refresh().await;
                if self.page == 3 {
                    if self.dirty() {
                        self.confirm(
                            "Discard drafts and reload?",
                            vec!["Reload reads the latest files from disk.".into()],
                            Decision::Reload,
                        );
                    } else {
                        self.reload_settings_page().await?;
                    }
                }
            }
            _ => match self.page {
                0 => self.overview_key(key).await?,
                1 => self.sessions_key(key),
                2 => self.devices_key(key),
                _ => self.settings_key(key).await?,
            },
        }
        Ok(false)
    }

    async fn go_to(&mut self, page: usize) -> Result<()> {
        self.page = page;
        self.selection = 0;
        self.message.clear();
        match page {
            1 => {
                self.log_pause = false;
                self.log_offset = 0;
                if self.online() {
                    self.refresh_sessions().await?;
                }
            }
            2 if self.online() => self.refresh_clients().await?,
            3 if self.settings_page == SettingsPage::Main => {
                self.settings()?;
            }
            _ => {}
        }
        Ok(())
    }

    fn list_len(&self) -> usize {
        match self.page {
            1 => self.sessions.len(),
            2 => self.clients.len(),
            3 if self.settings_page == SettingsPage::Main => FIELDS.len(),
            3 if self.settings_page == SettingsPage::Applications => self.filtered_apps().count(),
            _ => 0,
        }
    }

    async fn overview_key(&mut self, key: KeyEvent) -> Result<()> {
        match key.code {
            KeyCode::Char('p') if self.online() => {
                let pairing = self.rpc(json!({"op":"pair"})).await?;
                self.pairing = Some((
                    label(&pairing["pair_code"]).into(),
                    Instant::now()
                        + Duration::from_secs(pairing["expires_in"].as_u64().unwrap_or(120)),
                ));
            }
            KeyCode::Char('u') if self.online() && self.user_unit => {
                let pid = self.status["pid"]
                    .as_u64()
                    .context("Unrecognized service instance")?;
                if unit_pid().await? != pid {
                    bail!("This entry point is managed outside waybridge.service");
                }
                self.confirm("Restart Waybridge service?", vec!["Client connections will drop. Running compositor sessions keep their applications.".into()], Decision::Restart(pid));
            }
            KeyCode::Char('u') if !self.online() && self.user_unit => {
                systemctl(&["start", "waybridge.service"]).await?;
                self.refresh().await;
            }
            KeyCode::Char('h') => self.check_environment().await?,
            _ => {}
        }
        Ok(())
    }

    fn sessions_key(&mut self, key: KeyEvent) {
        match key.code {
            KeyCode::Char('x') => {
                if let Some(session) = self.sessions.get(self.selection).cloned() {
                    self.confirm(
                        "End this session?",
                        vec![
                            format!("{} · PID {}", label(&session["name"]), session["pid"]),
                            "The session and its applications will close.".into(),
                        ],
                        Decision::End(session),
                    );
                }
            }
            KeyCode::Char(' ') => self.log_pause = !self.log_pause,
            KeyCode::Char('/') => {
                self.editor = Some(Editor::new(
                    Edit::FilterLog,
                    "Log filter",
                    &self.log_filter,
                    false,
                ))
            }
            KeyCode::PageUp => {
                self.log_offset =
                    (self.log_offset + 12).min(self.log.lines().count().saturating_sub(1))
            }
            KeyCode::PageDown => self.log_offset = self.log_offset.saturating_sub(12),
            _ => {}
        }
    }

    fn devices_key(&mut self, key: KeyEvent) {
        if key.code == KeyCode::Char('x') {
            if let Some(device) = self.clients.get(self.selection) {
                self.confirm(
                    "Revoke this device?",
                    vec![
                        label(&device["label"]).into(),
                        "Its active connections will close immediately.".into(),
                    ],
                    Decision::Revoke(label(&device["client_id"]).into()),
                );
            }
        }
    }

    async fn settings_key(&mut self, key: KeyEvent) -> Result<()> {
        match key.code {
            KeyCode::Char('c' | 'a' | 'i') => {
                self.settings_page = match key.code {
                    KeyCode::Char('c') => SettingsPage::Main,
                    KeyCode::Char('a') => SettingsPage::Applications,
                    _ => SettingsPage::Components,
                };
                self.selection = 0;
                self.message.clear();
                match self.settings_page {
                    SettingsPage::Main => {
                        self.settings()?;
                    }
                    SettingsPage::Applications => self.load_applications().await?,
                    SettingsPage::Components => self.load_components().await,
                }
            }
            _ => match self.settings_page {
                SettingsPage::Main => self.configuration_key(key)?,
                SettingsPage::Applications => self.application_key(key).await?,
                SettingsPage::Components => self.component_key(key).await?,
            },
        }
        Ok(())
    }

    fn configuration_key(&mut self, key: KeyEvent) -> Result<()> {
        let index = self.selection.min(FIELDS.len() - 1);
        let field = FIELDS[index];
        match key.code {
            KeyCode::Enter => {
                let settings = self.settings()?;
                let value = settings
                    .edited(field)
                    .unwrap_or_else(|| field.value(&settings.saved));
                self.editor = Some(Editor::new(
                    Edit::Setting(index),
                    field.label,
                    &value,
                    false,
                ));
            }
            KeyCode::Char('d') => self.settings()?.inherit(field),
            KeyCode::Char('s') => {
                let running = self.status["configuration"].clone();
                let settings = self.settings()?;
                if !settings.dirty() {
                    bail!("No configuration changes");
                }
                let (_, changes) = settings.preview(&running)?;
                self.confirm("Save configuration?", changes, Decision::SaveSettings);
            }
            _ => {}
        }
        Ok(())
    }

    async fn load_applications(&mut self) -> Result<()> {
        let mut catalog = self
            .local_helper(
                json!({"op":"override_catalog"}),
                json!({"op":"local-override-catalog"}),
            )
            .await?;
        self.application_context = catalog["context"].take();
        self.applications = entries(catalog["apps"].take());
        Ok(())
    }

    async fn local_helper(&self, online: Value, offline: Value) -> Result<Value> {
        if self.online() {
            return self.rpc(online).await;
        }
        let python = Loaded::load(self.explicit.as_deref())
            .ok()
            .map(|loaded| loaded.config.service.python)
            .unwrap_or_else(|| "python3".into());
        timeout(Duration::from_secs(3), crate::run_helper(&python, &offline))
            .await
            .context("Local helper timed out")?
    }

    fn filtered_apps(&self) -> impl Iterator<Item = &Value> {
        let query = self.app_search.to_lowercase();
        self.applications.iter().filter(move |app| {
            query.is_empty()
                || label(&app["name"]).to_lowercase().contains(&query)
                || label(&app["id"]).to_lowercase().contains(&query)
        })
    }

    async fn preview_override(&self, draft: &OverrideDraft) -> Result<Value> {
        if draft.text.len() > DRAFT_LIMIT {
            bail!("Application draft exceeds the preview size limit");
        }
        self.local_helper(
            json!({"op":"preview_override","app":draft.app,"draft":draft.text,"context":draft.context}),
            json!({"op":"local-preview-override","app":draft.app,"draft":draft.text,"context":draft.context}),
        ).await
    }

    async fn application_key(&mut self, key: KeyEvent) -> Result<()> {
        match key.code {
            KeyCode::Char('/') => {
                self.editor = Some(Editor::new(
                    Edit::FilterApps,
                    "Find installed applications",
                    &self.app_search,
                    false,
                ))
            }
            KeyCode::Enter => {
                let app = self
                    .filtered_apps()
                    .nth(self.selection)
                    .and_then(|entry| entry["id"].as_str())
                    .context("Select an installed application")?
                    .to_owned();
                if self.override_file.as_ref().is_some_and(|draft| {
                    (draft.app != app || draft.context != self.application_context)
                        && draft.file.saved.as_deref().unwrap_or("version = 1\n") != draft.text
                }) {
                    bail!("Save the draft for the previous application, or use r to discard and reload");
                }
                if self.override_file.as_ref().is_none_or(|draft| {
                    draft.app != app || draft.context != self.application_context
                }) {
                    if app.is_empty() || app.contains('/') || !app.ends_with(".desktop") {
                        bail!("Invalid desktop application ID");
                    }
                    let path = Path::new(
                        self.application_context["config_home"]
                            .as_str()
                            .context("Application configuration directory unavailable")?,
                    )
                    .join("waybridge/desktop-overrides")
                    .join(format!("{app}.toml"));
                    let file = FileDraft::open(path)?;
                    let text = file.saved.as_deref().unwrap_or("version = 1\n").to_owned();
                    self.override_file = Some(OverrideDraft {
                        app,
                        context: self.application_context.clone(),
                        file,
                        text,
                    });
                }
                let draft = self
                    .override_file
                    .as_ref()
                    .context("Select an installed application")?;
                self.editor = Some(Editor::new(
                    Edit::Override,
                    "Edit application override (TOML)",
                    &draft.text,
                    true,
                ));
            }
            KeyCode::Char('s' | 'p') => {
                let draft = self
                    .override_file
                    .as_ref()
                    .context("Open an application with Enter first")?;
                let plan = self.preview_override(draft).await?;
                let mut lines = vec![format!("{} · Next launch", draft.file.path.display())];
                lines.extend(
                    serde_json::to_string_pretty(&plan)?
                        .lines()
                        .map(str::to_owned),
                );
                self.confirm(
                    "Application launch preview · Save?",
                    lines,
                    Decision::SaveOverride,
                );
            }
            _ => {}
        }
        Ok(())
    }

    async fn load_components(&mut self) {
        let status = systemctl(&[
            "show",
            "--property=LoadState,ActiveState,UnitFileState",
            NOTIFICATION_UNIT,
        ])
        .await;
        self.components = status
            .map(|text| text.lines().map(str::to_owned).collect())
            .unwrap_or_else(|error| vec![error.to_string()]);
    }

    async fn component_key(&mut self, key: KeyEvent) -> Result<()> {
        match key.code {
            KeyCode::Enter => {
                if self.portal_file.is_none() {
                    let path = self
                        .user_config_home()?
                        .join("xdg-desktop-portal/waybridge-portals.conf");
                    let file = FileDraft::open(path)?;
                    let draft = file.saved.as_deref().unwrap_or(PORTAL_TEMPLATE).to_owned();
                    self.portal_file = Some((file, draft));
                }
                self.editor = Some(Editor::new(
                    Edit::Portal,
                    "Waybridge Portal preferences",
                    &self.portal_file.as_ref().unwrap().1,
                    true,
                ));
            }
            KeyCode::Char('s') => {
                let (file, draft) = self
                    .portal_file
                    .as_ref()
                    .context("Open Portal preferences with Enter first")?;
                if draft.len() > DRAFT_LIMIT {
                    bail!("Portal draft exceeds the preview size limit");
                }
                crate::run_helper(
                    "python3",
                    &json!({"op":"local-validate-portal","draft":draft}),
                )
                .await?;
                self.confirm(
                    "Save Portal preferences?",
                    vec![
                        file.path.display().to_string(),
                        "Uses the standard Portal configuration lookup on its next startup.".into(),
                    ],
                    Decision::SavePortal,
                );
            }
            KeyCode::Char('n') => {
                let enabled = self
                    .components
                    .iter()
                    .any(|line| line == "UnitFileState=enabled");
                if !self
                    .components
                    .iter()
                    .any(|line| line == "LoadState=loaded")
                {
                    bail!("Install the optional Waybridge notification provider unit first");
                }
                self.confirm(if enabled { "Disable Waybridge notifications?" } else { "Enable Waybridge notifications?" },
                    vec!["Uses the systemd user unit for future Waybridge sessions. Existing providers keep running.".into()], Decision::Notifications(!enabled));
            }
            _ => {}
        }
        Ok(())
    }

    async fn reload_settings_page(&mut self) -> Result<()> {
        self.settings = None;
        self.override_file = None;
        self.portal_file = None;
        match self.settings_page {
            SettingsPage::Main => self.load_settings(),
            SettingsPage::Applications => self.load_applications().await,
            SettingsPage::Components => {
                self.load_components().await;
                Ok(())
            }
        }
    }

    fn finish_edit(&mut self, editor: &Editor) -> Result<()> {
        let text: String = editor.text.iter().collect();
        match editor.target {
            Edit::Setting(index) => self.settings()?.set(FIELDS[index], &text)?,
            Edit::FilterLog => {
                self.log_filter = text;
                self.log_offset = 0;
            }
            Edit::FilterApps => {
                self.app_search = text;
                self.selection = 0;
            }
            Edit::Override => {
                self.override_file
                    .as_mut()
                    .context("Application draft closed")?
                    .text = text
            }
            Edit::Portal => self.portal_file.as_mut().context("Portal draft closed")?.1 = text,
        }
        self.message.clear();
        Ok(())
    }

    async fn confirm_key(&mut self, key: KeyEvent) -> Result<bool> {
        let Some(mut dialog) = self.confirm.take() else {
            return Ok(false);
        };
        match key.code {
            KeyCode::Esc | KeyCode::Char('n') => return Ok(false),
            KeyCode::PageUp => dialog.offset = dialog.offset.saturating_sub(10),
            KeyCode::PageDown => {
                dialog.offset = dialog
                    .offset
                    .saturating_add(10)
                    .min((dialog.lines.len().saturating_sub(1)) as u16)
            }
            KeyCode::Enter | KeyCode::Char('y') => {
                match dialog.decision {
                    Decision::End(session) => {
                        self.rpc(json!({"op":"end_session","session":session["name"],"pid":session["pid"],"started":session["started"]})).await?;
                        self.message = "Session shutdown requested".into();
                        self.refresh_sessions().await?;
                    }
                    Decision::Revoke(id) => {
                        self.rpc(json!({"op":"revoke","revoke_client_id":id}))
                            .await?;
                        self.refresh_clients().await?;
                        self.message = "Device revoked".into();
                    }
                    Decision::SaveSettings => {
                        let running = self.status["configuration"].clone();
                        self.settings()?.save(&running)?;
                        self.message = "Saved. Service settings apply after restart and session settings on next creation.".into();
                    }
                    Decision::SaveOverride => {
                        self.preview_override(
                            self.override_file
                                .as_ref()
                                .context("Application draft closed")?,
                        )
                        .await?;
                        let draft = self
                            .override_file
                            .as_mut()
                            .context("Application draft closed")?;
                        draft.file.save(draft.text.clone())?;
                        self.message = "Saved for the next application launch".into();
                    }
                    Decision::SavePortal => {
                        let (file, text) =
                            self.portal_file.as_mut().context("Portal draft closed")?;
                        file.save(text.clone())?;
                        self.message = "Saved for the next Portal startup".into();
                    }
                    Decision::Restart(pid) => {
                        if unit_pid().await? != pid {
                            bail!("The selected service instance changed");
                        }
                        systemctl(&["restart", "waybridge.service"]).await?;
                        self.refresh().await;
                    }
                    Decision::Notifications(enable) => {
                        systemctl(&[if enable { "enable" } else { "disable" }, NOTIFICATION_UNIT])
                            .await?;
                        self.load_components().await;
                    }
                    Decision::Quit => return Ok(true),
                    Decision::Reload => self.reload_settings_page().await?,
                }
                return Ok(false);
            }
            _ => {}
        }
        self.confirm = Some(dialog);
        Ok(false)
    }

    async fn check_environment(&mut self) -> Result<()> {
        let (path, explicit) = self.config_path()?;
        let loaded = Loaded::load_path(Some(&path), explicit)?;
        let binary = self.status["configuration"]["values"]["service"]["compositor"]
            .as_str()
            .unwrap_or(&loaded.config.service.compositor);
        let mut command = Command::new(binary);
        command
            .arg("--check-session")
            .stdin(Stdio::null())
            .kill_on_drop(true);
        if loaded.file_loaded {
            command.arg("--config").arg(path);
        }
        let status = timeout(Duration::from_secs(3), command.output()).await;
        let readiness = match status {
            Ok(Ok(output)) if output.status.success() => {
                let data: Value = serde_json::from_slice(&output.stdout)?;
                if data["available"] == true {
                    "New session admission: available".into()
                } else {
                    format!("New session admission: {}", label(&data["error"]))
                }
            }
            Ok(Ok(output)) => format!(
                "New session admission check failed: {}",
                String::from_utf8_lossy(&output.stderr).trim()
            ),
            Ok(Err(error)) => format!("Could not run {binary}: {error}"),
            Err(_) => "Session admission check timed out".into(),
        };
        self.preflight = vec![readiness];
        for (tool, role) in [
            ("Xwayland", "X11 applications"),
            ("pactl", "audio routing"),
            ("parec", "audio capture"),
            ("rsvg-convert", "SVG icons"),
            ("fusermount3", "file delivery"),
            ("nvidia-smi", "NVIDIA diagnostics"),
        ] {
            let available = std::env::var_os("PATH").is_some_and(|path| {
                std::env::split_paths(&path).any(|directory| directory.join(tool).is_file())
            });
            self.preflight.push(format!(
                "{tool} · {role}: {}",
                if available { "installed" } else { "not found" }
            ));
        }
        Ok(())
    }
}

fn entries(value: Value) -> Vec<Value> {
    match value {
        Value::Array(entries) => entries,
        _ => Vec::new(),
    }
}

fn label(value: &Value) -> &str {
    value.as_str().unwrap_or("—")
}

async fn systemctl(arguments: &[&str]) -> Result<String> {
    let mut command = Command::new("systemctl");
    command
        .arg("--user")
        .args(arguments)
        .stdin(Stdio::null())
        .kill_on_drop(true);
    let output = timeout(Duration::from_secs(5), command.output())
        .await
        .context("systemd user command timed out")??;
    if !output.status.success() {
        bail!("{}", String::from_utf8_lossy(&output.stderr).trim());
    }
    Ok(String::from_utf8_lossy(&output.stdout).trim().into())
}

async fn unit_pid() -> Result<u64> {
    systemctl(&["show", "--property=MainPID", "--value", "waybridge.service"])
        .await?
        .parse::<u64>()
        .context("Invalid systemd user service PID")
}
