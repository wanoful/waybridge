//! Remote Precision Touchpad gestures share pointer focus, with their own
//! gesture lifetime. No contacts are sent through the touchscreen seat.
use crate::{x11::InputFocus, App};
use serde_json::Value;
use smithay::{
    backend::input::{Axis, AxisSource},
    input::pointer::{
        AxisFrame, GesturePinchBeginEvent, GesturePinchEndEvent, GesturePinchUpdateEvent,
    },
    utils::{IsAlive, SERIAL_COUNTER},
};

#[derive(Clone, Copy, PartialEq, Eq)]
enum Kind {
    Scroll,
    Pinch,
}

struct Gesture {
    token: u64,
    id: u32,
    focus: InputFocus,
    kind: Kind,
    horizontal: bool,
    vertical: bool,
}

#[derive(Default)]
pub(crate) struct Touchpad {
    initialized: bool,
    available: bool,
    latest: u64,
    active: Option<Gesture>,
}

impl Touchpad {
    pub(crate) fn negotiate(&mut self, available: bool) {
        if !self.initialized {
            self.initialized = true;
            self.available = available;
        }
    }
}

impl App {
    fn touchpad_allowed(&self, id: u32) -> bool {
        self.touchpad.available
            && !self.read_only
            && !self.pointer_locked()
            && !self.seat.get_pointer().unwrap().is_grabbed()
            && !self.external_drag_input()
            && !self.window_drag.active()
            && self.focused_window == Some(id)
            && self.pointer_window == Some(id)
            && self.input_allowed(id)
    }

    pub(crate) fn sync_touchpad(&mut self) {
        if self
            .touchpad
            .active
            .as_ref()
            .is_some_and(|g| !self.touchpad_allowed(g.id))
        {
            self.cancel_touchpad();
        }
    }

    pub(crate) fn touchpad_focus(&mut self, next: Option<&InputFocus>) {
        if self
            .touchpad
            .active
            .as_ref()
            .is_some_and(|g| Some(&g.focus) != next)
        {
            self.cancel_touchpad();
        }
    }

    pub(crate) fn cancel_touchpad(&mut self) {
        self.finish_touchpad(true);
    }

    fn finish_touchpad(&mut self, cancelled: bool) {
        let Some(gesture) = self.touchpad.active.take() else {
            return;
        };
        let pointer = self.seat.get_pointer().unwrap();
        // The pointer's own focus-leave sends a pinch cancel. Never end a
        // gesture on a different surface if focus already changed.
        if pointer.current_focus().as_ref() != Some(&gesture.focus) || !gesture.focus.alive() {
            return;
        }
        let time = self.time();
        match gesture.kind {
            Kind::Pinch => pointer.gesture_pinch_end(
                self,
                &GesturePinchEndEvent {
                    serial: SERIAL_COUNTER.next_serial(),
                    time,
                    cancelled,
                },
            ),
            Kind::Scroll if gesture.horizontal || gesture.vertical => {
                let mut frame = AxisFrame::new(time).source(AxisSource::Finger);
                if gesture.horizontal {
                    frame = frame.stop(Axis::Horizontal);
                }
                if gesture.vertical {
                    frame = frame.stop(Axis::Vertical);
                }
                pointer.axis(self, frame);
                pointer.frame(self);
            }
            Kind::Scroll => {}
        }
    }

    pub(crate) fn touchpad_message(&mut self, id: u32, message: &Value) {
        let Some(token) = message["gesture"].as_u64().filter(|token| *token > 0) else {
            return;
        };
        let phase = message["phase"].as_str().unwrap_or_default();
        let pointer = self.seat.get_pointer().unwrap();
        let allowed = self.touchpad_allowed(id);
        if phase == "begin" {
            if !allowed || token <= self.touchpad.latest {
                return;
            }
            let Some(focus) = pointer.current_focus().filter(IsAlive::alive) else {
                return;
            };
            let kind = match message["kind"].as_str() {
                Some("scroll") => Kind::Scroll,
                Some("pinch") => Kind::Pinch,
                _ => return,
            };
            self.cancel_touchpad();
            self.touchpad.latest = token;
            self.touchpad.active = Some(Gesture {
                token,
                id,
                focus,
                kind,
                horizontal: false,
                vertical: false,
            });
            if kind == Kind::Pinch {
                pointer.gesture_pinch_begin(
                    self,
                    &GesturePinchBeginEvent {
                        serial: SERIAL_COUNTER.next_serial(),
                        time: self.time(),
                        fingers: 2,
                    },
                );
            }
            return;
        }
        let Some(gesture) = self
            .touchpad
            .active
            .as_mut()
            .filter(|g| g.token == token && g.id == id)
        else {
            return;
        };
        if !allowed || pointer.current_focus().as_ref() != Some(&gesture.focus) {
            self.cancel_touchpad();
            return;
        }
        match phase {
            "end" | "cancel" => self.finish_touchpad(phase == "cancel"),
            "update" if gesture.kind == Kind::Pinch => {
                let Some(scale) = message["scale"]
                    .as_f64()
                    .filter(|s| (0.01..=100.).contains(s))
                else {
                    return;
                };
                pointer.gesture_pinch_update(
                    self,
                    &GesturePinchUpdateEvent {
                        time: self.time(),
                        delta: (0.0, 0.0).into(),
                        scale,
                        rotation: 0.0,
                    },
                );
            }
            "update" => {
                let (Some(dx), Some(dy)) = (message["dx"].as_f64(), message["dy"].as_f64()) else {
                    return;
                };
                if !(-1000.0..=1000.0).contains(&dx) || !(-1000.0..=1000.0).contains(&dy) {
                    return;
                }
                gesture.horizontal |= dx != 0.0;
                gesture.vertical |= dy != 0.0;
                let frame = AxisFrame::new(self.time())
                    .source(AxisSource::Finger)
                    .value(Axis::Horizontal, dx)
                    .value(Axis::Vertical, dy);
                pointer.axis(self, frame);
                pointer.frame(self);
            }
            _ => {}
        }
    }
}
