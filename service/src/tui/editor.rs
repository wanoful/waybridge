use super::{
    view::{overlay, paragraph},
    DRAFT_LIMIT,
};
use crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use ratatui::{
    layout::{Constraint, Layout},
    widgets::Paragraph,
    Frame,
};

#[derive(Clone, Copy)]
pub(super) enum Edit {
    Setting(usize),
    FilterLog,
    FilterApps,
    Override,
    Portal,
}

pub(super) struct Editor {
    pub(super) target: Edit,
    title: &'static str,
    pub(super) text: Vec<char>,
    cursor: usize,
    pub(super) multiline: bool,
}

impl Editor {
    pub(super) fn new(target: Edit, title: &'static str, value: &str, multiline: bool) -> Self {
        let text: Vec<_> = value.chars().collect();
        let cursor = text.len();
        Self {
            target,
            title,
            text,
            cursor,
            multiline,
        }
    }

    pub(super) fn insert(&mut self, text: &str) {
        let remaining = DRAFT_LIMIT.saturating_sub(self.text.len());
        let text = text.replace("\r\n", "\n").replace('\r', "\n");
        let text: Vec<_> = text
            .chars()
            .filter(|character| {
                !character.is_control() || (self.multiline && matches!(character, '\n' | '\t'))
            })
            .take(remaining)
            .collect();
        let size = text.len();
        self.text.splice(self.cursor..self.cursor, text);
        self.cursor += size;
    }

    pub(super) fn key(&mut self, key: KeyEvent) {
        let start = self.text[..self.cursor]
            .iter()
            .rposition(|character| *character == '\n')
            .map_or(0, |index| index + 1);
        let end = self.text[self.cursor..]
            .iter()
            .position(|character| *character == '\n')
            .map_or(self.text.len(), |index| self.cursor + index);
        match key.code {
            KeyCode::Char('u') if key.modifiers.contains(KeyModifiers::CONTROL) => {
                self.text.clear();
                self.cursor = 0;
            }
            KeyCode::Left => self.cursor = self.cursor.saturating_sub(1),
            KeyCode::Right => self.cursor = (self.cursor + 1).min(self.text.len()),
            KeyCode::Home => self.cursor = start,
            KeyCode::End => self.cursor = end,
            KeyCode::Up if start > 0 => {
                let previous = self.text[..start - 1]
                    .iter()
                    .rposition(|character| *character == '\n')
                    .map_or(0, |index| index + 1);
                self.cursor = (previous + self.cursor - start).min(start - 1);
            }
            KeyCode::Down if end < self.text.len() => {
                let next_end = self.text[end + 1..]
                    .iter()
                    .position(|character| *character == '\n')
                    .map_or(self.text.len(), |index| end + 1 + index);
                self.cursor = (end + 1 + self.cursor - start).min(next_end);
            }
            KeyCode::Backspace if self.cursor > 0 => {
                self.cursor -= 1;
                self.text.remove(self.cursor);
            }
            KeyCode::Delete if self.cursor < self.text.len() => {
                self.text.remove(self.cursor);
            }
            KeyCode::Enter if self.multiline => self.insert("\n"),
            KeyCode::Char(character)
                if !key
                    .modifiers
                    .intersects(KeyModifiers::CONTROL | KeyModifiers::ALT) =>
            {
                self.insert(character.encode_utf8(&mut [0; 4]))
            }
            _ => {}
        }
    }

    pub(super) fn draw(&self, frame: &mut Frame) {
        let area = overlay(frame);
        let [content, controls] =
            Layout::vertical([Constraint::Min(1), Constraint::Length(2)]).areas(area);
        let before: String = self.text[..self.cursor].iter().collect();
        let after: String = self.text[self.cursor..].iter().collect();
        let line = before.matches('\n').count() as u16;
        paragraph(
            frame,
            content,
            self.title,
            format!("{before}▏{after}"),
            line.saturating_sub(content.height.saturating_sub(3)),
        );
        frame.render_widget(
            Paragraph::new(if self.multiline {
                "Ctrl+S Apply draft · Esc Cancel · Ctrl+U Clear"
            } else {
                "Enter Apply draft · Esc Cancel · Ctrl+U Clear"
            }),
            controls,
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn multiline_paste_preserves_crlf_and_terminal_cr_separators() {
        let mut editor = Editor::new(Edit::Override, "Override", "version = 1\n", true);
        editor.insert("[default]\r\nextra_argv = []\r\t# keep indentation\r");
        assert_eq!(
            editor.text.iter().collect::<String>(),
            "version = 1\n[default]\nextra_argv = []\n\t# keep indentation\n"
        );
    }
}
