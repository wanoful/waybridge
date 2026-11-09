//! Local Windows IME bridged directly to focused Wayland text-input-v3 clients.
//! Application surrounding text stays on Linux; composition messages carry a focus generation.
use crate::App;
use serde_json::{json, Value};
use smithay::reexports::{
    wayland_protocols::wp::text_input::zv3::server::{
        zwp_text_input_manager_v3::{self, ZwpTextInputManagerV3},
        zwp_text_input_v3::{self, ZwpTextInputV3},
    },
    wayland_server::{
        backend::ClientId, protocol::wl_surface::WlSurface, Client, DataInit, Dispatch,
        DisplayHandle, GlobalDispatch, New, Resource,
    },
};
use smithay::wayland::compositor::get_parent;
use smithay::wayland::seat::WaylandFocus;

#[derive(Default)]
struct Pending {
    enable: Option<bool>,
    rectangle: Option<[i32; 4]>,
    purpose: Option<u32>,
}
struct Input {
    object: ZwpTextInputV3,
    serial: u32,
    enabled: bool,
    rectangle: [i32; 4],
    purpose: u32,
    pending: Pending,
}
#[derive(Default)]
pub struct Ime {
    inputs: Vec<Input>,
    focus: Option<WlSurface>,
    generation: u64,
    published: Value,
}

impl Ime {
    pub fn new(dh: &DisplayHandle) -> Self {
        dh.create_global::<App, ZwpTextInputManagerV3, _>(1, ());
        Self::default()
    }
    fn active(&self) -> Option<&Input> {
        let focus = self.focus.as_ref().filter(|f| f.is_alive())?;
        self.inputs
            .iter()
            .find(|i| i.enabled && i.object.id().same_client_as(&focus.id()))
    }
    pub fn focus(&mut self, surface: Option<WlSurface>) -> bool {
        if self.focus == surface {
            return false;
        }
        if let Some(old) = &self.focus {
            for i in &mut self.inputs {
                if i.object.id().same_client_as(&old.id()) {
                    i.object.leave(old);
                    i.enabled = false;
                    i.pending = Pending::default();
                }
            }
        }
        self.focus = surface;
        self.generation = self.generation.wrapping_add(1);
        if let Some(focus) = &self.focus {
            for i in &self.inputs {
                if i.object.id().same_client_as(&focus.id()) {
                    i.object.enter(focus);
                }
            }
        }
        true
    }
    pub fn republish(&mut self) {
        self.published = Value::Null;
    }
}

impl App {
    fn ime_window(&self) -> Option<u32> {
        let mut root = self.ime.focus.clone()?;
        while let Some(parent) = get_parent(&root) {
            root = parent;
        }
        self.remote_popups
            .iter()
            .find(|p| *p.surface.wl_surface() == root)
            .map(|p| p.id)
            .or_else(|| {
                self.windows
                    .iter()
                    .find(|w| w.window.wl_surface().is_some_and(|s| *s == root))
                    .map(|w| w.id)
            })
    }
    pub(crate) fn publish_ime(&mut self) {
        let state = if let (Some(input), Some(id)) = (self.ime.active(), self.ime_window()) {
            json!({"type":"ime_state","enabled":true,"id":id,"generation":self.ime.generation,
                "rectangle":input.rectangle,"purpose":input.purpose})
        } else {
            json!({"type":"ime_state","enabled":false,"generation":self.ime.generation})
        };
        if state != self.ime.published {
            self.control(state.clone());
            self.ime.published = state;
        }
    }
    pub(crate) fn ime_message(&mut self, message: &Value) {
        if self
            .seat
            .get_keyboard()
            .unwrap()
            .current_focus()
            .and_then(|s| s.wl_surface().map(|s| s.into_owned()))
            != self.ime.focus
        {
            return;
        }
        if message["generation"].as_u64() != Some(self.ime.generation)
            || message["id"].as_u64() != self.ime_window().map(u64::from)
        {
            return;
        }
        let Some(input) = self.ime.active() else {
            return;
        };
        let commit = message.get("commit").and_then(Value::as_str);
        let preedit = message.get("preedit").and_then(Value::as_str);
        if commit.is_some_and(|s| s.len() > 60 * 1024)
            || preedit.is_some_and(|s| s.len() > 60 * 1024)
        {
            return;
        }
        let begin = message["cursor_begin"].as_i64().unwrap_or(0);
        let end = message["cursor_end"].as_i64().unwrap_or(begin);
        if let Some(text) = preedit {
            if !valid_cursor(text, begin) || !valid_cursor(text, end) {
                return;
            }
        }
        if let Some(text) = commit {
            input.object.commit_string(Some(text.to_string()));
        }
        if let Some(text) = preedit {
            input
                .object
                .preedit_string(Some(text.to_string()), begin as i32, end as i32);
        }
        if commit.is_some() || preedit.is_some() {
            input.object.done(input.serial);
        }
    }
}

fn valid_cursor(text: &str, cursor: i64) -> bool {
    cursor == -1
        || (cursor >= 0 && cursor <= text.len() as i64 && text.is_char_boundary(cursor as usize))
}

impl GlobalDispatch<ZwpTextInputManagerV3, ()> for App {
    fn bind(
        _: &mut Self,
        _: &DisplayHandle,
        _: &Client,
        object: New<ZwpTextInputManagerV3>,
        _: &(),
        init: &mut DataInit<'_, Self>,
    ) {
        init.init(object, ());
    }
}
impl Dispatch<ZwpTextInputManagerV3, ()> for App {
    fn request(
        app: &mut Self,
        _: &Client,
        _: &ZwpTextInputManagerV3,
        request: zwp_text_input_manager_v3::Request,
        _: &(),
        _: &DisplayHandle,
        init: &mut DataInit<'_, Self>,
    ) {
        if let zwp_text_input_manager_v3::Request::GetTextInput { id, .. } = request {
            let object = init.init(id, ());
            if let Some(focus) = &app.ime.focus {
                if object.id().same_client_as(&focus.id()) {
                    object.enter(focus);
                }
            }
            app.ime.inputs.push(Input {
                object,
                serial: 0,
                enabled: false,
                rectangle: [0, 0, 1, 20],
                purpose: 0,
                pending: Pending::default(),
            });
        }
    }
}
impl Dispatch<ZwpTextInputV3, ()> for App {
    fn request(
        app: &mut Self,
        _: &Client,
        object: &ZwpTextInputV3,
        request: zwp_text_input_v3::Request,
        _: &(),
        _: &DisplayHandle,
        _: &mut DataInit<'_, Self>,
    ) {
        let focused = app
            .ime
            .focus
            .as_ref()
            .is_some_and(|s| s.id().same_client_as(&object.id()));
        let Some(input) = app.ime.inputs.iter_mut().find(|i| i.object == *object) else {
            return;
        };
        if matches!(request, zwp_text_input_v3::Request::Commit) {
            input.serial = input.serial.wrapping_add(1);
        }
        if !focused {
            return;
        }
        match request {
            zwp_text_input_v3::Request::Enable => {
                input.pending = Pending {
                    enable: Some(true),
                    ..Pending::default()
                }
            }
            zwp_text_input_v3::Request::Disable => {
                input.pending = Pending {
                    enable: Some(false),
                    ..Pending::default()
                }
            }
            zwp_text_input_v3::Request::SetCursorRectangle {
                x,
                y,
                width,
                height,
            } => {
                input.pending.rectangle = Some([
                    x.clamp(-32768, 32768),
                    y.clamp(-32768, 32768),
                    width.clamp(0, 32768),
                    height.clamp(0, 32768),
                ])
            }
            zwp_text_input_v3::Request::SetContentType { purpose, .. } => {
                input.pending.purpose = Some(purpose.into())
            }
            zwp_text_input_v3::Request::Commit => {
                let pending = std::mem::take(&mut input.pending);
                if let Some(enable) = pending.enable {
                    input.enabled = enable;
                    input.rectangle = [0, 0, 1, 20];
                    input.purpose = 0;
                    app.ime.generation = app.ime.generation.wrapping_add(1);
                }
                if let Some(rectangle) = pending.rectangle {
                    input.rectangle = rectangle;
                }
                if let Some(purpose) = pending.purpose {
                    input.purpose = purpose;
                }
                // There is one text-input owner per focused surface.
                let enabled = input.enabled;
                if enabled {
                    for other in &mut app.ime.inputs {
                        if other.object != *object {
                            other.enabled = false;
                        }
                    }
                }
                app.publish_ime();
            }
            _ => {}
        }
    }
    fn destroyed(app: &mut Self, _: ClientId, object: &ZwpTextInputV3, _: &()) {
        let was_active = app.ime.active().is_some_and(|i| i.object == *object);
        app.ime.inputs.retain(|i| i.object != *object);
        if was_active {
            app.ime.generation = app.ime.generation.wrapping_add(1);
            app.publish_ime();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::valid_cursor;
    #[test]
    fn utf8_preedit_cursor_is_a_byte_boundary() {
        assert!(valid_cursor("你🙂", 3));
        assert!(valid_cursor("你🙂", 7));
        assert!(valid_cursor("你🙂", -1));
        assert!(!valid_cursor("你🙂", 1));
        assert!(!valid_cursor("你🙂", 8));
        assert!(!valid_cursor("你🙂", -2));
    }
}
