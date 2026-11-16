use std::{
    any::Any,
    cmp,
    collections::{HashMap, VecDeque},
    fmt,
    os::fd::OwnedFd,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex, Weak,
    },
    time::{Duration, Instant},
};

use atomic_float::AtomicF64;
use calloop::LoopHandle;
use rustix::fs::OFlags;
use smallvec::SmallVec;
use tracing::{debug, trace, warn};
#[cfg(feature = "wayland_frontend")]
use wayland_server::DisplayHandle;
use wayland_server::Resource;
use x11rb::{
    connection::Connection as _,
    errors::{ConnectionError, ReplyOrIdError},
    protocol::{
        xfixes::SelectionNotifyEvent,
        xproto::{
            Atom, AtomEnum, ClientMessageData, ClientMessageEvent, ConfigureWindowAux, ConnectionExt,
            CreateWindowAux, EventMask, PropMode, Screen, StackMode, Window as X11Window, WindowClass,
        },
    },
    rust_connection::RustConnection,
    wrapper::ConnectionExt as _,
    CURRENT_TIME,
};

use crate::{
    input::{
        dnd::{DndAction, DndFocus, DndGrabHandler, GrabType, OfferData, Source, SourceMetadata},
        Seat, SeatHandler,
    },
    utils::{IsAlive, Logical, Point, Serial},
    wayland::seat::WaylandFocus,
    xwayland::{
        xwm::{atom_from_mime, mime_from_atom, selection::XWmSelection, Atoms, OwnedX11Window, XwmId},
        X11Surface, XwmHandler,
    },
};

// TODO: We should have a central source for this
const BTN_LEFT: u32 = 0x110;
const DND_VERSION: u32 = 5;
const MIN_DND_VERSION: u32 = 2;

#[derive(Debug)]
pub struct XWmDnd {
    pub selection: XWmSelection,
    pub active_offer: Option<XwmActiveOffer>,
    pub active_drag: Option<XwmActiveDrag>,
    pub xdnd_active: Arc<AtomicBool>,
}

impl XWmDnd {
    pub fn new(conn: &Arc<RustConnection>, screen: &Screen, atoms: &Atoms) -> Result<Self, ReplyOrIdError> {
        let selection = XWmSelection::new(conn, screen, atoms, atoms.XdndSelection)?;

        conn.change_property32(
            PropMode::REPLACE,
            *selection.window,
            atoms.XdndAware,
            AtomEnum::ATOM,
            &[DND_VERSION],
        )?;
        conn.flush()?;

        Ok(XWmDnd {
            selection,
            active_offer: None,
            active_drag: None,
            xdnd_active: Arc::new(AtomicBool::new(false)),
        })
    }

    pub fn expire<D>(&mut self, handle: &LoopHandle<'_, D>) {
        self.selection.expire(handle);
        if self.active_drag.as_ref().is_some_and(|d| {
            d.state
                .lock()
                .unwrap()
                .terminal_since
                .is_some_and(|t| t.elapsed() >= Duration::from_secs(10))
        }) {
            if let Some(drag) = self.active_drag.take() {
                let mut s = drag.state.lock().unwrap();
                s.wayland = WlState::Cancelled;
                if s.mapped {
                    let _ = self.selection.conn.unmap_window(*drag.target);
                    let _ = self.selection.conn.flush();
                    s.mapped = false;
                }
            }
            self.xdnd_active.store(false, Ordering::Release);
        }
    }

    pub fn update_screen(&self, screen: &Screen) -> Result<(), ReplyOrIdError> {
        if let Some(active_drag) = self.active_drag.as_ref() {
            let mut drag_state = active_drag.state.lock().unwrap();

            if drag_state.mapped {
                self.selection.conn.configure_window(
                    *active_drag.target,
                    &ConfigureWindowAux::new()
                        .width(screen.width_in_pixels as u32)
                        .height(screen.height_in_pixels as u32),
                )?;
            } else {
                drag_state.pending_configure = Some((screen.width_in_pixels, screen.height_in_pixels));
            }
        }

        Ok(())
    }

    pub fn window_destroyed<D>(&mut self, window: &X11Window, loop_handle: &LoopHandle<'_, D>) -> bool {
        let mut res = self.selection.window_destroyed(window, loop_handle);
        if self
            .active_offer
            .as_ref()
            .is_some_and(|o| o.state.lock().unwrap().target == *window)
        {
            if let Some(offer) = self.active_offer.take() {
                offer.source.cancel();
            }
            res = true;
        }
        if let Some(active_drag) = self.active_drag.as_ref() {
            if active_drag.owner == *window
                || active_drag
                    .state
                    .lock()
                    .unwrap()
                    .source
                    .as_ref()
                    .is_some_and(|w| w == window)
            {
                active_drag.state.lock().unwrap().wayland = WlState::Cancelled;
                self.active_drag.take();
                self.xdnd_active.store(false, Ordering::Release);
                res = true;
            }
        }
        res
    }

    pub fn has_window(&self, window: &X11Window) -> bool {
        if let Some(drag) = self.active_drag.as_ref() {
            if drag.target == *window {
                return true;
            }
        }

        self.selection.has_window(window)
    }

    pub fn xfixes_selection_notify<D>(
        _dh: &DisplayHandle,
        data: &mut D,
        id: XwmId,
        event: SelectionNotifyEvent,
    ) -> Result<(), ReplyOrIdError>
    where
        D: XwmHandler + SeatHandler + DndGrabHandler + 'static,
        <D as SeatHandler>::PointerFocus: DndFocus<D>,
        <D as SeatHandler>::TouchFocus: DndFocus<D>,
    {
        let xwm = data.xwm_state(id);

        xwm.dnd.selection.owner = event.owner;
        if xwm.dnd.selection.owner == *xwm.dnd.selection.window {
            xwm.dnd.selection.timestamp = event.timestamp;
            return Ok(());
        }

        if xwm
            .dnd
            .active_offer
            .as_ref()
            .is_some_and(|o| o.state.lock().unwrap().native.is_none())
        {
            // rough X11 client tries to take over the selection, take it back.
            xwm.conn
                .set_selection_owner(*xwm.dnd.selection.window, xwm.atoms.XdndSelection, CURRENT_TIME)?;
            return Ok(());
        }

        if let Some(active_drag) = xwm.dnd.active_drag.as_ref() {
            if active_drag.owner == xwm.dnd.selection.owner
                && active_drag.state.lock().unwrap().x11 != X11State::Finished
            {
                warn!("Got another selection notify for an already active grab. Ignoring...");
                return Ok(());
            } else {
                // new drag, ours is presumably outdated/stale now
                debug!("Dropping stale Xwm drag");

                let active_drag = xwm.dnd.active_drag.take().unwrap();
                active_drag.state.lock().unwrap().x11 = X11State::Finished;
                xwm.dnd.xdnd_active.store(false, Ordering::Release);
            }
        }

        if event.owner == x11rb::NONE {
            trace!("XDND selection went away");
            xwm.dnd.active_drag.take();
            xwm.dnd.xdnd_active.store(false, Ordering::Release);
            return Ok(());
        }

        trace!(
            owner = event.owner,
            time = event.timestamp,
            selection_time = event.selection_timestamp,
            "New XDND selection"
        );
        // figure out, if we have a grab
        let ptr_grab = data
            .seat_state()
            .seats
            .iter()
            .flat_map(|seat| {
                seat.get_pointer().and_then(|ptr| {
                    ptr.with_grab(|serial, grab| (seat.clone(), serial, grab.start_data().clone()))
                        .filter(|(_, _, start_data)| {
                            start_data.button == BTN_LEFT
                                && start_data
                                    .focus
                                    .as_ref()
                                    .and_then(|(f, _)| f.wl_surface())
                                    .and_then(|s| s.client())
                                    .is_some_and(|c| {
                                        c.get_data::<crate::xwayland::XWaylandClientData>().is_some()
                                    })
                        })
                })
            })
            .max_by(|(_, s1, _), (_, s2, _)| s1.partial_cmp(s2).unwrap_or(cmp::Ordering::Equal));

        let touch_grab = data
            .seat_state()
            .seats
            .iter()
            .flat_map(|seat| {
                seat.get_touch().and_then(|touch| {
                    touch.with_grab(|serial, grab| (seat.clone(), serial, grab.start_data().clone()))
                })
            })
            .max_by(|(_, s1, _), (_, s2, _)| s1.partial_cmp(s2).unwrap_or(cmp::Ordering::Equal));

        if ptr_grab.is_none() && touch_grab.is_none() {
            return Ok(());
        }

        // create our drop proxy
        let xwm = data.xwm_state(id);
        let window = xwm.conn.generate_id()?;
        xwm.conn.create_window(
            xwm.screen.root_depth,
            window,
            xwm.screen.root,
            0,
            0,
            xwm.screen.width_in_pixels,
            xwm.screen.height_in_pixels,
            0,
            WindowClass::INPUT_OUTPUT,
            xwm.screen.root_visual,
            &CreateWindowAux::new().event_mask(EventMask::PROPERTY_CHANGE),
        )?;
        xwm.conn.change_property32(
            PropMode::REPLACE,
            window,
            xwm.atoms.XdndAware,
            AtomEnum::ATOM,
            &[DND_VERSION],
        )?;
        xwm.conn.change_property8(
            PropMode::REPLACE,
            window,
            xwm.atoms.WM_NAME,
            xwm.atoms.UTF8_STRING,
            "Smithay XDND proxy".as_bytes(),
        )?;
        xwm.conn.map_window(window)?;
        xwm.conn
            .configure_window(window, &ConfigureWindowAux::new().stack_mode(StackMode::ABOVE))?;
        xwm.conn.flush()?;

        let window = OwnedX11Window::new(window, &xwm.conn);

        // create our dnd source
        let state = Arc::new(Mutex::new(XwmSourceState {
            mapped: true,
            x11: X11State::Active,
            wayland: WlState::Active,
            source: None,
            pending_configure: None,
            metadata: SourceMetadata {
                mime_types: vec![],
                dnd_actions: SmallVec::new_const(),
            },
            active_action: DndAction::None,
            requested_action: DndAction::None,
            offered_actions: SmallVec::new(),
            last_timestamp: None,
            pending_reads: VecDeque::new(),
            terminal_since: None,
            version: 0,
        }));
        let source = XwmDndSource {
            xwm: id,
            conn: Arc::downgrade(&xwm.conn),
            atoms: xwm.atoms,
            target: window.clone(),
            state: state.clone(),
            pending_transfers: xwm.dnd.selection.pending_transfers.clone(),
            deadlines: xwm.dnd.selection.deadlines.clone(),
        };
        xwm.dnd.active_drag = Some(XwmActiveDrag {
            target: window,
            owner: xwm.dnd.selection.owner,
            state,
            pending_transfers: xwm.dnd.selection.pending_transfers.clone(),
            deadlines: xwm.dnd.selection.deadlines.clone(),
        });
        xwm.dnd.xdnd_active.store(true, Ordering::Release);

        // create the DndGrab
        match (ptr_grab, touch_grab) {
            (Some((seat, s1, _start_data)), Some((_, s2, _))) if s1 >= s2 => {
                // pointer_grab
                data.x11_dnd_requested(source, seat, s1, GrabType::Pointer);
            }
            (Some((seat, serial, _start_data)), None) => {
                // pointer grab
                data.x11_dnd_requested(source, seat, serial, GrabType::Pointer);
            }
            (_, Some((seat, serial, _start_data))) => {
                // touch_grab
                data.x11_dnd_requested(source, seat, serial, GrabType::Touch);
            }
            (None, None) => unreachable!(),
        };

        Ok(())
    }

    pub fn accepts_source(&self, window: X11Window) -> bool {
        self.active_drag.as_ref().is_some_and(|d| *d.target == window)
    }
    pub fn accepts_reply(&self, window: X11Window) -> bool {
        self.active_offer
            .as_ref()
            .is_some_and(|o| *o.state.lock().unwrap().source_window == window)
    }
    pub fn handle_status(
        &mut self,
        window: &X11Surface,
        status_msg: ClientMessageData,
        atoms: &Atoms,
        client_scale: Arc<AtomicF64>,
    ) {
        let Some(offer) = self.active_offer.as_ref() else {
            return;
        };
        let data = status_msg.as_data32();
        let mut state = offer.state.lock().unwrap();
        if data[0] != state.target || !state.active {
            return;
        }
        let action = DndAction::from_x(data[4], atoms);
        let accepted =
            data[1] & 1 != 0 && state.supported_actions.contains(&action) && action != DndAction::None;
        if !state.dropped {
            state.client_accepts = accepted;
            state.preferred_action = if accepted { action } else { DndAction::None };
            let mime = offer
                .source
                .metadata()
                .and_then(|m| m.mime_types.into_iter().next());
            offer.source.accepted(if accepted { mime } else { None });
            offer.source.choose_action(state.preferred_action);
            offer.source.offer_status(accepted, state.preferred_action, true);
        }
        state.pos_pending = false;
        if let Some(pos) = state.pos_cached.take() {
            let p = (window.geometry().loc + pos.to_i32_round())
                .to_client_precise_round::<_, i32>(client_scale.load(Ordering::Acquire));
            let packet = [
                *state.source_window,
                0,
                ((p.x << 16) | p.y) as u32,
                state.time(false).unwrap_or(CURRENT_TIME),
                state.requested_action.to_x(atoms),
            ];
            let _ = self.selection.conn.send_event(
                false,
                state.proxy.unwrap_or(state.target),
                EventMask::NO_EVENT,
                ClientMessageEvent::new(32, state.target, atoms.XdndPosition, packet),
            );
            let _ = self.selection.conn.flush();
            state.pos_pending = true;
        } else if state.dropped {
            state.send_drop(&self.selection.conn, atoms);
        }
    }

    pub fn handle_finished(&mut self, status_msg: ClientMessageData) {
        if let Some(offer) = self.active_offer.as_ref() {
            let offer_state = offer.state.lock().unwrap();
            let data = status_msg.as_data32();
            trace!("Got XDND finished msg: {:?}", data);

            if data[0] != offer_state.target {
                return;
            }

            if !offer_state.dropped {
                return;
            }

            let success = if offer_state.version >= 5 {
                data[1] & 1 != 0
                    && offer_state
                        .supported_actions
                        .contains(&DndAction::from_x(data[2], &self.selection.atoms))
            } else {
                offer_state.client_accepts
            };
            if success {
                offer.source.choose_action(if offer_state.version >= 5 {
                    DndAction::from_x(data[2], &self.selection.atoms)
                } else {
                    offer_state.preferred_action
                });
                offer.source.finished()
            } else {
                offer.source.cancel()
            };

            std::mem::drop(offer_state);
            self.active_offer.take();
        }
    }

    pub fn transfer(&mut self, mime_type: impl AsRef<str>, send_fd: OwnedFd) {
        if let Some(offer) = self.active_offer.as_ref() {
            offer.source.send(mime_type.as_ref(), send_fd);
        }
    }

    pub fn handle_enter(&mut self, enter_msg: ClientMessageData) -> Result<(), ReplyOrIdError> {
        if let Some(drag) = self.active_drag.as_mut() {
            let mut drag_state = drag.state.lock().unwrap();
            let data = enter_msg.as_data32();

            trace!("Got XDND enter msg: {:?}", data);
            if drag_state.source.is_some_and(|w| w != data[0]) {
                debug!(
                    "Received XdndEnter from unknown source (got: {}, state: {:?}), ignoring..",
                    data[0], drag_state.source
                );
                return Ok(());
            }
            drag_state.source = Some(data[0]);
            let source = data[0];

            let version = data[1] >> 24;
            if version > DND_VERSION {
                warn!("New unsupported XDND version: {}", data[1]);
                return Ok(());
            }

            // get types
            let mimes = if (data[1] & 1) == 0 {
                (2..5)
                    .flat_map(|i| {
                        mime_from_atom(data[i], &self.selection.conn, &self.selection.atoms).transpose()
                    })
                    .collect::<Result<Vec<_>, _>>()?
            } else {
                let reply = self
                    .selection
                    .conn
                    .get_property(
                        false,
                        source,
                        self.selection.atoms.XdndTypeList,
                        AtomEnum::ATOM,
                        0,
                        64,
                    )?
                    .reply()?;
                if reply.bytes_after != 0 || reply.format != 32 {
                    return Ok(());
                }
                let Some(values) = reply.value32() else {
                    return Ok(());
                };

                values
                    .flat_map(|atom| {
                        mime_from_atom(atom, &self.selection.conn, &self.selection.atoms).transpose()
                    })
                    .collect::<Result<Vec<_>, _>>()?
            };

            drag_state.x11 = X11State::Active;
            if mimes.len() > 64 || mimes.iter().any(|m| m.len() > 255) {
                return Ok(());
            }
            drag_state.metadata.mime_types = mimes;
            drag_state.version = version;
            let list = self
                .selection
                .conn
                .get_property(
                    false,
                    source,
                    self.selection.atoms.XdndActionList,
                    AtomEnum::ATOM,
                    0,
                    8,
                )?
                .reply()?;
            if list.bytes_after == 0 && list.format == 32 {
                drag_state.offered_actions = list
                    .value32()
                    .into_iter()
                    .flatten()
                    .map(|a| DndAction::from_x(a, &self.selection.atoms))
                    .filter(|a| matches!(a, DndAction::Copy | DndAction::Move))
                    .collect();
            }
        }

        Ok(())
    }

    pub fn handle_position(&mut self, pos_msg: ClientMessageData) -> Result<(), ReplyOrIdError> {
        if let Some(drag) = self.active_drag.as_ref() {
            let mut drag_state = drag.state.lock().unwrap();

            let data = pos_msg.as_data32();
            if drag_state.source.is_none_or(|w| w != data[0]) {
                debug!(
                    "Received XdndPosition from unknown source (got: {}, expected: {:?}), ignoring..",
                    data[0], drag_state.source
                );
                return Ok(());
            }
            let source = data[0];

            let requested = DndAction::from_x(data[4], &self.selection.atoms);
            let mut actions = drag_state.offered_actions.clone();
            if actions.is_empty() {
                actions.push(DndAction::Copy);
                if requested == DndAction::Move {
                    actions.push(requested);
                }
            }
            drag_state.requested_action = requested;

            drag_state.last_timestamp = Some(data[3]);
            drag_state.metadata.dnd_actions = actions;

            let mut flags = 1 << 1;
            if !matches!(&drag_state.wayland, WlState::Cancelled)
                && drag_state.active_action != DndAction::None
            {
                flags |= 1 << 0;
            }
            let data = [
                *drag.target,
                flags,
                0,
                0,
                drag_state.active_action.to_x(&self.selection.atoms),
            ];
            self.selection.conn.send_event(
                false,
                source,
                EventMask::NO_EVENT,
                ClientMessageEvent::new(32, source, self.selection.atoms.XdndStatus, data),
            )?;
            self.selection.conn.flush()?;

            for (mime, fd) in std::mem::take(&mut drag_state.pending_reads) {
                request_data(
                    &self.selection.conn,
                    &self.selection.atoms,
                    &drag.target,
                    drag_state.last_timestamp.unwrap(),
                    &mime,
                    fd,
                    &drag.pending_transfers,
                    &drag.deadlines,
                )?;
            }
        }
        Ok(())
    }

    pub fn handle_leave(&mut self, leave_msg: ClientMessageData) -> Result<(), ReplyOrIdError> {
        if let Some(drag) = self.active_drag.as_mut() {
            let mut state = drag.state.lock().unwrap();

            let data = leave_msg.as_data32();
            trace!("Got XDND leave msg: {:?}", data);

            if state.source.is_none_or(|w| w != data[0]) {
                debug!(
                    "Received XdndLeave from unknown source (got: {}, expected: {:?}), ignoring..",
                    data[0], state.source
                );
                return Ok(());
            }
            let _ = state.source.take();

            state.x11 = X11State::Left;

            if matches!(state.wayland, WlState::Dropped) {
                state.wayland = WlState::Finished;
            }
            if matches!(
                &state.wayland,
                &WlState::Cancelled | &WlState::Finished | &WlState::Completed
            ) {
                state.pending_reads.clear();
                std::mem::drop(state);
                self.active_drag.take();
                self.xdnd_active.store(false, Ordering::Release);
            }
        }
        Ok(())
    }

    pub fn handle_drop(&mut self, drop_msg: ClientMessageData) -> Result<(), ReplyOrIdError> {
        if let Some(drag) = self.active_drag.take() {
            let mut state = drag.state.lock().unwrap();
            self.xdnd_active.store(false, Ordering::Release);

            let data = drop_msg.as_data32();
            trace!("Got XDND drop msg: {:?}", data);

            if state.source.is_none_or(|w| w != data[0]) {
                debug!(
                    "Received XdndDrop from unknown source (got: {}, expected: {:?}), ignoring..",
                    data[0], state.source
                );
                return Ok(());
            }
            let source = data[0];

            let conn = &self.selection.conn;
            let atoms = &self.selection.atoms;

            let failed = || -> Result<(), ReplyOrIdError> {
                drag.pending_transfers.lock().unwrap().remove(&drag.target);

                let data = [*drag.target, 0, 0, 0, 0];
                conn.send_event(
                    false,
                    source,
                    EventMask::NO_EVENT,
                    ClientMessageEvent::new(32, source, atoms.XdndFinished, data),
                )?;
                conn.flush()?;

                Ok(())
            };

            if matches!(&state.wayland, &WlState::Completed) {
                let packet = [*drag.target, 1, state.active_action.to_x(atoms), 0, 0];
                conn.send_event(
                    false,
                    source,
                    EventMask::NO_EVENT,
                    ClientMessageEvent::new(32, source, atoms.XdndFinished, packet),
                )?;
                conn.flush()?;
                state.x11 = X11State::Finished;
                if state.mapped {
                    conn.unmap_window(*drag.target)?;
                    conn.flush()?;
                    state.mapped = false;
                }
                return Ok(());
            }
            if matches!(&state.wayland, &WlState::Cancelled | &WlState::Finished) {
                failed()?;
                return Ok(());
            }

            if state.version >= 1 {
                state.last_timestamp = Some(data[2]);
            }

            for (mime, fd) in std::mem::take(&mut state.pending_reads) {
                request_data(
                    conn,
                    atoms,
                    &drag.target,
                    state.last_timestamp.unwrap(),
                    &mime,
                    fd,
                    &drag.pending_transfers,
                    &drag.deadlines,
                )?;
            }

            state.x11 = X11State::Dropped;
            std::mem::drop(state);
            if let Some(offer) = self.active_offer.as_ref() {
                let mut state = offer.state.lock().unwrap();
                if state.dropped && !state.pos_pending {
                    state.send_drop(conn, atoms);
                }
            }
        }
        Ok(())
    }
}

impl DndAction {
    fn from_x(atom: Atom, atoms: &Atoms) -> DndAction {
        match atom {
            x if x == atoms.XdndActionCopy => DndAction::Copy,
            x if x == atoms.XdndActionMove => DndAction::Move,
            x if x == atoms.XdndActionAsk => DndAction::Ask,
            _ => DndAction::None,
        }
    }

    fn to_x(self, atoms: &Atoms) -> Atom {
        match self {
            DndAction::Copy => atoms.XdndActionCopy,
            DndAction::Move => atoms.XdndActionMove,
            DndAction::Ask => atoms.XdndActionAsk,
            DndAction::None => AtomEnum::NONE.into(),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum X11State {
    Active,
    Left,
    Dropped,
    Finished,
}

#[derive(Debug)]
enum WlState {
    Active,
    Cancelled,
    Dropped,
    Completed,
    Finished,
}

#[derive(Debug)]
struct XwmSourceState {
    x11: X11State,
    wayland: WlState,
    source: Option<X11Window>,

    mapped: bool,
    pending_configure: Option<(u16, u16)>,

    active_action: DndAction,
    requested_action: DndAction,
    offered_actions: SmallVec<[DndAction; 3]>,
    last_timestamp: Option<u32>,
    pending_reads: VecDeque<(String, OwnedFd)>,
    terminal_since: Option<Instant>,
    version: u32,

    metadata: SourceMetadata,
}

#[derive(Debug)]
pub struct XwmActiveDrag {
    target: OwnedX11Window,
    owner: X11Window,

    state: Arc<Mutex<XwmSourceState>>,
    pending_transfers: Arc<Mutex<HashMap<X11Window, (OwnedX11Window, OwnedFd)>>>,
    deadlines: Arc<Mutex<HashMap<X11Window, Instant>>>,
}

#[derive(Debug)]
pub struct XwmDndSource {
    xwm: XwmId,
    conn: Weak<RustConnection>,
    atoms: Atoms,

    target: OwnedX11Window,

    state: Arc<Mutex<XwmSourceState>>,
    pending_transfers: Arc<Mutex<HashMap<X11Window, (OwnedX11Window, OwnedFd)>>>,
    deadlines: Arc<Mutex<HashMap<X11Window, Instant>>>,
}

impl Drop for XwmDndSource {
    fn drop(&mut self) {
        if let Some(conn) = self.conn.upgrade() {
            let mut state = self.state.lock().unwrap();
            // A Wayland/Windows target may finish before the original X11
            // source's Drop event reaches XWM. Keep that terminal result and
            // the shadow target alive until handle_drop can acknowledge it.
            if !matches!(state.wayland, WlState::Completed | WlState::Cancelled) {
                state.wayland = WlState::Finished;
            }
            state.terminal_since.get_or_insert_with(Instant::now);
            if state.mapped && !matches!(state.wayland, WlState::Completed) {
                let _ = conn.unmap_window(*self.target);
                let _ = conn.flush();
            }
        }
    }
}

impl IsAlive for XwmDndSource {
    fn alive(&self) -> bool {
        let state = self.state.lock().unwrap();
        !matches!(&state.wayland, &WlState::Cancelled)
            || (matches!(&state.wayland, &WlState::Finished) && state.x11 != X11State::Finished)
    }
}

impl Source for XwmDndSource {
    fn preferred_action(&self) -> Option<DndAction> {
        Some(self.state.lock().unwrap().requested_action)
    }
    fn metadata(&self) -> Option<crate::input::dnd::SourceMetadata> {
        Some(self.state.lock().unwrap().metadata.clone())
    }

    fn choose_action(&self, action: DndAction) {
        let mut state = self.state.lock().unwrap();
        state.active_action = action;
        // Wayland acceptance is asynchronous. Report changed feedback without
        // waiting for another physical motion from a stationary X11 source.
        if let (Some(conn), Some(source)) = (self.conn.upgrade(), state.source) {
            let accepted = action != DndAction::None && !matches!(state.wayland, WlState::Cancelled);
            let packet = [
                *self.target,
                2 | u32::from(accepted),
                0,
                0,
                action.to_x(&self.atoms),
            ];
            let _ = conn.send_event(
                false,
                source,
                EventMask::NO_EVENT,
                ClientMessageEvent::new(32, source, self.atoms.XdndStatus, packet),
            );
            let _ = conn.flush();
        }
    }

    fn send(&self, mime_type: &str, fd: OwnedFd) {
        let mut state = self.state.lock().unwrap();
        tracing::debug!(mime_type,time=?state.last_timestamp,phase=?state.wayland,"XDND source read");
        if !state.metadata.mime_types.iter().any(|m| m == mime_type)
            || matches!(state.wayland, WlState::Cancelled | WlState::Completed)
        {
            return;
        }
        if rustix::fs::fcntl_setfl(&fd, OFlags::NONBLOCK).is_err() {
            return;
        }
        if let Some(time) = state.last_timestamp {
            if let Some(conn) = self.conn.upgrade() {
                if let Err(error) = request_data(
                    &conn,
                    &self.atoms,
                    &self.target,
                    time,
                    mime_type,
                    fd,
                    &self.pending_transfers,
                    &self.deadlines,
                ) {
                    warn!(%error,"XDND read could not start");
                }
            }
        } else if state.pending_reads.len() < 8 {
            state.pending_reads.push_back((mime_type.to_owned(), fd));
        }
    }

    fn drop_performed(&self) {
        self.state.lock().unwrap().wayland = WlState::Dropped;
    }

    fn cancel(&self) {
        let mut state = self.state.lock().unwrap();
        state.wayland = WlState::Cancelled;
        state.terminal_since = Some(Instant::now());
        if state.mapped {
            if let Some(conn) = self.conn.upgrade() {
                let _ = conn.unmap_window(*self.target);
                let _ = conn.flush();
            }
            state.mapped = false;
        }

        if state.x11 == X11State::Dropped {
            if let (Some(conn), Some(source)) = (self.conn.upgrade(), state.source) {
                trace!("XDND cancelled, but already dropped, sending XDNDFinished");

                let data = [*self.target, 0, 0, 0, 0];
                if let Err(err) = conn.send_event(
                    false,
                    source,
                    EventMask::NO_EVENT,
                    ClientMessageEvent::new(32, source, self.atoms.XdndFinished, data),
                ) {
                    warn!("Failed to send XdndFinished: {:?}", err);
                }
                let _ = conn.flush();
            }
            state.x11 = X11State::Finished;
        }
    }

    fn finished(&self) {
        let mut state = self.state.lock().unwrap();

        if state.x11 == X11State::Dropped {
            if let (Some(conn), Some(source)) = (self.conn.upgrade(), state.source) {
                trace!("XDND done, sending XDNDFinished");

                let data = [
                    *self.target,
                    (1 << 0),
                    state.active_action.to_x(&self.atoms),
                    0,
                    0,
                ];
                if let Err(err) = conn.send_event(
                    false,
                    source,
                    EventMask::NO_EVENT,
                    ClientMessageEvent::new(32, source, self.atoms.XdndFinished, data),
                ) {
                    warn!("Failed to send XdndFinished: {:?}", err);
                }
                let _ = conn.flush();
            }
            state.x11 = X11State::Finished;
            if state.mapped {
                if let Some(conn) = self.conn.upgrade() {
                    let _ = conn.unmap_window(*self.target);
                    let _ = conn.flush();
                }
                state.mapped = false;
            }
        }

        state.wayland = WlState::Completed;
        state.terminal_since = Some(Instant::now());
    }

    fn is_client_local(&self, target: &dyn Any) -> bool {
        target.downcast_ref::<XwmId>().is_some_and(|id| *id == self.xwm)
    }
}

#[derive(Debug)]
struct XwmOfferState {
    target: X11Window,
    source_window: OwnedX11Window,
    native: Option<Arc<Mutex<XwmSourceState>>>,
    version: u32,
    drop_sent: bool,
    proxy: Option<X11Window>,

    requested_action: DndAction,
    preferred_action: DndAction,
    supported_actions: SmallVec<[DndAction; 3]>,

    pos_pending: bool,
    last_pos: Point<f64, Logical>,
    pos_cached: Option<Point<f64, Logical>>,

    client_accepts: bool,
    active: bool,
    dropped: bool,
}

pub struct XwmActiveOffer {
    state: Arc<Mutex<XwmOfferState>>,
    source: Arc<dyn Source>,
}

impl fmt::Debug for XwmActiveOffer {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("XwmActiveOffer")
            .field("state", &self.state)
            .finish_non_exhaustive()
    }
}

/// Type implementing [`OfferData`](crate::input::dnd::OfferData) for XDND based DnD sources
#[derive(Debug)]
pub struct XwmOfferData<S: Source> {
    state: Arc<Mutex<XwmOfferState>>,
    source: Arc<S>,
}

fn get_proxy_window(
    window: X11Window,
    conn: &impl ConnectionExt,
    atoms: &Atoms,
) -> Result<Option<X11Window>, ConnectionError> {
    if let Some(prop) = conn
        .get_property(false, window, atoms.XdndProxy, AtomEnum::WINDOW, 0, 1)?
        .reply_unchecked()?
    {
        if let Some(proxy) = prop.value32().and_then(|mut val| val.next()) {
            if let Some(prop) = conn
                .get_property(false, proxy, atoms.XdndProxy, AtomEnum::WINDOW, 0, 1)?
                .reply_unchecked()?
            {
                if let Some(verify) = prop.value32().and_then(|mut val| val.next()) {
                    if proxy == verify {
                        return Ok(Some(proxy));
                    }
                }
            }
        }
    }

    Ok(None)
}

impl<D: XwmHandler + SeatHandler + 'static> DndFocus<D> for X11Surface {
    type OfferData<S>
        = XwmOfferData<S>
    where
        S: Source;

    fn enter<S: Source>(
        &self,
        data: &mut D,
        _dh: &DisplayHandle,
        source: Arc<S>,
        seat: &Seat<D>,
        location: Point<f64, Logical>,
        _serial: &Serial,
    ) -> Option<Self::OfferData<S>> {
        let xwm_id = self.xwm_id()?;
        let xwm = data.xwm_state(xwm_id);

        let native = if source.is_client_local(&xwm_id) {
            Some(xwm.dnd.active_drag.as_ref()?.state.clone())
        } else {
            None
        };
        let source_window = if native.is_some() {
            xwm.dnd.active_drag.as_ref()?.target.clone()
        } else {
            let fresh =
                XWmSelection::new(&xwm.conn, &xwm.screen, &xwm.atoms, xwm.atoms.XdndSelection).ok()?;
            xwm.dnd.selection.window = fresh.window;
            xwm.dnd.selection.timestamp = 0;
            xwm.conn
                .set_selection_owner(*xwm.dnd.selection.window, xwm.atoms.XdndSelection, CURRENT_TIME)
                .ok()?;
            xwm.dnd.selection.window.clone()
        };
        let proxy = get_proxy_window(self.window_id(), &xwm.conn, &self.atoms).ok()?;
        let proxy_or_window = proxy.unwrap_or(self.window_id());

        let prop = xwm
            .conn
            .get_property(false, proxy_or_window, self.atoms.XdndAware, AtomEnum::ANY, 0, 1)
            .ok()?
            .reply()
            .ok()?;
        if prop.type_ != AtomEnum::ATOM.into() {
            return None;
        }
        let client_ver = prop.value32()?.next()?;
        if client_ver < MIN_DND_VERSION {
            return None;
        }
        let version = client_ver.min(DND_VERSION);

        let metadata = source.metadata()?;
        let mime_types = metadata
            .mime_types
            .iter()
            .filter_map(|mime| atom_from_mime(mime, &xwm.conn, &xwm.atoms).transpose())
            .collect::<Result<Vec<_>, _>>()
            .inspect_err(|err| warn!("Failed to convert mime types: {:?}", err))
            .ok()?;

        let mut enter_data = [
            *source_window,
            version << 24,
            AtomEnum::NONE.into(),
            AtomEnum::NONE.into(),
            AtomEnum::NONE.into(),
        ];
        for (i, mime_type) in mime_types.iter().take(3).enumerate() {
            enter_data[i + 2] = *mime_type;
        }

        if mime_types.len() > 3 {
            enter_data[1] |= 1;
            xwm.conn
                .change_property32(
                    PropMode::REPLACE,
                    *source_window,
                    self.atoms.XdndTypeList,
                    AtomEnum::ATOM,
                    &mime_types,
                )
                .inspect_err(|err| warn!("Failed to update DND_TYPE_LIST: {:?}", err))
                .ok()?;
        } else {
            xwm.conn
                .delete_property(*source_window, self.atoms.XdndTypeList)
                .inspect_err(|err| warn!("Failed to update DND_TYPE_LIST: {:?}", err))
                .ok()?;
        }
        let actions: Vec<_> = metadata.dnd_actions.iter().map(|a| a.to_x(&xwm.atoms)).collect();
        xwm.conn
            .change_property32(
                PropMode::REPLACE,
                *source_window,
                xwm.atoms.XdndActionList,
                AtomEnum::ATOM,
                &actions,
            )
            .ok()?;
        xwm.dnd.selection.mime_types = metadata.mime_types;

        trace!("Sending XdndEnter: {:?}", enter_data);
        xwm.conn
            .send_event(
                false,
                proxy_or_window,
                EventMask::NO_EVENT,
                ClientMessageEvent::new(32, self.window_id(), self.atoms.XdndEnter, enter_data),
            )
            .inspect_err(|err| warn!("Failed to send DND_ENTER event: {:?}", err))
            .ok()?;

        let mut offer = XwmOfferData {
            state: Arc::new(Mutex::new(XwmOfferState {
                target: self.window_id(),
                source_window: source_window.clone(),
                native,
                version,
                drop_sent: false,
                proxy,

                requested_action: if metadata.dnd_actions.contains(&DndAction::Move) {
                    DndAction::Move
                } else if metadata.dnd_actions.contains(&DndAction::Copy) {
                    DndAction::Copy
                } else {
                    DndAction::None
                },
                preferred_action: DndAction::None,
                supported_actions: metadata.dnd_actions,

                last_pos: location,
                pos_pending: false,
                pos_cached: None,

                client_accepts: false,
                active: true,
                dropped: false,
            })),
            source,
        };
        xwm.dnd.active_offer = Some(XwmActiveOffer {
            state: offer.state.clone(),
            source: offer.source.clone(),
        });

        DndFocus::motion(self, data, Some(&mut offer), seat, location, 0);
        Some(offer)
    }

    fn motion<S: Source>(
        &self,
        data: &mut D,
        offer: Option<&mut XwmOfferData<S>>,
        seat: &Seat<D>,
        location: Point<f64, Logical>,
        _time: u32,
    ) {
        let Some(offer) = offer else { return };
        let mut state = offer.state.lock().unwrap();
        if state.dropped {
            return;
        }
        state.last_pos = location;
        let mods = seat.get_keyboard().map(|k| k.modifier_state());
        let requested = offer.source.preferred_action().unwrap_or_else(|| {
            if mods.as_ref().is_some_and(|m| m.ctrl) {
                DndAction::Copy
            } else if mods.as_ref().is_some_and(|m| m.shift) {
                DndAction::Move
            } else if state.supported_actions.contains(&DndAction::Move) {
                DndAction::Move
            } else {
                DndAction::Copy
            }
        });
        state.requested_action = if state.supported_actions.contains(&requested) {
            requested
        } else {
            DndAction::None
        };
        let preferred = {
            if state.pos_pending {
                state.pos_cached = Some(location);
                return;
            }
            state.pos_pending = true;
            state.requested_action
        };

        let Some(xwm_id) = self.xwm_id() else { return };
        let xwm = data.xwm_state(xwm_id);
        let location = (self.geometry().loc + location.to_i32_round())
            .to_client_precise_round::<_, i32>(xwm.client_scale.load(Ordering::Acquire));

        let data = [
            *state.source_window,
            0,
            ((location.x << 16) | location.y) as u32,
            state.time(false).unwrap_or(CURRENT_TIME),
            preferred.to_x(&xwm.atoms),
        ];

        if let Err(err) = xwm.conn.send_event(
            false,
            state.proxy.unwrap_or(self.window_id()),
            EventMask::NO_EVENT,
            ClientMessageEvent::new(32, self.window_id(), self.atoms.XdndPosition, data),
        ) {
            warn!("Failed to send DND_POSITION event: {:?}", err);
        }
        let _ = xwm.conn.flush();
    }

    fn leave<S: Source>(&self, data: &mut D, offer: Option<&mut XwmOfferData<S>>, _seat: &Seat<D>) {
        let Some(xwm_id) = self.xwm_id() else { return };
        let Some(offer) = offer else { return };
        let state = offer.state.lock().unwrap();
        if !state.dropped {
            let xwm = data.xwm_state(xwm_id);
            let data = [*state.source_window, 0, 0, 0, 0];
            trace!("Sending XdndLeave: {:?}", data);

            if let Err(err) = xwm.conn.send_event(
                false,
                state.proxy.unwrap_or(self.window_id()),
                EventMask::NO_EVENT,
                ClientMessageEvent::new(32, self.window_id(), self.atoms.XdndLeave, data),
            ) {
                warn!("Failed to send DND_LEAVE event: {:?}", err);
            }
            let _ = xwm.conn.flush();

            xwm.dnd.active_offer = None;
        }
    }

    fn drop<S: Source>(&self, data: &mut D, offer: Option<&mut XwmOfferData<S>>, _seat: &Seat<D>) {
        let Some(xwm_id) = self.xwm_id() else { return };
        let xwm = data.xwm_state(xwm_id);

        let Some(offer) = offer else { return };
        let mut state = offer.state.lock().unwrap();
        if !state.active || !state.client_accepts || state.preferred_action == DndAction::None {
            return;
        }
        state.dropped = true;
        if !state.pos_pending {
            state.send_drop(&xwm.conn, &xwm.atoms);
        }
    }
}

impl<S: Source> OfferData for XwmOfferData<S> {
    fn disable(&self) {
        self.state.lock().unwrap().active = false;
        self.source.accepted(None);
        self.source.choose_action(DndAction::None);
        self.source.offer_status(false, DndAction::None, true);
    }

    fn drop(&self) {
        self.state.lock().unwrap().dropped = true;
    }

    fn validated(&self) -> bool {
        let state = self.state.lock().unwrap();
        state.active && state.preferred_action != DndAction::None && state.client_accepts
    }
}

// Each read owns a separate requestor/property. Concurrent format probes and
// the Windows export read cannot replace one another's descriptor or INCR data.
fn request_data(
    conn: &Arc<RustConnection>,
    atoms: &Atoms,
    parent: &OwnedX11Window,
    time: u32,
    mime: &str,
    fd: OwnedFd,
    pending: &Arc<Mutex<HashMap<X11Window, (OwnedX11Window, OwnedFd)>>>,
    deadlines: &Arc<Mutex<HashMap<X11Window, Instant>>>,
) -> Result<(), ReplyOrIdError> {
    let Some(atom) = atom_from_mime(mime, conn, atoms)? else {
        return Ok(());
    };
    let mut times = deadlines.lock().unwrap();
    let mut reads = pending.lock().unwrap();
    if times.len() >= 8 {
        return Ok(());
    }
    let root = &conn.setup().roots[0];
    let window = conn.generate_id()?;
    conn.create_window(
        root.root_depth,
        window,
        **parent,
        0,
        0,
        1,
        1,
        0,
        WindowClass::INPUT_OUTPUT,
        root.root_visual,
        &CreateWindowAux::new().event_mask(EventMask::PROPERTY_CHANGE),
    )?;
    let owned = OwnedX11Window::new(window, conn);
    tracing::trace!(window, atom, time, "XDND conversion request");
    conn.convert_selection(window, atoms.XdndSelection, atom, atoms._WL_SELECTION, time)?;
    times.insert(window, Instant::now());
    reads.insert(window, (owned, fd));
    conn.flush()?;
    Ok(())
}

impl XwmOfferState {
    fn time(&self, dropped: bool) -> Option<u32> {
        match self.native.as_ref() {
            None => Some(CURRENT_TIME),
            Some(s) => {
                let s = s.lock().unwrap();
                if dropped && s.x11 != X11State::Dropped {
                    None
                } else {
                    s.last_timestamp
                }
            }
        }
    }
    fn send_drop(&mut self, conn: &Arc<RustConnection>, atoms: &Atoms) {
        if self.drop_sent || !self.dropped || !self.active {
            return;
        }
        let Some(time) = self.time(true) else { return };
        let packet = [*self.source_window, 0, time, 0, 0];
        if conn
            .send_event(
                false,
                self.proxy.unwrap_or(self.target),
                EventMask::NO_EVENT,
                ClientMessageEvent::new(32, self.target, atoms.XdndDrop, packet),
            )
            .is_ok()
        {
            self.drop_sent = true;
            let _ = conn.flush();
        }
    }
}
