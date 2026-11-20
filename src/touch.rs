//! Negotiated native touch. Every focus/mapping lease has a server generation;
//! contacts retain their initial Wayland surface until up or whole-gesture cancel.
use super::*;
use smithay::{
    backend::input::TouchSlot,
    input::touch::{DownEvent, GrabStartData, MotionEvent as TouchMotionEvent, UpEvent},
};
use std::collections::BTreeMap;

type TouchFocus = Option<(x11::InputFocus, Point<f64, Logical>)>;
type TouchHit = (TouchFocus, Point<f64, Logical>);

#[derive(Clone)]
struct Contact {
    surface: WlSurface,
    origin: Point<f64, Logical>,
    serial: Serial,
    gesture: u64,
    target: u32,
    position: Point<f64, Logical>,
    local: (f64, f64),
}
struct Interaction {
    operation: u64,
    kind: &'static str,
    root: u32,
    generation: u64,
    gesture: u64,
    slot: u32,
    source: Option<u64>,
    pen: Option<PenContact>,
    acknowledged: bool,
    owner_up: bool,
    returning: bool,
    return_focus: Option<x11::InputFocus>,
    return_settled: bool,
    started: Instant,
    ended: Option<bool>,
    deferred_focus: Option<Option<u32>>,
}
struct PenContact {
    epoch: u64,
    tool: u64,
    eraser: bool,
    position: Point<f64, Logical>,
}
impl Interaction {
    fn tag_input(&self, message: &mut Value) {
        if let Some(pen) = &self.pen {
            message["input"] = json!("pen");
            message["pen_tool"] = json!(pen.tool);
            message["pen_eraser"] = json!(pen.eraser);
        }
    }
    fn input_matches(&self, message: &Value) -> bool {
        self.pen.as_ref().map_or_else(
            || message["input"] != "pen",
            |pen| {
                message["input"] == "pen"
                    && message["pen_tool"].as_u64() == Some(pen.tool)
                    && message["pen_eraser"].as_bool() == Some(pen.eraser)
            },
        )
    }
    fn matches_pen_message(&self, message: &Value) -> bool {
        self.input_matches(message)
            && (self.pen.is_none()
                || (message["generation"].as_u64() == Some(self.generation)
                    && message["gesture"].as_u64() == Some(self.gesture)
                    && message["slot"].as_u64() == Some(u64::from(self.slot))))
    }
}
#[derive(Default)]
pub struct Touch {
    available: bool,
    initialized: bool,
    enabled: bool,
    interactions: bool,
    operation_sequence: u64,
    operation: Option<Interaction>,
    resume_focus: Option<Option<u32>>,
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
        self.touch.interactions = false;
        self.touch.resume_focus = None;
        self.touch.generation = self.touch.generation.wrapping_add(1).max(1);
        self.touch.target = None;
        self.publish_touch(None);
        self.sync_touch();
    }
    /// Bind capability/preference to the first hello of this connection. A
    /// subsequent hello cannot upgrade a viewer which did not negotiate touch.
    pub fn touch_negotiate(&mut self, available: bool, enabled: bool, interactions: bool) {
        if self.touch.initialized {
            return;
        }
        self.touch.initialized = true;
        self.touch.available = available;
        self.touch.enabled = available && enabled;
        self.touch.interactions = available && interactions;
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
            if self.pen_drag_operation().is_none() {
                self.cancel_touch();
            }
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
        if self.touch.operation.is_some() {
            self.finish_touch_interaction(true);
            return;
        }
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
    pub(crate) fn window_touch_bound(&self, window: &Window) -> bool {
        !window.is_x11()
            && self.seat.get_touch().is_some_and(|touch| {
                window
                    .wl_surface()
                    .and_then(|surface| surface.client())
                    .is_some_and(|client| touch.client_touches(&client).next().is_some())
            })
    }
    fn touch_candidate(&self, id: u32) -> Option<(u32, u64)> {
        if !self.touch.enabled
            || self.read_only
            || self.peer.as_ref().is_none_or(|p| !p.desktop_input())
            || self.external_drag_input()
            || self.window_drag.active()
            || self.pointer_locked()
            || self.x11.grab.is_some()
            || (!self.touch.interactions
                && self
                    .popup_grab
                    .as_ref()
                    .is_some_and(|(_, grab)| !grab.has_ended()))
            || !self.input_allowed(id)
        {
            return None;
        }
        self.windows
            .iter()
            .find(|w| w.id == id && w.mapped && self.window_touch_bound(&w.window))
            .map(|w| (id, w.state.mapping_epoch))
    }
    pub fn sync_touch(&mut self) {
        if let Some(focus) = self.touch.resume_focus.take() {
            self.focus(focus);
        }
        if let Some(operation) = &self.touch.operation {
            let valid = !self.read_only
                && self.peer.as_ref().is_some_and(|p| p.desktop_input())
                && operation.pen.as_ref().map_or_else(
                    || {
                        self.touch.enabled
                            && self.windows.iter().any(|w| {
                                w.id == operation.root
                                    && w.mapped
                                    && w.window.alive()
                                    && self.window_touch_bound(&w.window)
                            })
                            && self
                                .touch
                                .contacts
                                .get(&operation.slot)
                                .is_some_and(|c| c.gesture == operation.gesture)
                    },
                    |pen| {
                        self.pen_drag_allowed(operation.root, pen.epoch)
                            && self.focused_window == Some(operation.root)
                            && operation.started.elapsed() < Duration::from_secs(120)
                    },
                )
                && (operation.acknowledged || operation.started.elapsed() < Duration::from_secs(2));
            if !valid {
                self.finish_touch_interaction(true);
            } else if let Some(success) = operation.ended {
                self.finish_touch_interaction(!success);
            } else {
                return;
            }
        }
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
        let gesture = value["gesture"].as_u64().unwrap_or(0);
        if self.touch.interactions && gesture == 0 {
            return;
        }
        if self.touch.operation.is_some() {
            for point in points {
                let Some(slot) = point["slot"].as_u64().and_then(|n| u32::try_from(n).ok()) else {
                    return;
                };
                let operation = self.touch.operation.as_ref().unwrap();
                if slot != operation.slot {
                    if point["phase"] == "up" {
                        self.touch.contacts.remove(&slot);
                    }
                    continue;
                }
                let mut input = point.clone();
                input["operation"] = json!(operation.operation);
                input["gesture"] = json!(gesture);
                input["id"] = point
                    .get("target")
                    .or_else(|| value.get("target"))
                    .cloned()
                    .unwrap_or(json!(id));
                input["origin_x"] = value["origin_x"].clone();
                input["origin_y"] = value["origin_y"].clone();
                self.touch_interaction_input(&input);
                if self.touch.operation.is_none() {
                    break;
                }
            }
            return;
        }
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
            let target = point
                .get("target")
                .or_else(|| value.get("target"))
                .and_then(Value::as_u64)
                .and_then(|n| u32::try_from(n).ok())
                .unwrap_or(id);
            if self.root_id(target) != id && !(target == 0 && self.popup_grab.is_some()) {
                return;
            }
            if phase == "down"
                && self.touch.interactions
                && self
                    .popup_grab
                    .as_ref()
                    .is_some_and(|(_, grab)| !grab.has_ended())
                && !self
                    .remote_popups
                    .iter()
                    .any(|p| p.id == target && p.root == id)
            {
                self.dismiss_touch_popups(id);
                return;
            }
            let (x, y) = self.message_position(target, &coordinates);
            if !x.is_finite() || !y.is_finite() || x.abs() > 32768. || y.abs() > 32768. {
                return;
            }
            let Some((hit, location)) = self.touch_hit(target, x, y) else {
                return;
            };
            match phase {
                "down" if !next.contains_key(&slot) && next.len() < 10 => {
                    if self.touch.interactions && next.values().any(|c| c.gesture != gesture) {
                        return;
                    }
                    let Some((surface, origin)) = hit else {
                        return;
                    };
                    let Some(surface) = surface.wl_surface().map(|s| s.into_owned()) else {
                        return;
                    };
                    let contact = Contact {
                        surface,
                        origin,
                        serial: SERIAL_COUNTER.next_serial(),
                        gesture,
                        target,
                        position: location,
                        local: (x, y),
                    };
                    next.insert(slot, contact.clone());
                    events.push((slot, phase, location, contact));
                }
                "motion" if next.contains_key(&slot) => {
                    let contact = next.get_mut(&slot).unwrap();
                    if self.touch.interactions && contact.gesture != gesture {
                        return;
                    }
                    contact.target = target;
                    contact.position = location;
                    contact.local = (x, y);
                    events.push((slot, phase, location, contact.clone()));
                }
                "up" if next.contains_key(&slot) => {
                    if self.touch.interactions && next[&slot].gesture != gesture {
                        return;
                    }
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
                    let serial = contact.serial;
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

impl App {
    fn touch_hit(&self, id: u32, x: f64, y: f64) -> Option<TouchHit> {
        if !x.is_finite() || !y.is_finite() || x.abs() > 32768. || y.abs() > 32768. {
            return None;
        }
        if id == 0 {
            return Some((None, (x, y).into()));
        }
        if !self.input_allowed(id) {
            return Some((None, (x, y).into()));
        }
        let window = self.window(self.root_id(id))?;
        let mut position: Point<f64, Logical> = (x, y).into();
        if let Some(popup) = self.remote_popups.iter().find(|p| p.id == id) {
            if !popup.surface.wl_surface().is_alive() {
                return None;
            }
            position += (popup_location(&window, &popup.surface)
                + bbox_from_surface_tree(popup.surface.wl_surface(), (0, 0)).loc)
                .to_f64();
        } else {
            position += window.geometry().loc.to_f64();
        }
        let hit = window
            .surface_under(position, WindowSurfaceType::ALL)
            .map(|(surface, origin)| (self.pointer_focus(surface), origin.to_f64()));
        Some((hit, position))
    }
    fn dismiss_touch_popups(&mut self, root: u32) {
        if let Some(surface) = self
            .window(root)
            .and_then(|w| w.wl_surface().map(|s| s.into_owned()))
        {
            for (popup, _) in PopupManager::popups_for_surface(&surface).collect::<Vec<_>>() {
                let _ = PopupManager::dismiss_popup(&surface, &popup);
            }
        }
        self.cancel_touch();
    }
    fn touch_serial_owner(&self, serial: Serial) -> Option<(u32, u32, Contact)> {
        if !self.touch.interactions
            || !self.touch.enabled
            || self.read_only
            || self.touch.operation.is_some()
        {
            return None;
        }
        let (root, _) = self.touch.target?;
        let handle = self.seat.get_touch()?;
        if !handle.has_grab(serial) {
            return None;
        }
        let start = handle.grab_start_data()?;
        let slot: i32 = start.slot.into();
        let slot = u32::try_from(slot).ok()?;
        let contact = self.touch.contacts.get(&slot)?;
        if contact.serial != serial || contact.gesture == 0 || !contact.surface.is_alive() {
            return None;
        }
        Some((root, slot, contact.clone()))
    }
    pub(crate) fn touch_popup_serial(&self, root: &WlSurface, serial: Serial) -> bool {
        self.touch_serial_owner(serial)
            .is_some_and(|(id, _, contact)| {
                self.window(id)
                    .and_then(|w| w.wl_surface().map(|s| s.into_owned()))
                    .as_ref()
                    == Some(root)
                    && contact.surface.id().same_client_as(&root.id())
            })
    }
    pub(crate) fn touch_window_action(
        &mut self,
        surface: &ToplevelSurface,
        seat: &wl_seat::WlSeat,
        serial: Serial,
        kind: &'static str,
        edge: Option<u32>,
    ) -> bool {
        if Seat::<Self>::from_resource(seat).as_ref() != Some(&self.seat) {
            return false;
        }
        let Some((root, _, contact)) = self.touch_serial_owner(serial) else {
            return false;
        };
        if !contact
            .surface
            .id()
            .same_client_as(&surface.wl_surface().id())
            || self
                .window(root)
                .and_then(|w| w.wl_surface().map(|s| s.into_owned()))
                .as_ref()
                != Some(surface.wl_surface())
        {
            return false;
        }
        if !self.start_touch_interaction(serial, kind, edge, None) {
            return false;
        }
        // Native move/resize takes over the physical contact, while the ordinary
        // application touch stream receives a real cancel instead of a fake up.
        if let Some(touch) = self.seat.get_touch() {
            touch.cancel(self);
        }
        true
    }
    pub(crate) fn start_touch_interaction(
        &mut self,
        serial: Serial,
        kind: &'static str,
        edge: Option<u32>,
        source: Option<u64>,
    ) -> bool {
        let Some((root, slot, contact)) = self.touch_serial_owner(serial) else {
            return false;
        };
        self.touch.operation_sequence = self.touch.operation_sequence.wrapping_add(1).max(1);
        let operation = self.touch.operation_sequence;
        self.touch.operation = Some(Interaction {
            operation,
            kind,
            root,
            generation: self.touch.generation,
            gesture: contact.gesture,
            slot,
            source,
            pen: None,
            acknowledged: false,
            owner_up: false,
            returning: false,
            return_focus: None,
            return_settled: false,
            started: Instant::now(),
            ended: None,
            deferred_focus: None,
        });
        self.data_drop.point = Some((contact.target, contact.local.0, contact.local.1));
        self.control(json!({"type":"touch_interaction","operation":operation,"kind":kind,
            "id":root,"generation":self.touch.generation,"gesture":contact.gesture,"slot":slot,"edge":edge}));
        true
    }
    pub(crate) fn pen_drag_operation(&self) -> Option<u64> {
        let operation = self.touch.operation.as_ref()?;
        operation.pen.as_ref().map(|_| operation.operation)
    }
    pub(crate) fn start_pen_drag(
        &mut self,
        pen: pen::PenDragStart,
        source: u64,
    ) -> Option<GrabStartData<Self>> {
        if self.touch.operation.is_some() || self.seat.get_touch().is_none() {
            return None;
        }
        self.cancel_touch();
        self.touch.operation_sequence = self.touch.operation_sequence.wrapping_add(1).max(1);
        let operation = self.touch.operation_sequence;
        self.touch.operation = Some(Interaction {
            operation,
            kind: "dnd",
            root: pen.root,
            generation: pen.generation,
            gesture: pen.gesture,
            slot: 0,
            source: Some(source),
            pen: Some(PenContact {
                epoch: pen.epoch,
                tool: pen.tool,
                eraser: pen.eraser,
                position: pen.position,
            }),
            acknowledged: false,
            owner_up: false,
            returning: false,
            return_focus: None,
            return_settled: false,
            started: Instant::now(),
            ended: None,
            deferred_focus: None,
        });
        self.data_drop.point = Some((pen.target, pen.local.0, pen.local.1));
        self.control(
            json!({"type":"touch_interaction","operation":operation,"kind":"dnd","input":"pen",
            "id":pen.root,"generation":pen.generation,"gesture":pen.gesture,"slot":0,
            "pen_tool":pen.tool,"pen_eraser":pen.eraser}),
        );
        self.retire_pen_for_drag();
        // Drive Smithay's contact drag with a private slot. No wl_touch down,
        // wl_pointer button or synthetic client input is sent for the pen.
        Some(GrabStartData {
            focus: Some((pen.surface.into(), pen.origin)),
            slot: TouchSlot::from(Some(0)),
            location: pen.position,
        })
    }
    pub(crate) fn defer_touch_focus(&mut self, id: Option<u32>) -> bool {
        if let Some(operation) = &mut self.touch.operation {
            operation.deferred_focus = Some(id);
            true
        } else {
            false
        }
    }
    pub(crate) fn finish_touch_interaction(&mut self, cancelled: bool) {
        let Some(operation) = self.touch.operation.take() else {
            return;
        };
        let mut end = json!({"type":"touch_interaction","kind":"end","operation":operation.operation,
            "id":operation.root,"generation":operation.generation,"gesture":operation.gesture,
            "slot":operation.slot,"cancelled":cancelled});
        operation.tag_input(&mut end);
        self.control(end);
        self.touch.resume_focus = operation.deferred_focus;
        self.touch.contacts.clear();
        if let Some(touch) = self.seat.get_touch() {
            touch.cancel(self);
        }
        self.touch.target = None;
        self.touch.generation = self.touch.generation.wrapping_add(1).max(1);
        self.publish_touch(None);
    }
    pub fn touch_interaction_ack(&mut self, value: &Value) {
        let Some(operation) = &self.touch.operation else {
            return;
        };
        if value["operation"].as_u64() != Some(operation.operation)
            || !operation.matches_pen_message(value)
        {
            return;
        }
        let held = !operation.owner_up
            && (operation.pen.is_some()
                || self
                    .touch
                    .contacts
                    .get(&operation.slot)
                    .is_some_and(|c| c.gesture == operation.gesture));
        if value["accepted"] != true || !held {
            self.finish_touch_interaction(true);
            return;
        }
        let is_dnd = operation.kind == "dnd";
        self.touch.operation.as_mut().unwrap().acknowledged = true;
        if is_dnd {
            if let Some((id, x, y)) = self.data_drop.point {
                self.touch_drag_motion(id, x, y);
            }
        }
    }
    pub fn touch_interaction_input(&mut self, value: &Value) {
        let Some(operation) = &self.touch.operation else {
            return;
        };
        if value["operation"].as_u64() != Some(operation.operation)
            || value["gesture"].as_u64() != Some(operation.gesture)
            || value["slot"].as_u64() != Some(u64::from(operation.slot))
            || !operation.matches_pen_message(value)
        {
            return;
        }
        if let Some(success) = operation.ended {
            self.finish_touch_interaction(!success);
            return;
        }
        let phase = value["phase"].as_str().unwrap_or("");
        if !matches!(phase, "motion" | "up" | "cancel") {
            return;
        }
        if phase == "cancel" || (!operation.acknowledged && phase == "up") {
            self.finish_touch_interaction(true);
            return;
        }
        if !operation.acknowledged || operation.owner_up {
            return;
        }
        let is_dnd = operation.kind == "dnd";
        let external = self
            .data_drop
            .outbound
            .as_ref()
            .is_some_and(|d| d.external && !d.dropped);
        if is_dnd && !external {
            let Some(id) = value["id"].as_u64().and_then(|n| u32::try_from(n).ok()) else {
                return;
            };
            if value["x"].as_f64().is_none_or(|v| !v.is_finite())
                || value["y"].as_f64().is_none_or(|v| !v.is_finite())
            {
                return;
            }
            let (x, y) = self.message_position(id, value);
            if !self.touch_drag_motion(id, x, y) {
                if phase == "up" {
                    self.finish_touch_interaction(true);
                }
                return;
            }
        }
        if phase == "up" {
            self.touch.operation.as_mut().unwrap().owner_up = true;
            if external
                || self
                    .touch
                    .operation
                    .as_ref()
                    .is_some_and(|op| op.returning && !op.return_settled)
            {
                // OLE completion or the returned target's negotiation releases
                // the original grab. The physical UP is retained exactly once.
                return;
            }
            if is_dnd {
                self.touch_drag_up();
            }
            self.finish_touch_interaction(false);
        }
    }
    pub(crate) fn touch_drag_motion(&mut self, id: u32, x: f64, y: f64) -> bool {
        let Some(operation) = &self.touch.operation else {
            return false;
        };
        if operation.kind != "dnd" || !operation.acknowledged {
            return false;
        }
        let slot = operation.slot;
        let Some((hit, location)) = self.touch_hit(id, x, y) else {
            return false;
        };
        let focus = hit.as_ref().map(|(focus, _)| focus.clone());
        let changed_return = operation.returning && operation.return_focus != focus;
        let source = operation.source;
        if changed_return {
            let operation = self.touch.operation.as_mut().unwrap();
            operation.return_focus = focus.clone();
            operation.return_settled = false;
        }
        self.data_drop.point = Some((id, x, y));
        let Some(touch) = self.seat.get_touch() else {
            return false;
        };
        touch.motion(
            self,
            hit,
            &TouchMotionEvent {
                slot: TouchSlot::from(Some(slot)),
                location,
                time: self.time(),
            },
        );
        touch.frame(self);
        if changed_return && focus.is_some() {
            // Leaving the sink publishes a settled rejection for that old
            // offer. The new target has not dispatched its enter yet, so only
            // its later negotiation may settle this return. Keep terminal
            // source feedback intact; it must still cancel/finish ownership.
            let mut feedback = self.data_drop.feedback.lock().unwrap();
            if let Some(source) = source {
                if feedback
                    .get(&source)
                    .is_some_and(|state| state.finished.is_none())
                {
                    feedback.remove(&source);
                }
            }
        }
        true
    }
    pub(crate) fn touch_claim_sink(&mut self, surface: WlSurface) {
        let Some(operation) = &self.touch.operation else {
            return;
        };
        if operation.kind != "dnd" || !operation.acknowledged {
            return;
        }
        let slot = operation.slot;
        let location = operation
            .pen
            .as_ref()
            .map(|pen| pen.position)
            .or_else(|| self.touch.contacts.get(&slot).map(|c| c.position))
            .unwrap_or((0., 0.).into());
        if let Some(touch) = self.seat.get_touch() {
            touch.motion(
                self,
                Some((surface.into(), location)),
                &TouchMotionEvent {
                    slot: TouchSlot::from(Some(slot)),
                    location,
                    time: self.time(),
                },
            );
            touch.frame(self);
        }
    }
    pub(crate) fn touch_drag_up(&mut self) {
        let Some(operation) = &self.touch.operation else {
            return;
        };
        if operation.kind != "dnd" {
            return;
        }
        let slot = operation.slot;
        if let Some(touch) = self.seat.get_touch() {
            touch.up(
                self,
                &UpEvent {
                    slot: TouchSlot::from(Some(slot)),
                    serial: SERIAL_COUNTER.next_serial(),
                    time: self.time(),
                },
            );
            touch.frame(self);
        }
    }
    pub(crate) fn touch_begin_return(&mut self, source: u64) {
        if let Some(operation) = self
            .touch
            .operation
            .as_mut()
            .filter(|op| op.source == Some(source))
        {
            operation.returning = true;
            operation.return_focus = None;
            operation.return_settled = false;
        }
    }
    pub(crate) fn touch_return_feedback(&mut self, source: u64, settled: bool) {
        let Some(operation) = self.touch.operation.as_mut().filter(|op| {
            op.kind == "dnd" && op.source == Some(source) && op.returning && op.acknowledged
        }) else {
            return;
        };
        operation.return_settled = settled;
        if settled && operation.owner_up {
            // Either order is valid: return -> UP -> settlement, or UP ->
            // return -> settlement. Settlement before UP is retained as well.
            self.touch_drag_up();
            self.finish_touch_interaction(false);
        }
    }
    pub(crate) fn touch_drag_matches(&self, source: u64, message: &Value) -> bool {
        self.touch
            .operation
            .as_ref()
            .filter(|op| op.source == Some(source))
            .is_none_or(|op| {
                if message["type"] == "drag_read" {
                    return true;
                }
                op.input_matches(message)
                    && [
                        ("touch_operation", op.operation),
                        ("touch_generation", op.generation),
                        ("touch_gesture", op.gesture),
                        ("touch_slot", u64::from(op.slot)),
                    ]
                    .into_iter()
                    .all(|(field, expected)| {
                        message
                            .get(field)
                            .map_or(op.pen.is_none(), |value| value.as_u64() == Some(expected))
                    })
            })
    }
    pub(crate) fn touch_source_held(&self, source: u64) -> bool {
        self.touch.operation.as_ref().is_some_and(|op| {
            op.kind == "dnd"
                && op.source == Some(source)
                && !op.owner_up
                && op.ended.is_none()
                && (op.pen.is_some()
                    || self
                        .touch
                        .contacts
                        .get(&op.slot)
                        .is_some_and(|c| c.gesture == op.gesture))
        })
    }
    pub(crate) fn touch_source_active(&self, source: u64) -> bool {
        self.touch
            .operation
            .as_ref()
            .is_some_and(|op| op.kind == "dnd" && op.source == Some(source))
    }
    pub(crate) fn touch_drag_tag(&self, message: &mut Value, source: Option<u64>) {
        if let Some(op) = self
            .touch
            .operation
            .as_ref()
            .filter(|op| op.kind == "dnd" && source.is_none_or(|s| op.source == Some(s)))
        {
            message["input"] = json!("touch");
            message["touch_operation"] = json!(op.operation);
            message["touch_generation"] = json!(op.generation);
            message["touch_gesture"] = json!(op.gesture);
            message["touch_slot"] = json!(op.slot);
            op.tag_input(message);
        }
    }
    pub(crate) fn touch_source_finished(&mut self, source: u64, success: bool) {
        if let Some(operation) = self
            .touch
            .operation
            .as_mut()
            .filter(|op| op.source == Some(source))
        {
            operation.ended = Some(success);
        }
    }
}
