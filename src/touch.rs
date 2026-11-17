//! Negotiated native touch. Every focus/mapping lease has a server generation;
//! contacts retain their initial Wayland surface until up or whole-gesture cancel.
use super::*;
use smithay::{
    backend::input::TouchSlot,
    input::touch::{DownEvent, MotionEvent as TouchMotionEvent, UpEvent},
};
use std::collections::BTreeMap;

#[derive(Clone)]
struct Contact {
    surface: WlSurface,
    origin: Point<f64, Logical>,
}
#[derive(Default)]
pub struct Touch {
    available: bool,
    initialized: bool,
    enabled: bool,
    generation: u64,
    target: Option<(u32, u64)>,
    handoff: Option<u32>,
    contacts: BTreeMap<u32, Contact>,
}

impl App {
    fn publish_touch(&self, pending_target: Option<u32>) {
        if self.touch.available {
            let mut message = json!({"type":"touch_state", "enabled":self.touch.target.is_some(),
                "id":self.touch.target.map_or(0, |(id, _)| id),
                "generation":self.touch.generation, "requested":self.touch.enabled, "max_contacts":10});
            if let Some(id) = pending_target {
                message["pending_target"] = json!(id);
            }
            self.control(message);
        }
    }
    pub fn touch_available(&mut self, available: bool) {
        self.cancel_touch();
        self.touch.available = available;
        self.touch.initialized = available;
        self.touch.enabled = available;
        self.touch.generation = self.touch.generation.wrapping_add(1).max(1);
        self.touch.target = None;
        self.publish_touch(None);
        self.sync_touch();
    }
    /// Bind capability/preference to the first hello of this connection. A
    /// subsequent hello cannot upgrade a viewer which did not negotiate touch.
    pub fn touch_negotiate(&mut self, available: bool, enabled: bool) {
        if self.touch.initialized {
            return;
        }
        self.touch.initialized = true;
        self.touch.available = available;
        self.touch.enabled = available && enabled;
        self.touch.target = None;
        self.touch.generation = self.touch.generation.wrapping_add(1).max(1);
        self.publish_touch(None);
        self.sync_touch();
    }
    pub fn touch_enabled(&mut self, enabled: bool) {
        if !self.touch.initialized || !self.touch.available || self.touch.enabled == enabled {
            return;
        }
        self.touch.enabled = enabled;
        if !enabled {
            self.cancel_touch();
        } else {
            let before = self.touch.generation;
            self.sync_touch();
            if self.touch.generation == before {
                self.publish_touch(None);
            }
        }
    }
    pub fn cancel_touch(&mut self) {
        let pending_target = self.touch.handoff.take();
        if !self.touch.available {
            return;
        }
        if !self.touch.contacts.is_empty() {
            self.touch.contacts.clear();
            if let Some(touch) = self.seat.get_touch() {
                touch.cancel(self);
            }
        }
        self.touch.target = None;
        self.touch.generation = self.touch.generation.wrapping_add(1).max(1);
        self.publish_touch(pending_target);
    }
    /// Only focus() prepares this single-use marker. Read-only, input cancel,
    /// disconnect and other policy changes call cancel_touch without a handoff.
    pub fn touch_handoff(&mut self, id: Option<u32>) {
        self.touch.handoff = id.and_then(|id| self.touch_candidate(id).map(|(id, _)| id));
    }
    fn touch_candidate(&self, id: u32) -> Option<(u32, u64)> {
        if !self.touch.enabled
            || self.read_only
            || self.peer.as_ref().is_none_or(|p| !p.desktop_input())
            || self.external_drag_input()
            || self.window_drag.active()
            || self.pointer_locked()
            || self.x11.grab.is_some()
            || self
                .popup_grab
                .as_ref()
                .is_some_and(|(_, grab)| !grab.has_ended())
            || !self.input_allowed(id)
        {
            return None;
        }
        self.windows
            .iter()
            .find(|w| w.id == id && w.mapped && !w.window.is_x11())
            .map(|w| (id, w.state.mapping_epoch))
    }
    pub fn sync_touch(&mut self) {
        if !self.touch.available {
            return;
        }
        let desired = self.focused_window.and_then(|id| self.touch_candidate(id));
        let dead = self.touch.contacts.values().any(|c| !c.surface.is_alive());
        if desired != self.touch.target || dead {
            // A marked transition can retain only a not-yet-delivered first
            // gesture for that exact new target. Actual revocations fail closed.
            self.touch.handoff = (!dead).then_some(desired).flatten().map(|(id, _)| id);
            self.cancel_touch();
            self.touch.target = desired;
            self.touch.generation = self.touch.generation.wrapping_add(1).max(1);
            self.publish_touch(None);
        }
    }
    pub fn touch_message(&mut self, value: &Value) {
        self.sync_touch();
        let Some((id, _)) = self.touch.target else {
            return;
        };
        if !self.touch.available
            || value["id"].as_u64() != Some(u64::from(id))
            || value["generation"].as_u64() != Some(self.touch.generation)
        {
            return;
        }
        if value["action"] == "cancel" {
            self.cancel_touch();
            self.sync_touch();
            return;
        }
        let Some(points) = value["points"]
            .as_array()
            .filter(|p| !p.is_empty() && p.len() <= 10)
        else {
            return;
        };
        let Some(window) = self.window(id) else {
            return;
        };
        let mut next = self.touch.contacts.clone();
        let mut slots = HashSet::new();
        let mut events = Vec::new();
        for point in points {
            let Some(slot) = point["slot"].as_u64().filter(|s| *s < 10).map(|s| s as u32) else {
                return;
            };
            if !slots.insert(slot) {
                return;
            }
            let Some(phase) = point["phase"].as_str() else {
                return;
            };
            let mut coordinates = point.clone();
            coordinates["origin_x"] = value["origin_x"].clone();
            coordinates["origin_y"] = value["origin_y"].clone();
            let x = point["x"].as_f64();
            let y = point["y"].as_f64();
            if !matches!(phase, "up")
                && (x.is_none_or(|x| !x.is_finite() || x.abs() > 16384.)
                    || y.is_none_or(|y| !y.is_finite() || y.abs() > 16384.))
            {
                return;
            }
            let (x, y) = self.message_position(id, &coordinates);
            if !x.is_finite() || !y.is_finite() || x.abs() > 32768. || y.abs() > 32768. {
                return;
            }
            let location = Point::from((x, y)) + window.geometry().loc.to_f64();
            match phase {
                "down" if !next.contains_key(&slot) && next.len() < 10 => {
                    let Some((surface, origin)) =
                        window.surface_under(location, WindowSurfaceType::ALL)
                    else {
                        return;
                    };
                    let contact = Contact {
                        surface,
                        origin: origin.to_f64(),
                    };
                    next.insert(slot, contact.clone());
                    events.push((slot, phase, location, contact));
                }
                "motion" if next.contains_key(&slot) => {
                    events.push((slot, phase, location, next[&slot].clone()));
                }
                "up" if next.contains_key(&slot) => {
                    events.push((slot, phase, location, next.remove(&slot).unwrap()));
                }
                _ => return,
            }
        }
        let Some(touch) = self.seat.get_touch() else {
            return;
        };
        self.touch.contacts = next;
        for (slot, phase, location, contact) in events {
            let slot = TouchSlot::from(Some(slot));
            match phase {
                "down" => {
                    let serial = SERIAL_COUNTER.next_serial();
                    self.last_interaction = Some((serial, id, Instant::now()));
                    touch.down(
                        self,
                        Some((contact.surface.into(), contact.origin)),
                        &DownEvent {
                            slot,
                            location,
                            serial,
                            time: self.time(),
                        },
                    );
                }
                "motion" => touch.motion(
                    self,
                    Some((contact.surface.into(), contact.origin)),
                    &TouchMotionEvent {
                        slot,
                        location,
                        time: self.time(),
                    },
                ),
                "up" => touch.up(
                    self,
                    &UpEvent {
                        slot,
                        serial: SERIAL_COUNTER.next_serial(),
                        time: self.time(),
                    },
                ),
                _ => unreachable!(),
            }
        }
        touch.frame(self);
    }
}
