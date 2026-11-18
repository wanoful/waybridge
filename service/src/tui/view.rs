use super::{label, App, SettingsPage, FIELDS, TABS};
use ratatui::{
    layout::{Constraint, Direction, Layout, Rect},
    style::{Color, Modifier, Style},
    text::Line,
    widgets::{Block, Clear, List, ListState, Paragraph, Tabs, Wrap},
    Frame,
};
use std::time::Instant;

impl App {
    pub(super) fn draw(&self, frame: &mut Frame) {
        let area = frame.area();
        let [tabs, body, footer] = Layout::vertical([
            Constraint::Length(3),
            Constraint::Min(1),
            Constraint::Length(5),
        ])
        .areas(area);
        frame.render_widget(
            Tabs::new(TABS.iter().copied())
                .block(Block::bordered().title(" Waybridge administration "))
                .select(self.page)
                .highlight_style(
                    Style::default()
                        .fg(Color::Cyan)
                        .add_modifier(Modifier::BOLD),
                ),
            tabs,
        );
        match self.page {
            0 => self.draw_overview(frame, body),
            1 => self.draw_sessions(frame, body),
            2 => self.draw_devices(frame, body),
            _ => self.draw_settings(frame, body),
        }
        let help = match (self.page, self.settings_page) {
            (0, _) if self.user_unit => "p Pair · h Preflight · u Start/restart user unit",
            (0, _) => "p Pair · h Preflight",
            (1, _) => "↑↓ Select · x End · / Filter log · Space Pause · PgUp/PgDn Scroll",
            (2, _) => "↑↓ Select · x Revoke",
            (3, SettingsPage::Main) => {
                "Enter Edit · d Default · s Review save · a Apps · i Components"
            }
            (3, SettingsPage::Applications) => {
                "↑↓ Select · / Find · Enter Draft · s Preview · c Core · i Components"
            }
            _ => "Enter Portal preferences · s Review save · n Notifications · c Core · a Apps",
        };
        paragraph(
            frame,
            footer,
            " Controls ",
            format!(
                "{help}\n1–4/Tab View · r Refresh · q Quit\n{}",
                self.message
            ),
            0,
        );
        if let Some(editor) = &self.editor {
            editor.draw(frame);
        }
        if let Some(dialog) = &self.confirm {
            let area = overlay(frame);
            let [content, controls] =
                Layout::vertical([Constraint::Min(1), Constraint::Length(2)]).areas(area);
            paragraph(
                frame,
                content,
                dialog.title,
                dialog.lines.join("\n"),
                dialog.offset,
            );
            frame.render_widget(
                Paragraph::new("Enter/y Confirm · Esc Cancel · PgUp/PgDn Scroll"),
                controls,
            );
        }
    }

    fn draw_overview(&self, frame: &mut Frame, area: Rect) {
        let [service, preflight] = panes(area);
        let mut details = if self.online() {
            format!(
                "Service online · PID {}\nListening on {}",
                self.status["pid"],
                label(&self.status["listen"])
            )
        } else {
            let start = if self.user_unit {
                "Start the user unit with u or run waybridge-service serve."
            } else {
                "Run waybridge-service serve with the same configuration and runtime arguments."
            };
            format!("Service offline\n{}\n\n{start}", self.status_error)
        };
        if let Some((code, expires)) = &self.pairing {
            details.push_str(&format!(
                "\n\nOne-use pairing code · {}s remaining\n{code}",
                expires.saturating_duration_since(Instant::now()).as_secs()
            ));
        } else if self.online() {
            details.push_str("\n\nPress p to generate a one-use pairing code.");
        }
        if self.online() {
            details.push_str(&format!(
                "\n\nFull certificate SHA-256\n{}",
                label(&self.status["certificate_sha256"])
            ));
        }
        if let Ok((path, _)) = self.config_path() {
            details.push_str(&format!("\n\nConfig: {}", path.display()));
        }
        paragraph(frame, service, " Service & pairing ", details, 0);
        paragraph(
            frame,
            preflight,
            " Environment ",
            if self.preflight.is_empty() {
                "Press h to check session admission and local dependencies.".into()
            } else {
                self.preflight.join("\n")
            },
            0,
        );
    }

    fn draw_sessions(&self, frame: &mut Frame, area: Rect) {
        let [sessions, log] = panes(area);
        list(
            frame,
            sessions,
            " Sessions ",
            self.sessions
                .iter()
                .map(|session| {
                    format!(
                        "{} · PID {} · port {}",
                        label(&session["name"]),
                        session["pid"],
                        session["port"]
                    )
                })
                .collect(),
            self.selection,
        );
        let pattern = self.log_filter.to_lowercase();
        let matching: Vec<_> = self
            .log
            .lines()
            .filter(|line| pattern.is_empty() || line.to_lowercase().contains(&pattern))
            .collect();
        let end = matching
            .len()
            .saturating_sub(self.log_offset.min(matching.len().saturating_sub(1)));
        let start = end.saturating_sub(log.height.saturating_sub(2) as usize);
        let title = if self.log_pause {
            " Session log · paused "
        } else {
            " Session log "
        };
        paragraph(
            frame,
            log,
            title,
            if self.sessions.is_empty() {
                "No running session to display".into()
            } else {
                matching[start..end].join("\n")
            },
            0,
        );
    }

    fn draw_devices(&self, frame: &mut Frame, area: Rect) {
        let [devices, detail] = panes(area);
        list(
            frame,
            devices,
            " Authorized devices ",
            self.clients
                .iter()
                .map(|device| label(&device["label"]).to_owned())
                .collect(),
            self.selection,
        );
        let device = self
            .clients
            .get(self.selection)
            .map(|device| {
                format!(
                    "{}\n\nDevice ID\n{}\n\nPaired at (Unix time)\n{}",
                    label(&device["label"]),
                    label(&device["client_id"]),
                    device["created_unix"]
                )
            })
            .unwrap_or_default();
        paragraph(frame, detail, " Device ", device, 0);
    }

    fn draw_settings(&self, frame: &mut Frame, area: Rect) {
        let [left, right] = panes(area);
        match self.settings_page {
            SettingsPage::Main => self.draw_configuration(frame, left, right),
            SettingsPage::Applications => {
                list(
                    frame,
                    left,
                    " Installed applications ",
                    self.filtered_apps()
                        .map(|app| format!("{} · {}", label(&app["name"]), label(&app["id"])))
                        .collect(),
                    self.selection,
                );
                let detail = self.override_file.as_ref().map(|draft| format!("{}\n\n{}", draft.file.path.display(), draft.text))
                    .unwrap_or_else(|| "Enter opens an application override. Use [default] and [actions.<id>] for its launch settings.".into());
                paragraph(frame, right, " Application override draft ", detail, 0);
            }
            SettingsPage::Components => {
                paragraph(
                    frame,
                    left,
                    " Desktop components ",
                    self.components.join("\n"),
                    0,
                );
                let detail = self.portal_file.as_ref().map(|(file, draft)| format!("{}\n\n{draft}", file.path.display()))
                    .unwrap_or_else(|| "Enter opens the user's Waybridge Portal preferences. The notification provider can be selected through its systemd user unit.".into());
                paragraph(frame, right, " Waybridge Portal ", detail, 0);
            }
        }
    }

    fn draw_configuration(&self, frame: &mut Frame, list_area: Rect, detail: Rect) {
        let Some(settings) = &self.settings else {
            paragraph(
                frame,
                list_area,
                " Configuration ",
                "Press r to load the configuration file.".into(),
                0,
            );
            return;
        };
        list(
            frame,
            list_area,
            if settings.dirty() {
                " Configuration · draft "
            } else {
                " Configuration "
            },
            FIELDS
                .iter()
                .map(|field| format!("{}  {}", field.section, field.label))
                .collect(),
            self.selection,
        );
        let field = FIELDS[self.selection.min(FIELDS.len() - 1)];
        let name = field.path();
        let service = &self.status["configuration"];
        let effective = if self.online() {
            field.value(service)
        } else {
            "Service offline".into()
        };
        let saved = field.value(&settings.saved);
        let source = label(&settings.saved["sources"][&name]["kind"]);
        let draft = settings
            .edited(field)
            .unwrap_or_else(|| "Inherited default".into());
        let timing = match (service["sources"][&name]["kind"].as_str(), field.section) {
            (Some("cli"), _) => "Service launch parameter overrides the file",
            (_, "service") => "Service restart",
            (_, "session") if matches!(field.key, "listen" | "socket") => {
                "Standalone launch. Managed sessions allocate these values"
            }
            _ => "New session. Existing sessions keep their startup configuration",
        };
        let mut details = format!("{}\n{} · {}\n\nService startup value: {effective}\nSaved value: {saved} ({source})\nDraft value: {draft}\n\nApplies: {timing}", settings.file.path.display(), name, field.input_hint());
        if let Some(problem) = &settings.problem {
            details.insert_str(0, &format!("Saved configuration is invalid: {problem}\n\n"));
        }
        paragraph(frame, detail, field.label, details, 0);
    }
}

fn clean(value: &str) -> String {
    value
        .chars()
        .filter(|character| *character == '\n' || !character.is_control())
        .collect()
}

fn panes(area: Rect) -> [Rect; 2] {
    Layout::new(
        if area.width >= 72 {
            Direction::Horizontal
        } else {
            Direction::Vertical
        },
        [Constraint::Percentage(48), Constraint::Percentage(52)],
    )
    .areas(area)
}

pub(super) fn paragraph(frame: &mut Frame, area: Rect, title: &str, text: String, offset: u16) {
    frame.render_widget(
        Paragraph::new(clean(&text))
            .block(Block::bordered().title(title))
            .wrap(Wrap { trim: false })
            .scroll((offset, 0)),
        area,
    );
}

fn list(frame: &mut Frame, area: Rect, title: &str, entries: Vec<String>, selection: usize) {
    if entries.is_empty() {
        paragraph(frame, area, title, "No entries".into(), 0);
        return;
    }
    let widget = List::new(
        entries
            .iter()
            .map(|entry| Line::raw(clean(entry).replace('\n', " "))),
    )
    .block(Block::bordered().title(title))
    .highlight_symbol("› ")
    .highlight_style(
        Style::default()
            .fg(Color::Cyan)
            .add_modifier(Modifier::BOLD),
    );
    frame.render_stateful_widget(
        widget,
        area,
        &mut ListState::default().with_selected(Some(selection)),
    );
}

pub(super) fn overlay(frame: &mut Frame) -> Rect {
    let area = frame.area();
    let middle: [Rect; 3] = Layout::vertical([
        Constraint::Percentage(13),
        Constraint::Percentage(74),
        Constraint::Percentage(13),
    ])
    .areas(area);
    let [_, area, _] = Layout::horizontal([
        Constraint::Percentage(7),
        Constraint::Percentage(86),
        Constraint::Percentage(7),
    ])
    .areas(middle[1]);
    frame.render_widget(Clear, area);
    area
}
