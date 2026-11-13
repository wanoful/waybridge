//! Negotiated native window state; input-region transport stays separate from visual alpha.
use super::*;
use smithay::{
    backend::renderer::utils::RendererSurfaceStateUserData,
    wayland::{
        compositor::{RectangleKind, SurfaceAttributes},
        shell::xdg::SurfaceCachedState,
    },
};

pub(crate) struct State {
    pub fullscreen: bool,
    pub fullscreen_epoch: u64,
    pub visible: bool,
    pub minimized: bool,
    pub mapping_epoch: u64,
    pub input: Value,
}
impl Default for State {
    fn default() -> Self {
        Self {
            fullscreen: false,
            fullscreen_epoch: 0,
            visible: true,
            minimized: false,
            mapping_epoch: 1,
            input: Value::Null,
        }
    }
}
impl State {
    pub fn fullscreen(&mut self, value: bool) {
        if self.fullscreen != value {
            self.fullscreen = value;
            self.fullscreen_epoch += 1;
        }
    }
}

pub(crate) fn limits(surface: &WlSurface) -> ([i32; 2], [i32; 2]) {
    with_states(surface, |states| {
        let mut cache = states.cached_state.get::<SurfaceCachedState>();
        let current = cache.current();
        let min = [
            current.min_size.w.clamp(0, 8192),
            current.min_size.h.clamp(0, 8192),
        ];
        let max = [
            current.max_size.w.clamp(0, 8192),
            current.max_size.h.clamp(0, 8192),
        ];
        (
            min,
            [
                if max[0] == 0 { 0 } else { max[0].max(min[0]) },
                if max[1] == 0 { 0 } else { max[1].max(min[1]) },
            ],
        )
    })
}
fn axis(value: i32, min: i32, max: i32) -> i32 {
    value.max(min.max(1)).min(if max == 0 {
        8192
    } else {
        max.max(min).clamp(1, 8192)
    })
}

pub(crate) fn input_region(surface: &WlSurface, bbox: Rectangle<i32, Logical>) -> Value {
    let layers = std::cell::RefCell::new(Vec::new());
    let valid = std::cell::Cell::new(true);
    let operations = std::cell::Cell::new(0usize);
    with_surface_tree_downward(
        surface,
        Point::<i32, Logical>::from((0, 0)),
        |_, states, location| {
            let view = states
                .data_map
                .get::<RendererSurfaceStateUserData>()
                .and_then(|s| s.lock().unwrap().view());
            if let Some(view) = view {
                if let (Some(x), Some(y)) = (
                    location.x.checked_add(view.offset.x),
                    location.y.checked_add(view.offset.y),
                ) {
                    return TraversalAction::DoChildren((x, y).into());
                }
                valid.set(false);
            }
            TraversalAction::SkipChildren
        },
        |_, states, location| {
            if !valid.get() {
                return;
            }
            let Some(view) = states
                .data_map
                .get::<RendererSurfaceStateUserData>()
                .and_then(|s| s.lock().unwrap().view())
            else {
                return;
            };
            let (Some(x), Some(y)) = (
                location.x.checked_add(view.offset.x),
                location.y.checked_add(view.offset.y),
            ) else {
                valid.set(false);
                return;
            };
            if layers.borrow().len() >= 64 {
                valid.set(false);
                return;
            }
            let mut cache = states.cached_state.get::<SurfaceAttributes>();
            let region = cache.current().input_region.as_ref().map(|region| {
                operations.set(operations.get().saturating_add(region.rects.len()));
                if operations.get() > 256 {
                    valid.set(false);
                    return Vec::new();
                }
                region
                    .rects
                    .iter()
                    .filter_map(|(kind, rect)| {
                        rect.intersection(Rectangle::from_size(view.dst))
                            .map(|rect| {
                                json!([
                                    matches!(kind, RectangleKind::Add),
                                    rect.loc.x,
                                    rect.loc.y,
                                    rect.size.w,
                                    rect.size.h
                                ])
                            })
                    })
                    .collect::<Vec<_>>()
            });
            layers
                .borrow_mut()
                .push(json!({"x":x,"y":y,"width":view.dst.w,"height":view.dst.h,"rects":region}));
        },
        |_, _, _| valid.get(),
    );
    if !valid.get() {
        return Value::Null;
    }
    json!({"origin_x":bbox.loc.x,"origin_y":bbox.loc.y,"width":bbox.size.w,"height":bbox.size.h,"layers":layers.into_inner()})
}

impl App {
    pub(crate) fn semantics_enabled(&self) -> bool {
        self.peer.as_ref().is_some_and(|p| p.semantics_enabled())
    }
    pub(crate) fn modal_target(&self, id: u32) -> u32 {
        let mut id = self.root_id(id);
        for _ in 0..self.windows.len() {
            let Some(parent) = self
                .window(id)
                .and_then(|w| w.wl_surface().map(|s| s.into_owned()))
            else {
                break;
            };
            let modal = self.windows.iter().rev().find(|w| {
                w.mapped
                    && (w.window.toplevel().is_some_and(|t| {
                        t.parent().as_ref() == Some(&parent)
                            && with_states(t.wl_surface(), |s| {
                                s.data_map
                                    .get::<XdgToplevelSurfaceData>()
                                    .unwrap()
                                    .lock()
                                    .unwrap()
                                    .modal
                            })
                    }) || w.window.x11_surface().is_some_and(|x| {
                        x.is_popup()
                            && x.is_transient_for().is_some_and(|xid| {
                                self.window(id)
                                    .and_then(|p| p.x11_surface().map(|p| p.window_id()))
                                    == Some(xid)
                            })
                    }))
            });
            let Some(modal) = modal else { break };
            id = modal.id;
        }
        id
    }
    pub(crate) fn input_allowed(&self, id: u32) -> bool {
        self.modal_target(id) == self.root_id(id)
            && self
                .windows
                .iter()
                .find(|w| w.id == self.root_id(id))
                .is_some_and(|w| w.mapped && (!self.semantics_enabled() || w.state.visible))
    }
    pub(crate) fn fullscreen_request(&mut self, surface: &ToplevelSurface, value: bool) {
        let Some(remote) = self
            .windows
            .iter_mut()
            .find(|w| w.window.wl_surface().as_deref() == Some(surface.wl_surface()))
        else {
            return;
        };
        remote.state.fullscreen(value);
        let message = json!({"type":"window_action","id":remote.id,"action":if value {"fullscreen"} else {"unfullscreen"},"fullscreen_epoch":remote.state.fullscreen_epoch});
        if self.semantics_enabled() {
            self.control(message);
        } else {
            surface.send_pending_configure();
        }
    }
    pub(crate) fn clear_fullscreen_request(&mut self, surface: &ToplevelSurface) {
        if let Some(remote) = self
            .windows
            .iter_mut()
            .find(|w| w.window.wl_surface().as_deref() == Some(surface.wl_surface()))
        {
            remote.state.fullscreen(false);
        }
    }
    pub(crate) fn reparent_children(&self, surface: &WlSurface, parent: Option<WlSurface>) {
        let parent = parent.filter(|p| {
            self.windows.iter().any(|w| {
                w.mapped && w.window.alive() && w.window.wl_surface().as_deref() == Some(p)
            })
        });
        for remote in &self.windows {
            let Some(top) = remote.window.toplevel() else {
                continue;
            };
            if top.parent().as_ref() == Some(surface) {
                with_states(top.wl_surface(), |s| {
                    s.data_map
                        .get::<XdgToplevelSurfaceData>()
                        .unwrap()
                        .lock()
                        .unwrap()
                        .parent = parent.clone()
                });
            }
        }
    }
    pub(crate) fn unmap_toplevel(&mut self, id: u32) {
        let Some(window) = self.window(id) else {
            return;
        };
        let Some(top) = window.toplevel() else { return };
        let surface = top.wl_surface().clone();
        self.window_drag.unmap(&surface);
        self.reparent_children(&surface, top.parent());
        with_states(&surface, |states| {
            let mut role = states
                .data_map
                .get::<XdgToplevelSurfaceData>()
                .unwrap()
                .lock()
                .unwrap();
            role.parent = None;
            role.modal = false;
            role.title = None;
            role.app_id = None;
            let mut cache = states.cached_state.get::<SurfaceCachedState>();
            *cache.current() = SurfaceCachedState::default();
            *cache.pending() = SurfaceCachedState::default();
        });
        top.with_pending_state(|s| {
            s.states = Default::default();
        });
        if self.focused_window == Some(id) {
            self.focus(None);
        }
        if self.pointer_window == Some(id) && !self.window_drag.active() {
            self.pointer_window = None;
            let pointer = self.seat.get_pointer().unwrap();
            pointer.unset_grab(self, SERIAL_COUNTER.next_serial(), self.time());
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
    pub(crate) fn native_window_state(&mut self, id: u32, value: &Value) {
        if !self.semantics_enabled() {
            return;
        }
        let Some(remote) = self
            .windows
            .iter_mut()
            .find(|w| w.id == id && value["mapping_epoch"].as_u64() == Some(w.state.mapping_epoch))
        else {
            return;
        };
        let Some(visible) = value["visible"].as_bool() else {
            return;
        };
        let Some(minimized) = value["minimized"].as_bool() else {
            return;
        };
        let visible = visible && !minimized;
        remote.state.visible = visible;
        remote.state.minimized = minimized;
        remote.dirty = true;
        if !remote.mapped {
            return;
        }
        if let Some(top) = remote.window.toplevel() {
            top.with_pending_state(|s| {
                if visible {
                    s.states.unset(xdg_toplevel::State::Suspended);
                } else {
                    s.states.set(xdg_toplevel::State::Suspended);
                }
            });
            top.send_pending_configure();
        } else if let Some(x) = remote.window.x11_surface() {
            let _ = x.set_suspended(!visible);
        }
        if !visible && self.focused_window == Some(id) {
            self.focus(None);
        }
    }
    pub(crate) fn native_resize(&mut self, id: u32, value: &Value) -> Option<(i32, i32)> {
        let index = self.windows.iter().position(|w| w.id == id)?;
        let top = self.windows[index].window.toplevel().cloned();
        let x11 = self.windows[index].window.x11_surface().cloned();
        let mut size = (
            value["width"].as_i64().unwrap_or(960).clamp(1, 8192) as i32,
            value["height"].as_i64().unwrap_or(640).clamp(1, 8192) as i32,
        );
        let remote = &mut self.windows[index];
        let valid_fullscreen =
            value["fullscreen_epoch"].as_u64() == Some(remote.state.fullscreen_epoch);
        let fullscreen = valid_fullscreen && value["fullscreen"].as_bool().unwrap_or(false);
        if valid_fullscreen {
            remote.state.fullscreen(fullscreen);
        }
        if !fullscreen {
            let (min, max) = if let Some(top) = &top {
                limits(top.wl_surface())
            } else if let Some(x) = &x11 {
                (
                    x.min_size().map(|s| [s.w, s.h]).unwrap_or([0, 0]),
                    x.max_size().map(|s| [s.w, s.h]).unwrap_or([0, 0]),
                )
            } else {
                ([0, 0], [0, 0])
            };
            size = (axis(size.0, min[0], max[0]), axis(size.1, min[1], max[1]));
        }
        if let Some(top) = top {
            top.with_pending_state(|s| {
                if valid_fullscreen {
                    if fullscreen {
                        s.states.set(xdg_toplevel::State::Fullscreen);
                        s.states.unset(xdg_toplevel::State::Maximized);
                    } else {
                        s.states.unset(xdg_toplevel::State::Fullscreen);
                    }
                }
            });
        }
        Some(size)
    }
    pub(crate) fn configure_capabilities(&mut self) {
        let fullscreen = self.semantics_enabled();
        let mut capabilities = vec![
            xdg_toplevel::WmCapabilities::Maximize,
            xdg_toplevel::WmCapabilities::Minimize,
        ];
        if fullscreen {
            capabilities.push(xdg_toplevel::WmCapabilities::Fullscreen);
        }
        self.shell.replace_capabilities(capabilities.clone());
        for window in &self.windows {
            let Some(top) = window.window.toplevel() else {
                continue;
            };
            top.with_pending_state(|s| {
                s.capabilities.replace(capabilities.clone());
            });
            top.send_pending_configure();
        }
    }
}
impl smithay::wayland::shell::xdg::dialog::XdgDialogHandler for App {
    fn modal_changed(&mut self, _: ToplevelSurface, is_modal: bool) {
        // Do not steal the keyboard from an unrelated active toplevel. Redirect
        // only the parent that has just become blocked by a real modal hint.
        if is_modal {
            if let Some(focused) = self.focused_window {
                let target = self.modal_target(focused);
                if target != focused {
                    self.focus(Some(target));
                }
            }
        }
    }
}
smithay::delegate_xdg_dialog!(App);

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn dimensions_and_fullscreen_generations() {
        assert_eq!(axis(5, 200, 500), 200);
        assert_eq!(axis(900, 200, 500), 500);
        assert_eq!(axis(800, 0, 0), 800);
        assert_eq!(axis(500, 500, 300), 500);
        let mut state = State::default();
        state.fullscreen(true);
        assert_eq!(state.fullscreen_epoch, 1);
        state.fullscreen(true);
        assert_eq!(state.fullscreen_epoch, 1);
        state.fullscreen(false);
        assert_eq!(state.fullscreen_epoch, 2);
    }
}
