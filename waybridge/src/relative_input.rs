//! Locked-pointer v1. A Wayland lock is activated only after the viewer has
//! acknowledged its native Raw Input/cursor lock. Confinement is never activated.
use super::*;
use smithay::{
    input::pointer::{PointerHandle, RelativeMotionEvent},
    wayland::pointer_constraints::{
        with_pointer_constraint, PointerConstraint, PointerConstraintsHandler,
    },
};

struct Lock {
    id: u32,
    surface: WlSurface,
    generation: u64,
    requested: Instant,
    active: bool,
}

#[derive(Default)]
pub struct RelativeInput {
    available: bool,
    generation: u64,
    lock: Option<Lock>,
    blocked: Option<WlSurface>,
    pub hit: Option<(WlSurface, Point<f64, Logical>)>,
}

impl App {
    pub fn relative_focus_changed(&mut self) {
        self.relative.blocked = None;
    }

    pub fn relative_available(&mut self, available: bool) {
        self.unlock_pointer(false);
        self.relative.available = available;
        self.relative.blocked = None;
        self.relative.hit = None;
    }

    pub fn unlock_pointer(&mut self, block: bool) {
        let Some(lock) = self.relative.lock.take() else {
            return;
        };
        let pointer = self.seat.get_pointer().unwrap();
        // End an implicit button grab before leaving the locked surface; otherwise
        // ClickGrab would retain focus and turn the leave into absolute motion.
        for button in std::mem::take(&mut self.pressed_buttons) {
            pointer.button(
                self,
                &ButtonEvent {
                    button,
                    state: ButtonState::Released,
                    serial: SERIAL_COUNTER.next_serial(),
                    time: self.time(),
                },
            );
        }
        pointer.frame(self);
        if lock.active
            && lock.surface.is_alive()
            && pointer.current_focus().as_ref() == Some(&lock.surface)
        {
            // WlSurface::leave deactivates the constraint in Smithay. Let that
            // path send the sole unlocked event instead of deactivating twice.
            // A newly recreated inactive constraint must not be deactivated.
            let leave = with_pointer_constraint(&lock.surface, &pointer, |constraint| {
                constraint.is_none_or(|constraint| constraint.is_active())
            });
            if leave {
                pointer.motion(
                    self,
                    None,
                    &MotionEvent {
                        location: pointer.current_location(),
                        serial: SERIAL_COUNTER.next_serial(),
                        time: self.time(),
                    },
                );
                pointer.frame(self);
            }
        }
        if block {
            self.relative.blocked = Some(lock.surface);
        }
        self.control(json!({"type":"pointer_lock","id":lock.id,
            "generation":lock.generation,"active":false}));
    }

    pub fn relative_hit(&mut self, hit: Option<(WlSurface, Point<f64, Logical>)>) {
        if self.relative.blocked.as_ref() != hit.as_ref().map(|(surface, _)| surface) {
            self.relative.blocked = None;
        }
        self.relative.hit = hit;
    }

    pub fn pointer_locked(&self) -> bool {
        self.relative.lock.is_some()
    }

    // Called after dispatch and absolute motion, so destruction, region commits,
    // unmapping and changes made without a viewer message also release the lock.
    pub fn sync_pointer_lock(&mut self) {
        let pointer = self.seat.get_pointer().unwrap();
        let focus = pointer.current_focus();
        if focus
            .as_ref()
            .is_some_and(|focus| self.relative.blocked.as_ref() != Some(focus))
        {
            self.relative.blocked = None;
        }
        let candidate = self.relative.hit.as_ref().and_then(|(surface, _)| {
            let id = self.pointer_window?;
            let root = self.root_id(id);
            if !self.relative.available
                || self.peer.is_none()
                || self.outbound.is_some()
                || self.window_drag.active()
                || id != root
                || self.focused_window != Some(root)
                || !surface.is_alive()
                || focus.as_ref() != Some(surface)
                || self.window(root).is_none()
                || self.windows.iter().any(|w| w.id == root && !w.mapped)
                || self
                    .popup_grab
                    .as_ref()
                    .is_some_and(|(_, grab)| !grab.has_ended())
            {
                return None;
            }
            let window = self.window(root)?;
            let (hit, origin) =
                window.surface_under(pointer.current_location(), WindowSurfaceType::ALL)?;
            if &hit != surface {
                return None;
            }
            let local = pointer.current_location() - origin.to_f64();
            let active = with_pointer_constraint(surface, &pointer, |constraint| {
                constraint.and_then(|constraint| {
                    if !matches!(&*constraint, PointerConstraint::Locked(_))
                        || constraint
                            .region()
                            .is_some_and(|r| !r.contains(local.to_i32_floor()))
                    {
                        return None;
                    }
                    Some(constraint.is_active())
                })
            })?;
            Some((root, surface.clone(), active))
        });
        if let Some(lock) = &self.relative.lock {
            let valid = candidate.as_ref().is_some_and(|(id, surface, active)| {
                *id == lock.id && *surface == lock.surface && *active == lock.active
            }) && (lock.active || lock.requested.elapsed() < Duration::from_secs(2));
            if !valid {
                self.unlock_pointer(true);
            }
        }
        if self.relative.lock.is_none() {
            if let Some((id, surface, false)) = candidate {
                if self.relative.blocked.as_ref() == Some(&surface) {
                    return;
                }
                self.relative.generation = self.relative.generation.wrapping_add(1).max(1);
                let generation = self.relative.generation;
                self.relative.lock = Some(Lock {
                    id,
                    surface,
                    generation,
                    requested: Instant::now(),
                    active: false,
                });
                self.control(json!({"type":"pointer_lock","id":id,
                    "generation":generation,"active":true}));
            }
        }
    }

    pub fn pointer_lock_ack(&mut self, id: u32, value: &Value) {
        self.sync_pointer_lock();
        let Some(lock) = &self.relative.lock else {
            return;
        };
        if lock.id != id || value["generation"].as_u64() != Some(lock.generation) {
            return;
        }
        if value["active"].as_bool() != Some(true) {
            self.release_input();
            return;
        }
        if lock.active {
            return;
        }
        let surface = lock.surface.clone();
        let pointer = self.seat.get_pointer().unwrap();
        let activated = with_pointer_constraint(&surface, &pointer, |constraint| {
            if let Some(constraint) =
                constraint.filter(|c| matches!(&**c, PointerConstraint::Locked(_)))
            {
                constraint.activate();
                true
            } else {
                false
            }
        });
        if activated {
            self.relative.lock.as_mut().unwrap().active = true;
        } else {
            self.unlock_pointer(true);
        }
    }

    pub fn relative_motion(&mut self, id: u32, value: &Value) {
        self.sync_pointer_lock();
        let Some(lock) = &self.relative.lock else {
            return;
        };
        if !lock.active || id != lock.id || value["generation"].as_u64() != Some(lock.generation) {
            return;
        }
        let (Some(dx), Some(dy)) = (value["dx"].as_f64(), value["dy"].as_f64()) else {
            return;
        };
        if !dx.is_finite() || !dy.is_finite() || dx.abs() > 1_000_000.0 || dy.abs() > 1_000_000.0 {
            return;
        }
        let pointer = self.seat.get_pointer().unwrap();
        let focus = self.relative.hit.clone();
        let utime = self.start.elapsed().as_micros().min(u64::MAX as u128) as u64;
        // Raw Input mouse counts are passed through without Windows acceleration.
        pointer.relative_motion(
            self,
            focus,
            &RelativeMotionEvent {
                delta: (dx, dy).into(),
                delta_unaccel: (dx, dy).into(),
                utime,
            },
        );
        pointer.frame(self);
    }
}

impl PointerConstraintsHandler for App {
    fn new_constraint(&mut self, surface: &WlSurface, _: &PointerHandle<Self>) {
        if self
            .relative
            .lock
            .as_ref()
            .is_some_and(|lock| &lock.surface == surface)
        {
            self.unlock_pointer(false);
        }
        if self.relative.blocked.as_ref() == Some(surface) {
            self.relative.blocked = None;
        }
        self.sync_pointer_lock();
    }

    fn cursor_position_hint(
        &mut self,
        _: &WlSurface,
        _: &PointerHandle<Self>,
        _: Point<f64, Logical>,
    ) {
        // A hint is optional. Leave the absolute pointer at its last real position.
    }
}
