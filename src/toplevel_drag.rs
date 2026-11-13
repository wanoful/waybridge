//! Keep Wayland's data-device grab alive while Windows moves an attached HWND.
use crate::*;
use smithay::reexports::{
    wayland_protocols::xdg::toplevel_drag::v1::server::{
        xdg_toplevel_drag_manager_v1::{self as manager, XdgToplevelDragManagerV1},
        xdg_toplevel_drag_v1::{self as drag, XdgToplevelDragV1},
    },
    wayland_server::{DataInit, Dispatch, GlobalDispatch, New},
};

struct Attached {
    surface: WlSurface,
    x: i32,
    y: i32,
    mapped: bool,
}
struct Entry {
    resource: XdgToplevelDragV1,
    source: WlDataSource,
    attached: Option<Attached>,
    ended: bool,
}
struct Active {
    source: Option<WlDataSource>,
    origin: u32,
    deferred_focus: Option<Option<u32>>,
    finishing: bool,
    cancelled: bool,
}
#[derive(Default)]
pub struct State {
    entries: Vec<Entry>,
    used: Vec<WlDataSource>,
    active: Option<Active>,
    last: Value,
    pub available: bool,
}
impl State {
    pub fn register(dh: &DisplayHandle) {
        dh.create_global::<App, XdgToplevelDragManagerV1, _>(1, ());
    }
    pub fn active(&self) -> bool {
        self.active.is_some()
    }
    pub fn unmap(&mut self, surface: &WlSurface) {
        for entry in &mut self.entries {
            if entry
                .attached
                .as_ref()
                .is_some_and(|a| &a.surface == surface)
            {
                entry.attached = None;
            }
        }
    }
    pub fn defer_focus(&mut self, id: Option<u32>) -> bool {
        if let Some(active) = &mut self.active {
            active.deferred_focus = Some(id);
            return true;
        }
        false
    }
    pub fn excludes(&self, surface: &WlSurface) -> bool {
        self.active.as_ref().is_some_and(|a| {
            self.entries.iter().any(|e| {
                Some(&e.source) == a.source.as_ref()
                    && e.attached.as_ref().is_some_and(|t| &t.surface == surface)
            })
        })
    }
    pub fn is_toplevel_source(&self, source: &WlDataSource) -> bool {
        self.entries.iter().any(|e| &e.source == source && !e.ended)
    }
    pub fn completes_move(&self, source: &WlDataSource) -> bool {
        self.active
            .as_ref()
            .is_some_and(|a| !a.cancelled && a.source.as_ref() == Some(source))
            && self.is_toplevel_source(source)
    }
    pub fn ended(&mut self) {
        if let Some(active) = &mut self.active {
            active.finishing = true;
            for entry in &mut self.entries {
                if Some(&entry.source) == active.source.as_ref() {
                    entry.ended = true;
                }
            }
        }
    }
}
impl App {
    pub fn start_window_drag(&mut self, source: Option<WlDataSource>) {
        if !self.window_drag.available || self.window_drag.active() {
            return;
        }
        self.window_drag.active = Some(Active {
            source,
            origin: self.focused_window.unwrap_or(0),
            deferred_focus: None,
            finishing: false,
            cancelled: false,
        });
    }
    pub fn cancel_window_drag(&mut self) {
        if !self.window_drag.active() {
            return;
        }
        self.window_drag.active.as_mut().unwrap().cancelled = true;
        let pointer = self.seat.get_pointer().unwrap();
        // Clearing the DnD target before releasing the grab produces cancelled,
        // rather than accidentally dropping into the last accepted target.
        pointer.motion(
            self,
            None,
            &MotionEvent {
                location: pointer.current_location(),
                serial: SERIAL_COUNTER.next_serial(),
                time: self.time(),
            },
        );
        pointer.unset_grab(self, SERIAL_COUNTER.next_serial(), self.time());
        self.release_input();
        self.window_drag.ended();
    }
    pub fn poll_window_drag(&mut self) {
        if self
            .window_drag
            .active
            .as_ref()
            .is_some_and(|a| !a.finishing && a.source.as_ref().is_some_and(|s| !s.is_alive()))
        {
            self.cancel_window_drag();
        }
        for entry in &mut self.window_drag.entries {
            if let Some(attached) = &mut entry.attached {
                let mapped = self.windows.iter().any(|w| {
                    w.mapped && w.window.wl_surface().as_deref() == Some(&attached.surface)
                });
                if !attached.surface.is_alive() || (attached.mapped && !mapped) {
                    entry.attached = None;
                } else {
                    attached.mapped |= mapped;
                }
            }
        }
        self.window_drag.entries.retain(|e| e.resource.is_alive());
        self.window_drag.used.retain(Resource::is_alive);
        let Some(active) = &self.window_drag.active else {
            return;
        };
        if active.finishing || active.source.as_ref().is_some_and(|s| !s.is_alive()) {
            let attached_id = self
                .window_drag
                .entries
                .iter()
                .find(|e| Some(&e.source) == active.source.as_ref())
                .and_then(|e| e.attached.as_ref())
                .and_then(|a| {
                    self.windows
                        .iter()
                        .find(|w| w.mapped && w.window.wl_surface().as_deref() == Some(&a.surface))
                })
                .map(|w| w.id);
            let active = self.window_drag.active.take().unwrap();
            self.control(json!({"type":"toplevel_drag","active":false}));
            self.window_drag.last = Value::Null;
            // Applying focus inside DnDGrab::unset would re-enter the locked pointer.
            let alive = |id: &u32| self.windows.iter().any(|w| w.id == *id && w.mapped);
            let focus = active
                .deferred_focus
                .flatten()
                .filter(alive)
                .or(if active.cancelled { None } else { attached_id })
                .or_else(|| Some(active.origin).filter(alive));
            self.focus(focus);
            return;
        }
        let attached = self
            .window_drag
            .entries
            .iter()
            .find(|e| Some(&e.source) == active.source.as_ref())
            .and_then(|e| e.attached.as_ref());
        let id = attached
            .and_then(|a| {
                self.windows
                    .iter()
                    .find(|w| w.window.wl_surface().as_deref() == Some(&a.surface))
            })
            .map_or(0, |w| w.id);
        let message = json!({"type":"toplevel_drag","active":true,"source":active.origin,
            "id":id,"offset_x":attached.map_or(0,|a|a.x),"offset_y":attached.map_or(0,|a|a.y)});
        if message != self.window_drag.last {
            self.control(message.clone());
            self.window_drag.last = message;
        }
    }
}
impl GlobalDispatch<XdgToplevelDragManagerV1, ()> for App {
    fn bind(
        _: &mut Self,
        _: &DisplayHandle,
        _: &Client,
        resource: New<XdgToplevelDragManagerV1>,
        _: &(),
        init: &mut DataInit<'_, Self>,
    ) {
        init.init(resource, ());
    }
}
impl Dispatch<XdgToplevelDragManagerV1, ()> for App {
    fn request(
        state: &mut Self,
        _: &Client,
        resource: &XdgToplevelDragManagerV1,
        request: manager::Request,
        _: &(),
        _: &DisplayHandle,
        init: &mut DataInit<'_, Self>,
    ) {
        if let manager::Request::GetXdgToplevelDrag { id, data_source } = request {
            if state.window_drag.used.contains(&data_source) {
                resource.post_error(
                    manager::Error::InvalidSource,
                    "source already used for a toplevel drag",
                );
                return;
            }
            state.window_drag.used.push(data_source.clone());
            state.window_drag.entries.push(Entry {
                resource: init.init(id, ()),
                source: data_source,
                attached: None,
                ended: false,
            });
        }
    }
}
impl Dispatch<XdgToplevelDragV1, ()> for App {
    fn request(
        state: &mut Self,
        _: &Client,
        resource: &XdgToplevelDragV1,
        request: drag::Request,
        _: &(),
        _: &DisplayHandle,
        _: &mut DataInit<'_, Self>,
    ) {
        let Some(entry) = state
            .window_drag
            .entries
            .iter_mut()
            .find(|e| &e.resource == resource)
        else {
            return;
        };
        match request {
            drag::Request::Destroy if !entry.ended => {
                resource.post_error(drag::Error::OngoingDrag, "drag has not ended");
            }
            drag::Request::Attach {
                toplevel,
                x_offset,
                y_offset,
            } => {
                if entry.attached.as_ref().is_some_and(|a| {
                    a.surface.is_alive()
                        && (!a.mapped
                            || state.windows.iter().any(|w| {
                                w.mapped && w.window.wl_surface().as_deref() == Some(&a.surface)
                            }))
                }) {
                    resource.post_error(
                        drag::Error::ToplevelAttached,
                        "a live toplevel is already attached",
                    );
                } else if let Some(top) = state.shell.get_toplevel(&toplevel) {
                    entry.attached = Some(Attached {
                        surface: top.wl_surface().clone(),
                        x: x_offset,
                        y: y_offset,
                        mapped: false,
                    });
                }
            }
            _ => {}
        }
    }
}
