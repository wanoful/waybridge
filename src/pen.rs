//! Negotiated tablet-v2 pen input. One proximity owner, bounded tool identities,
//! and focus/mapping generations keep stale strokes outside later leases.
use super::*;
use smithay::{
    backend::input::{TabletToolCapabilities, TabletToolDescriptor, TabletToolType},
    wayland::tablet_manager::{TabletDescriptor, TabletHandle, TabletSeatTrait, TabletToolHandle},
};

struct Active {
    gesture: u64,
    sequence: u64,
    key: (u64, bool),
    tool: TabletToolHandle,
    surface: WlSurface,
    origin: Point<f64, Logical>,
    position: Point<f64, Logical>,
    tip: bool,
    tip_serial: Option<Serial>,
    capabilities: u8,
    barrel: bool,
    tilt: (Option<f64>, Option<f64>),
}
struct Tool {
    handle: TabletToolHandle,
    advertised: u8,
    observed: u8,
}
struct WindowOperation {
    operation: u64,
    root: u32,
    epoch: u64,
    generation: u64,
    gesture: u64,
    key: (u64, bool),
    started: Instant,
}
#[derive(Default)]
pub struct Pen {
    available: bool,
    initialized: bool,
    enabled: bool,
    generation: u64,
    target: Option<(u32, u64)>,
    handoff: Option<u32>,
    tablet: Option<TabletHandle>,
    tools: BTreeMap<(u64, bool), Tool>,
    active: Option<Active>,
    last_gesture: u64,
    window_operation: Option<WindowOperation>,
    window_sequence: u64,
}
fn tablet_descriptor() -> TabletDescriptor {
    TabletDescriptor {
        name: "Waybridge native pen".into(),
        usb_id: None,
        syspath: None,
    }
}
// Capabilities are observed Windows validity-mask bits, frozen per proximity.
// A newly observed axis upgrades the advertised tool only between strokes.
fn tool_descriptor(key: (u64, bool), axes: u8) -> TabletToolDescriptor {
    let mut capabilities = TabletToolCapabilities::empty();
    if axes & 1 != 0 {
        capabilities |= TabletToolCapabilities::PRESSURE;
    }
    if axes & 12 == 12 {
        capabilities |= TabletToolCapabilities::TILT;
    }
    if axes & 2 != 0 {
        capabilities |= TabletToolCapabilities::ROTATION;
    }
    TabletToolDescriptor {
        tool_type: if key.1 {
            TabletToolType::Eraser
        } else {
            TabletToolType::Pen
        },
        hardware_serial: key.0,
        hardware_id_wacom: 0,
        capabilities,
    }
}
impl App {
    pub fn init_pen(&mut self) {
        if self.pen.tablet.is_none() {
            self.pen.tablet = Some(
                self.seat
                    .tablet_seat()
                    .add_tablet::<Self>(&self.dh, &tablet_descriptor()),
            );
        }
    }
    fn publish_pen(&self, pending: Option<u32>) {
        if !self.pen.available {
            return;
        }
        let mut value = json!({"type":"pen_state","enabled":self.pen.target.is_some(),
            "requested":self.pen.enabled,"id":self.pen.target.map_or(0, |(id,_)| id),"generation":self.pen.generation});
        if let Some(id) = pending {
            value["pending_target"] = json!(id);
        }
        if let Some(operation) = &self.pen.window_operation {
            value["window_operation"] = json!(operation.operation);
        }
        self.control(value);
    }
    pub fn pen_negotiate(&mut self, available: bool, enabled: bool) {
        if self.pen.initialized {
            return;
        }
        self.pen.initialized = true;
        self.pen.available = available;
        self.pen.enabled = available && enabled;
        self.pen.generation = self.pen.generation.wrapping_add(1).max(1);
        // Advertise before the first physical event, so applications can bind
        // tablet-seat resources without a proximity-driven detection cycle.
        if available {
            self.init_pen();
        }
        self.publish_pen(None);
        self.sync_pen();
    }
    pub fn pen_available(&mut self, available: bool) {
        self.cancel_pen();
        let seat = self.seat.tablet_seat();
        for (key, tool) in &self.pen.tools {
            seat.remove_tool(&tool_descriptor(*key, tool.advertised));
        }
        self.pen.tools.clear();
        self.pen.available = false;
        self.pen.initialized = false;
        self.pen.enabled = false;
        self.pen.last_gesture = 0;
        if available {
            self.pen_negotiate(true, true);
        }
    }
    pub fn pen_enabled(&mut self, enabled: bool) {
        if !self.pen.available || self.pen.enabled == enabled {
            return;
        }
        self.pen.enabled = enabled;
        if !enabled {
            self.cancel_pen();
        } else {
            self.sync_pen();
            self.publish_pen(None);
        }
    }
    pub fn window_pen_bound(&self, window: &Window) -> bool {
        !window.is_x11()
            && window
                .wl_surface()
                .and_then(|s| s.client())
                .is_some_and(|client| {
                    self.seat
                        .tablet_seat()
                        .client_tablet_seats(&client)
                        .next()
                        .is_some()
                })
    }
    fn pen_candidate(&self, id: u32) -> Option<(u32, u64)> {
        if !self.pen.enabled
            || self.read_only
            || self.peer.as_ref().is_none_or(|p| !p.desktop_input())
            || self.external_drag_input()
            || self.window_drag.active()
            || self.pointer_locked()
            || self.x11.grab.is_some()
            || !self.input_allowed(id)
        {
            return None;
        }
        self.windows
            .iter()
            .find(|w| w.id == id && w.mapped && self.window_pen_bound(&w.window))
            .map(|w| (id, w.state.mapping_epoch))
    }
    pub fn pen_handoff(&mut self, id: Option<u32>) {
        self.pen.handoff = id.and_then(|id| self.pen_candidate(id).map(|(id, _)| id));
    }
    fn end_pen_proximity(&mut self) {
        if let Some(active) = self.pen.active.take() {
            if active.tip {
                active.tool.tip_up(self.time());
            }
            if active.barrel {
                active.tool.button(
                    0x14b,
                    ButtonState::Released,
                    SERIAL_COUNTER.next_serial(),
                    self.time(),
                );
            }
            active.tool.proximity_out(self.time());
        }
    }
    pub fn cancel_pen(&mut self) {
        let pending = self.pen.handoff.take();
        self.end_pen_proximity();
        self.pen.window_operation = None;
        self.pen.target = None;
        self.pen.generation = self.pen.generation.wrapping_add(1).max(1);
        self.publish_pen(pending);
    }
    pub fn sync_pen(&mut self) {
        if !self.pen.available {
            return;
        }
        if let Some(operation) = &self.pen.window_operation {
            if self.focused_window == Some(operation.root)
                && self.pen_candidate(operation.root) == Some((operation.root, operation.epoch))
                && operation.started.elapsed() < Duration::from_secs(120)
            {
                return;
            }
            self.cancel_pen();
        }
        let desired = self.focused_window.and_then(|id| self.pen_candidate(id));
        let dead = self
            .pen
            .active
            .as_ref()
            .is_some_and(|p| !p.surface.is_alive());
        if desired != self.pen.target || dead {
            self.pen.handoff = (!dead).then_some(desired).flatten().map(|(id, _)| id);
            self.cancel_pen();
            self.pen.target = desired;
            self.pen.generation = self.pen.generation.wrapping_add(1).max(1);
            self.publish_pen(None);
        }
    }
    pub fn pen_message(&mut self, value: &Value) {
        self.sync_pen();
        let Some((root, _)) = self.pen.target else {
            return;
        };
        if value["id"].as_u64() != Some(u64::from(root))
            || value["generation"].as_u64() != Some(self.pen.generation)
        {
            return;
        }
        let Some(gesture) = value["gesture"].as_u64().filter(|g| *g != 0) else {
            return;
        };
        let Some(sequence) = value["sequence"].as_u64().filter(|s| *s != 0) else {
            return;
        };
        let Some(tool_id) = value["tool"].as_u64().filter(|id| *id != 0) else {
            return;
        };
        let Some(eraser) = value["eraser"].as_bool() else {
            return;
        };
        let key = (tool_id, eraser);
        let phase = value["phase"].as_str().unwrap_or("");
        if !matches!(phase, "in" | "motion" | "out" | "cancel") {
            return;
        }
        if phase == "in" {
            if self.pen.active.is_some() || gesture <= self.pen.last_gesture {
                return;
            }
        } else if self
            .pen
            .active
            .as_ref()
            .is_none_or(|p| p.gesture != gesture || p.key != key || sequence <= p.sequence)
        {
            return;
        }
        if phase == "out" || phase == "cancel" {
            self.end_pen_proximity();
            if phase == "cancel" {
                self.cancel_pen();
                self.sync_pen();
            }
            return;
        }
        let Some(tip) = value["tip"].as_bool() else {
            return;
        };
        let Some(barrel) = value["barrel"].as_bool() else {
            return;
        };
        let axis = |field: &str, low: f64, high: f64| -> std::result::Result<Option<f64>, ()> {
            match value.get(field) {
                None => Ok(None),
                Some(v) => v
                    .as_f64()
                    .filter(|n| n.is_finite() && *n >= low && *n <= high)
                    .map(Some)
                    .ok_or(()),
            }
        };
        let (Ok(pressure), Ok(tx), Ok(ty), Ok(rotation)) = (
            axis("pressure", 0., 1.),
            axis("tilt_x", -90., 90.),
            axis("tilt_y", -90., 90.),
            axis("rotation", 0., 360.),
        ) else {
            return;
        };
        let inferred = u8::from(pressure.is_some())
            | (u8::from(rotation.is_some()) << 1)
            | (u8::from(tx.is_some()) << 2)
            | (u8::from(ty.is_some()) << 3);
        let axes = match value.get("capabilities") {
            None => inferred,
            Some(value) => {
                let Some(mask) = value.as_u64().filter(|mask| *mask <= 15) else {
                    return;
                };
                mask as u8 | inferred
            }
        };
        let Some(target) = value["target"].as_u64().and_then(|n| u32::try_from(n).ok()) else {
            return;
        };
        if self.root_id(target) != root || !self.input_allowed(target) {
            return;
        }
        let Some(x) = value["x"]
            .as_f64()
            .filter(|x| x.is_finite() && x.abs() <= 16384.)
        else {
            return;
        };
        let Some(y) = value["y"]
            .as_f64()
            .filter(|y| y.is_finite() && y.abs() <= 16384.)
        else {
            return;
        };
        let _ = (x, y);
        let (x, y) = self.message_position(target, value);
        if !x.is_finite() || !y.is_finite() || x.abs() > 32768. || y.abs() > 32768. {
            return;
        }
        let Some(window) = self.window(root) else {
            return;
        };
        let offset = if let Some(popup) = self.remote_popups.iter().find(|p| p.id == target) {
            popup_location(&window, &popup.surface)
                + bbox_from_surface_tree(popup.surface.wl_surface(), (0, 0)).loc
        } else {
            window.geometry().loc
        };
        let position: Point<f64, Logical> =
            (x + f64::from(offset.x), y + f64::from(offset.y)).into();
        let hit = window.surface_under(position, WindowSurfaceType::ALL);
        let focus = if self.pen.active.as_ref().is_some_and(|p| p.tip) {
            self.pen
                .active
                .as_ref()
                .map(|p| (p.surface.clone(), p.origin))
        } else {
            hit.map(|(surface, origin)| (surface, origin.to_f64()))
        };
        let Some((surface, origin)) = focus else {
            self.end_pen_proximity();
            return;
        };
        if phase == "in" {
            let observed = self
                .pen
                .tools
                .get(&key)
                .map_or(axes, |tool| tool.observed | axes);
            if self
                .pen
                .tools
                .get(&key)
                .is_none_or(|tool| tool.advertised != observed)
            {
                if !self.pen.tools.contains_key(&key) && self.pen.tools.len() >= 32 {
                    return;
                }
                let seat = self.seat.tablet_seat();
                let dh = self.dh.clone();
                if let Some(previous) = self.pen.tools.remove(&key) {
                    seat.remove_tool(&tool_descriptor(key, previous.advertised));
                }
                let handle = seat.add_tool(self, &dh, &tool_descriptor(key, observed));
                self.pen.tools.insert(
                    key,
                    Tool {
                        handle,
                        advertised: observed,
                        observed,
                    },
                );
            }
            let tool = self.pen.tools[&key].handle.clone();
            let capabilities = self.pen.tools[&key].advertised;
            self.pen.last_gesture = gesture;
            tool.proximity_in(
                position,
                (surface.clone(), origin),
                self.pen.tablet.as_ref().unwrap(),
                SERIAL_COUNTER.next_serial(),
                self.time(),
            );
            self.pen.active = Some(Active {
                gesture,
                sequence,
                key,
                tool,
                surface: surface.clone(),
                origin,
                position,
                tip: false,
                tip_serial: None,
                capabilities,
                barrel: false,
                tilt: (None, None),
            });
        }
        if let Some(tool) = self.pen.tools.get_mut(&key) {
            tool.observed |= axes;
        }
        let active = self.pen.active.as_mut().unwrap();
        // Hover focus may follow popup surfaces; a held tip keeps its original
        // focus through the final UP instead of accidentally ending a new tool.
        let changed_surface = active.surface != surface;
        if changed_surface && active.barrel {
            active.tool.button(
                0x14b,
                ButtonState::Released,
                SERIAL_COUNTER.next_serial(),
                self.start.elapsed().as_millis() as u32,
            );
            active.barrel = false;
        }
        active.surface = surface.clone();
        active.origin = origin;
        active.position = position;
        active.sequence = sequence;
        if active.capabilities & 1 != 0 {
            if let Some(p) = pressure {
                active.tool.pressure(p);
            }
        }
        if tx.is_some() || ty.is_some() {
            active.tilt = (tx.or(active.tilt.0), ty.or(active.tilt.1));
            if let (Some(x), Some(y)) = active.tilt {
                if active.capabilities & 12 == 12 {
                    active.tool.tilt((x, y));
                }
            }
        }
        if active.capabilities & 2 != 0 {
            if let Some(r) = rotation {
                active.tool.rotation(r % 360.);
            }
        }
        let tool = active.tool.clone();
        let was_tip = active.tip;
        let was_barrel = active.barrel;
        active.tip = tip;
        active.barrel = barrel;
        tool.motion(
            position,
            Some((surface.clone(), origin)),
            self.pen.tablet.as_ref().unwrap(),
            SERIAL_COUNTER.next_serial(),
            self.time(),
        );
        if changed_surface {
            tool.motion(
                position,
                Some((surface, origin)),
                self.pen.tablet.as_ref().unwrap(),
                SERIAL_COUNTER.next_serial(),
                self.time(),
            );
        }
        if tip != was_tip {
            if tip {
                let serial = SERIAL_COUNTER.next_serial();
                self.pen.active.as_mut().unwrap().tip_serial = Some(serial);
                tool.tip_down(serial, self.time());
            } else {
                self.pen.active.as_mut().unwrap().tip_serial = None;
                tool.tip_up(self.time());
            }
        }
        if barrel != was_barrel {
            tool.button(
                0x14b,
                if barrel {
                    ButtonState::Pressed
                } else {
                    ButtonState::Released
                },
                SERIAL_COUNTER.next_serial(),
                self.time(),
            );
        }
    }
}

impl App {
    pub fn pen_window_request(
        &mut self,
        surface: &ToplevelSurface,
        seat: &wl_seat::WlSeat,
        serial: Serial,
        action: &'static str,
        edge: Option<u32>,
    ) -> bool {
        if Seat::<Self>::from_resource(seat).as_ref() != Some(&self.seat)
            || self.pen.window_operation.is_some()
            || !matches!(action, "move" | "resize")
            || (action == "resize"
                && !edge.is_some_and(|edge| matches!(edge, 1 | 2 | 4 | 5 | 6 | 8 | 9 | 10)))
        {
            return false;
        }
        let Some((root, epoch)) = self.pen.target else {
            return false;
        };
        if self.pen_candidate(root) != Some((root, epoch)) || self.focused_window != Some(root) {
            return false;
        }
        let Some(active) = self.pen.active.as_ref() else {
            return false;
        };
        if !active.tip
            || active.tip_serial != Some(serial)
            || !active
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
        self.pen.window_sequence = self.pen.window_sequence.wrapping_add(1).max(1);
        let operation = self.pen.window_sequence;
        let generation = self.pen.generation;
        let gesture = active.gesture;
        let key = active.key;
        self.pen.window_operation = Some(WindowOperation {
            operation,
            root,
            epoch,
            generation,
            gesture,
            key,
            started: Instant::now(),
        });
        // The client validates a still-held physical tip before this retirement
        // marker can preserve its geometry operation. No synthetic mouse grab.
        self.control(json!({"type":"window_action","id":root,"action":action,"edge":edge,
            "input":"pen","pen_operation":operation,"pen_generation":generation,"pen_gesture":gesture,
            "pen_tool":key.0,"pen_eraser":key.1}));
        self.end_pen_proximity();
        self.pen.handoff = None;
        self.pen.target = None;
        self.pen.generation = self.pen.generation.wrapping_add(1).max(1);
        self.publish_pen(None);
        true
    }
    pub fn pen_window_end(&mut self, value: &Value) {
        let Some(operation) = &self.pen.window_operation else {
            return;
        };
        if value["operation"].as_u64() != Some(operation.operation)
            || value["generation"].as_u64() != Some(operation.generation)
            || value["gesture"].as_u64() != Some(operation.gesture)
            || value["tool"].as_u64() != Some(operation.key.0)
            || value["eraser"].as_bool() != Some(operation.key.1)
        {
            return;
        }
        self.pen.window_operation = None;
        self.pen.generation = self.pen.generation.wrapping_add(1).max(1);
        self.publish_pen(None);
        self.sync_pen();
    }
}
