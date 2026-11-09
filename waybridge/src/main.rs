//! Headless compositor. It never connects to a host desktop or injects host input.
mod cursor;
mod app_icon;
mod ime;
mod metrics;
mod network;
mod protocol;
mod relative_input;
mod transfer;
mod video;
mod x11;
mod window_semantics;

use anyhow::{bail, Context, Result};
use base64::{engine::general_purpose::STANDARD as B64, Engine};
use clap::Parser;
use network::{Event, Peer};
use serde_json::{json, Value};
use smithay::{
    backend::{
        allocator::{dmabuf::Dmabuf, Fourcc},
        egl::{EGLContext, EGLDevice, EGLDisplay},
        input::{ButtonState, KeyState},
        renderer::{
            element::{
                surface::{render_elements_from_surface_tree, WaylandSurfaceRenderElement},
                Kind,
            },
            gles::{GlesRenderbuffer, GlesRenderer},
            utils::{draw_render_elements, on_commit_buffer_handler},
            Bind, Color32F, ExportMem, Frame, ImportDma, Offscreen, Renderer, TextureMapping,
        },
    },
    desktop::{
        find_popup_root_surface, utils::bbox_from_surface_tree, PopupGrab, PopupKeyboardGrab,
        PopupKind, PopupManager, PopupPointerGrab, Window, WindowSurfaceType,
    },
    input::{
        keyboard::FilterResult,
        pointer::{AxisFrame, ButtonEvent, MotionEvent},
        Seat, SeatHandler, SeatState,
    },
    output::{Mode, Output, PhysicalProperties, Scale, Subpixel},
    reexports::{
        wayland_protocols::xdg::{
            decoration::zv1::server::zxdg_toplevel_decoration_v1::Mode as DecorationMode,
            shell::server::xdg_toplevel,
        },
        wayland_server::{
            backend::{ClientData, ClientId, DisconnectReason, GlobalId},
            protocol::{wl_buffer, wl_seat, wl_surface::WlSurface},
            Client, Display, DisplayHandle, ListeningSocket, Resource,
        },
    },
    utils::{IsAlive, Logical, Point, Rectangle, Serial, Transform, SERIAL_COUNTER},
    wayland::{
        buffer::BufferHandler,
        compositor::{
            get_parent, send_surface_state, with_states, with_surface_tree_downward,
            CompositorClientState, CompositorHandler, CompositorState, TraversalAction,
        },
        dmabuf::{DmabufFeedbackBuilder, DmabufGlobal, DmabufHandler, DmabufState, ImportNotifier},
        fractional_scale::{
            with_fractional_scale, FractionalScaleHandler, FractionalScaleManagerState,
        },
        output::{OutputHandler, OutputManagerState},
        pointer_constraints::PointerConstraintsState,
        relative_pointer::RelativePointerManagerState,
        seat::WaylandFocus,
        selection::{
            data_device::{
                request_data_device_client_selection, set_data_device_focus,
                set_data_device_selection, start_dnd, ClientDndGrabHandler, DataDeviceHandler,
                DataDeviceState, ServerDndGrabHandler, SourceMetadata,
            },
            wlr_data_control::{DataControlHandler, DataControlState},
            SelectionHandler, SelectionSource, SelectionTarget,
        },
        shell::xdg::{
            decoration::{XdgDecorationHandler, XdgDecorationState},
            PopupSurface, PositionerState, ToplevelSurface, XdgShellHandler, XdgShellState,
            XdgToplevelSurfaceData,
        },
        shm::{ShmHandler, ShmState},
        viewporter::ViewporterState,
    },
};
use std::{
    collections::{BTreeMap, HashSet},
    fs::{self, OpenOptions},
    io::{Read, Write},
    os::unix::{
        fs::{MetadataExt, OpenOptionsExt},
        io::OwnedFd,
        net::UnixStream,
    },
    path::PathBuf,
    process::{Child, Command},
    sync::{mpsc::SyncSender, Arc},
    time::{Duration, Instant},
};

#[derive(Parser, Debug)]
#[command(version, about)]
struct Args {
    /// Use an SSH tunnel to this loopback listener for the initial prototype.
    #[arg(long, default_value = "127.0.0.1:49090")]
    listen: String,
    #[arg(long, default_value = "waybridge-0")]
    socket: String,
    /// Launch a private rootless Xwayland server for legacy X11 applications.
    #[arg(long)]
    xwayland: bool,
    /// Authentication token file; created with mode 0600 if absent.
    #[arg(long)]
    token_file: Option<PathBuf>,
    /// EGL render node, e.g. /dev/dri/renderD128. Auto-select if omitted.
    #[arg(long)]
    render_node: Option<PathBuf>,
    #[arg(long, default_value_t = 60, value_parser = clap::value_parser!(u32).range(1..=120))]
    fps: u32,
    /// Legacy compatibility option; use --bitrate for H.264.
    #[arg(long, default_value_t = 85, value_parser = clap::value_parser!(u8).range(1..=100))]
    quality: u8,
    #[arg(long, default_value = "us")]
    layout: String,
    /// Media codec. Auto enables NVENC only for viewers advertising H.264/alpha support.
    #[arg(long,default_value="auto",value_parser=["auto","png","h264-nvenc"])]
    codec: String,
    /// Per-stream H.264 bitrate in kilobits/s.
    #[arg(long,default_value_t=30000,value_parser=clap::value_parser!(u32).range(1000..=200000))]
    bitrate: u32,
    /// Log same-clock pipeline stage average/max durations every two seconds.
    #[arg(long)]
    stats: bool,
    /// Optional program to launch in the private Wayland session (after --).
    #[arg(last = true)]
    command: Vec<String>,
}

struct RemoteWindow {
    id: u32,
    icon: app_icon::WindowIcon,
    state: window_semantics::State,
    window: Window,
    dirty: bool,
    announced: bool,
    last_info: Value,
    frame_sequence: u64,
    target: Option<GlesRenderbuffer>,
    target_size: (i32, i32),
    output: Output,
    output_global: GlobalId,
    scale: f64,
    mapped: bool,
    work_area: Rectangle<i32, Logical>,
}

struct RemotePopup {
    id: u32,
    surface: PopupSurface,
    root: u32,
    parent: u32,
    dirty: bool,
    last_info: Value,
    sequence: u64,
    target: Option<GlesRenderbuffer>,
    target_size: (i32, i32),
}

type Clipboard = Arc<BTreeMap<String, Vec<u8>>>;

struct App {
    cursor: cursor::Cursor,
    _cursor_shapes: smithay::wayland::cursor_shape::CursorShapeManagerState,
    icons: app_icon::Icons,
    relative: relative_input::RelativeInput,
    _relative_manager: RelativePointerManagerState,
    _constraints: PointerConstraintsState,
    ime: ime::Ime,
    dh: DisplayHandle,
    compositor: CompositorState,
    x11: x11::State,
    shell: XdgShellState,
    _dialogs: smithay::wayland::shell::xdg::dialog::XdgDialogState,
    _decorations: XdgDecorationState,
    _fractional_scale: FractionalScaleManagerState,
    _viewporter: ViewporterState,
    _outputs: OutputManagerState,
    shm: ShmState,
    seats: SeatState<Self>,
    seat: Seat<Self>,
    data_device: DataDeviceState,
    data_control: DataControlState,
    dmabuf: DmabufState,
    renderer: GlesRenderer,
    output: Output,
    popups: PopupManager,
    remote_popups: Vec<RemotePopup>,
    popup_grab: Option<(u32, PopupGrab<Self>)>,
    windows: Vec<RemoteWindow>,
    next_id: u32,
    peer: Option<Arc<Peer>>,
    events: SyncSender<Event>,
    clipboard_generation: u64,
    clipboard: BTreeMap<String, Vec<u8>>,
    clipboard_reads: Vec<(u64, String)>,
    transfers: transfer::Transfers,
    drop_data: Option<Arc<Vec<u8>>>,
    drop_deadline: Option<Instant>,
    drop_accepted: bool,
    pressed_keys: HashSet<u32>,
    pressed_buttons: HashSet<u32>,
    pointer_window: Option<u32>,
    start: Instant,
    default_scale: f64,
    focused_window: Option<u32>,
    last_interaction: Option<(Serial, u32, Instant)>,
    encoder: video::Encoder,
}

impl App {
    fn control(&self, message: Value) {
        if let Some(peer) = &self.peer {
            peer.control(message);
        }
    }

    fn window(&self, id: u32) -> Option<Window> {
        self.windows
            .iter()
            .find(|w| w.id == id)
            .map(|w| w.window.clone())
    }

    fn root_id(&self, id: u32) -> u32 {
        self.remote_popups
            .iter()
            .find(|p| p.id == id)
            .map_or(id, |p| p.root)
    }

    fn focus(&mut self, id: Option<u32>) {
        let id = id.map(|id| self.root_id(id));
        // Override-redirect menus keep their owning X11 window's keyboard focus.
        let id = id.and_then(|id| {
            if let Some(x) = self.window(id).and_then(|w| w.x11_surface().cloned()) {
                if x.is_override_redirect() {
                    return x
                        .is_transient_for()
                        .and_then(|parent| {
                            self.windows
                                .iter()
                                .find(|w| {
                                    w.window.x11_surface().is_some_and(|x| {
                                        x.window_id() == parent && !x.is_override_redirect()
                                    })
                                })
                                .map(|w| w.id)
                        })
                        .or(self.focused_window);
                }
            }
            Some(id)
        });
        let id = id.map(|id| self.modal_target(id)).filter(|id| self.window(*id).is_some());
        let changed = self.focused_window != id;
        if changed {
            if let Some(root) = self.x11.grab.filter(|root| Some(*root) != id) {
                self.x11_cancel_popup(root);
            }
            self.release_input();
            if let Some(root) = self
                .focused_window
                .and_then(|id| self.window(id))
                .and_then(|w| w.wl_surface().map(|s| s.into_owned()))
            {
                let popups: Vec<_> = PopupManager::popups_for_surface(&root)
                    .map(|(p, _)| p)
                    .collect();
                for popup in popups {
                    let _ = PopupManager::dismiss_popup(&root, &popup);
                }
            }
            let pointer = self.seat.get_pointer().unwrap();
            pointer.unset_grab(self, SERIAL_COUNTER.next_serial(), self.time());
        }
        self.focused_window = id;
        if changed {
            if let Some(x) = id
                .and_then(|id| self.window(id))
                .and_then(|w| w.x11_surface().cloned())
            {
                if let Some(wm) = self.x11.wm.as_mut() {
                    let _ = wm.raise_window(&x);
                }
            }
            self.relative_focus_changed();
        }
        let surface = id
            .and_then(|id| self.windows.iter().find(|w| w.id == id && w.mapped))
            .and_then(|w| {
                if let Some(x) = w.window.x11_surface() {
                    Some(x11::KeyboardFocus::X11(x.clone()))
                } else {
                    w.window
                        .wl_surface()
                        .map(|s| x11::KeyboardFocus::Wayland(s.into_owned()))
                }
            });
        let keyboard = self.seat.get_keyboard().unwrap();
        // An active popup owns keyboard focus until its grab ends.
        if changed && keyboard.is_grabbed() {
            keyboard.unset_grab(self);
        }
        if !keyboard.is_grabbed() {
            keyboard.set_focus(self, surface, SERIAL_COUNTER.next_serial());
        }
        // Smithay 0.7 does not call SeatHandler::focus_changed when focus becomes None.
        self.ime.focus(keyboard.current_focus());
        self.publish_ime();
        for w in &self.windows {
            if w.window.set_activated(Some(w.id) == id) {
                if let Some(top) = w.window.toplevel() {
                    top.send_pending_configure();
                }
            }
        }
    }

    fn set_scale(&mut self, id: u32, scale: f64, mode: Option<Mode>) {
        let scale = scale.clamp(1.0, 4.0);
        let Some(remote) = self.windows.iter_mut().find(|w| w.id == id) else {
            return;
        };
        self.cursor.dirty = true;
        remote.scale = scale;
        remote.dirty = true;
        for popup in self.remote_popups.iter_mut().filter(|p| p.root == id) {
            popup.dirty = true;
        }
        remote
            .output
            .change_current_state(mode, None, Some(Scale::Fractional(scale)), None);
        let Some(root) = remote.window.wl_surface().map(|s| s.into_owned()) else {
            return;
        };
        set_surface_scale(&root, &remote.output, scale);
        for (popup, _) in PopupManager::popups_for_surface(&root) {
            set_surface_scale(popup.wl_surface(), &remote.output, scale);
        }
    }

    fn root_for(&self, surface: &WlSurface) -> WlSurface {
        let mut root = surface.clone();
        while let Some(parent) = get_parent(&root) {
            root = parent;
        }
        if let Some(popup) = self.popups.find_popup(&root) {
            if let Ok(parent) = find_popup_root_surface(&popup) {
                root = parent;
            }
        }
        root
    }

    fn pointer_motion(&mut self, id: u32, x: f64, y: f64) {
        self.sync_pointer_lock();
        if self.pointer_locked() { return; }
        if !self.input_allowed(id) || !x.is_finite() || !y.is_finite() {
            return;
        }
        let Some(window) = self.window(self.root_id(id)) else {
            return;
        };
        let mut position: Point<f64, Logical> = (x, y).into();
        if let Some(popup) = self.remote_popups.iter().find(|p| p.id == id) {
            let bbox = bbox_from_surface_tree(popup.surface.wl_surface(), (0, 0));
            let offset = popup_location(&window, &popup.surface) + bbox.loc;
            position += offset.to_f64();
        } else {
            position += window.geometry().loc.to_f64();
        }
        let mut hit = window
            .surface_under(position, WindowSurfaceType::ALL)
            .map(|(surface, origin)| (surface, origin.to_f64()));
        // Windows captures menu mouse events on the owner. X11 menus are independent
        // rootless surfaces, so reproduce their hit test before handing input to Xwayland.
        if let Some(root) = window.x11_surface().filter(|_| self.x11.grab == Some(id)) {
            hit = window
                .wl_surface()
                .map(|s| (s.into_owned(), (0.0, 0.0).into()));
            let root_location = self
                .x11
                .geometries
                .get(&root.window_id())
                .map_or(root.geometry().loc, |g| g.loc);
            for popup in self.windows.iter().rev() {
                let Some(x) = popup.window.x11_surface().filter(|x| {
                    x11::input_popup(x) && popup.last_info["root"].as_u64() == Some(u64::from(id))
                }) else {
                    continue;
                };
                let location = self
                    .x11
                    .geometries
                    .get(&x.window_id())
                    .map_or(x.geometry().loc, |g| g.loc)
                    - root_location;
                let local = position - location.to_f64();
                if let Some((surface, origin)) =
                    popup.window.surface_under(local, WindowSurfaceType::ALL)
                {
                    hit = Some((surface, (location + origin).to_f64()));
                    break;
                }
            }
        }
        if self.pointer_window != Some(id) {
            self.cursor.dirty = true;
        }
        self.relative_hit(hit.clone());
        self.pointer_window = Some(id);
        let pointer = self.seat.get_pointer().unwrap();
        pointer.motion(
            self,
            hit,
            &MotionEvent {
                location: position,
                serial: SERIAL_COUNTER.next_serial(),
                time: self.time(),
            },
        );
        pointer.frame(self);
        self.sync_pointer_lock();
    }

    fn message_position(&self, id: u32, value: &Value) -> (f64, f64) {
        let mut x = value["x"].as_f64().unwrap_or(0.0);
        let mut y = value["y"].as_f64().unwrap_or(0.0);
        let origin = self
            .remote_popups
            .iter()
            .find(|p| p.id == id)
            .map(|p| bbox_from_surface_tree(p.surface.wl_surface(), (0, 0)).loc)
            .or_else(|| self.window(id).map(|w| w.geometry().loc));
        if let (Some(ox), Some(oy), Some(origin)) = (
            value["origin_x"].as_f64(),
            value["origin_y"].as_f64(),
            origin,
        ) {
            x += ox - origin.x as f64;
            y += oy - origin.y as f64;
        }
        (x, y)
    }

    fn pointer_message(&mut self, id: u32, value: &Value) {
        let (x, y) = self.message_position(id, value);
        self.pointer_motion(id, x, y);
    }

    fn time(&self) -> u32 {
        self.start.elapsed().as_millis() as u32
    }

    fn begin_file_drop(&mut self, id: u32, x: f64, y: f64, uris: Vec<u8>) {
        if !self.input_allowed(id) {
            return;
        }
        self.release_input();
        self.focus(Some(id));
        self.pointer_motion(id, x, y);
        let pointer = self.seat.get_pointer().unwrap();
        let serial = SERIAL_COUNTER.next_serial();
        pointer.button(
            self,
            &ButtonEvent {
                button: 0x110,
                state: ButtonState::Pressed,
                serial,
                time: self.time(),
            },
        );
        self.pressed_buttons.insert(0x110);
        let Some(start) = pointer.grab_start_data() else {
            self.release_input();
            return;
        };
        self.drop_data = Some(Arc::new(uris));
        self.drop_accepted = false;
        self.drop_deadline = Some(Instant::now() + Duration::from_secs(5));
        let dh = self.dh.clone();
        let seat = self.seat.clone();
        start_dnd(&dh,&seat,self,serial,Some(start),None,SourceMetadata{mime_types:vec!["text/uri-list".into()],dnd_action:smithay::reexports::wayland_server::protocol::wl_data_device_manager::DndAction::Copy});
        self.pointer_motion(id, x, y);
    }

    fn finish_file_drop(&mut self) {
        if self
            .drop_deadline
            .is_some_and(|deadline| self.drop_accepted || Instant::now() >= deadline)
        {
            self.drop_deadline = None;
            self.pressed_buttons.remove(&0x110);
            let pointer = self.seat.get_pointer().unwrap();
            pointer.button(
                self,
                &ButtonEvent {
                    button: 0x110,
                    state: ButtonState::Released,
                    serial: SERIAL_COUNTER.next_serial(),
                    time: self.time(),
                },
            );
            pointer.frame(self);
        }
    }

    fn read_pending_clipboard(&mut self) {
        // Smithay calls new_selection before installing the new provider on the seat.
        // Read only after dispatch_clients returns, otherwise we read the previous owner.
        for (generation, mime) in std::mem::take(&mut self.clipboard_reads) {
            if generation != self.clipboard_generation {
                continue;
            }
            let Ok((mut read, write)) = UnixStream::pair() else {
                continue;
            };
            let _ = read.set_read_timeout(Some(Duration::from_secs(10)));
            if request_data_device_client_selection::<Self>(&self.seat, mime.clone(), write.into())
                .is_err()
            {
                continue;
            }
            let tx = self.events.clone();
            std::thread::spawn(move || {
                let mut bytes = Vec::new();
                if (&mut read)
                    .take(12 * 1024 * 1024 + 1)
                    .read_to_end(&mut bytes)
                    .is_ok()
                    && bytes.len() <= 12 * 1024 * 1024
                {
                    let _ = tx.send(Event::Clipboard {
                        generation,
                        mime,
                        bytes,
                    });
                }
            });
        }
    }

    fn release_input(&mut self) {
        let keyboard = self.seat.get_keyboard().unwrap();
        for key in std::mem::take(&mut self.pressed_keys) {
            keyboard.input::<(), _>(
                self,
                (key + 8).into(),
                KeyState::Released,
                SERIAL_COUNTER.next_serial(),
                self.time(),
                |_, _, _| FilterResult::Forward,
            );
        }
        let pointer = self.seat.get_pointer().unwrap();
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
        self.unlock_pointer(true);
        self.pointer_window = None;
        self.cursor.dirty = true;
        self.drop_deadline = None;
        self.drop_accepted = false;
    }

    fn message(&mut self, value: Value) {
        let id = value["id"]
            .as_u64()
            .and_then(|n| u32::try_from(n).ok())
            .unwrap_or(0);
        match value["type"].as_str().unwrap_or("") {
            "window_state" => self.native_window_state(id, &value),
            "ime" => self.ime_message(&value),
            "frame_ack" => {
                if let Some(peer) = &self.peer {
                    peer.acknowledge(id, value["sequence"].as_u64().unwrap_or(0));
                }
            }
            "ping" => self.control(json!({"type":"pong","nonce":value["nonce"]})),
            "hello" => {
                self.relative_available(value["pointer_lock"].as_bool().unwrap_or(false));
                self.default_scale = dpi_scale(&value);
                self.x11_scale(self.default_scale);
                let mode = monitor_mode(&value, self.output.current_mode().unwrap().refresh);
                self.output.change_current_state(
                    mode,
                    None,
                    Some(Scale::Fractional(self.default_scale)),
                    None,
                );
                for id in self.windows.iter().map(|w| w.id).collect::<Vec<_>>() {
                    self.set_scale(id, self.default_scale, mode);
                }
            }
            "focus" => {
                if value["focused"].as_bool().unwrap_or(false) {
                    self.focus(Some(id));
                } else if self.focused_window == Some(id) {
                    self.focus(None);
                }
            }
            "pointer" => self.pointer_message(id, &value),
            "relative_pointer" => self.relative_motion(id, &value),
            "pointer_lock_ack" => self.pointer_lock_ack(id, &value),
            "leave" if self.pointer_window == Some(id) && !self.pointer_locked() => {
                self.relative_hit(None);
                self.pointer_window = None;
                self.cursor.dirty = true;
                let p = self.seat.get_pointer().unwrap();
                p.motion(
                    self,
                    None,
                    &MotionEvent {
                        location: p.current_location(),
                        serial: SERIAL_COUNTER.next_serial(),
                        time: self.time(),
                    },
                );
                p.frame(self);
            }
            "button" if self.input_allowed(id) => {
                if value["x"].is_number() && value["y"].is_number() {
                    self.pointer_message(id, &value);
                }
                let button = value["button"].as_u64().unwrap_or(0) as u32;
                if !(0x110..=0x114).contains(&button) {
                    return;
                }
                let down = value["down"].as_bool().unwrap_or(false);
                if down && self.window(self.root_id(id)).is_none() {
                    return;
                }
                if !down && !self.pressed_buttons.contains(&button) {
                    return;
                }
                if down {
                    self.focus(Some(id));
                    self.pressed_buttons.insert(button);
                } else {
                    self.pressed_buttons.remove(&button);
                }
                let pointer = self.seat.get_pointer().unwrap();
                let serial = SERIAL_COUNTER.next_serial();
                if down {
                    self.last_interaction = Some((serial, self.root_id(id), Instant::now()));
                }
                pointer.button(
                    self,
                    &ButtonEvent {
                        button,
                        state: if down {
                            ButtonState::Pressed
                        } else {
                            ButtonState::Released
                        },
                        serial,
                        time: self.time(),
                    },
                );
                pointer.frame(self);
            }
            "key" if self.input_allowed(id) && self.focused_window == Some(self.root_id(id)) => {
                let key = value["key"].as_u64().unwrap_or(u64::MAX);
                if key > 767 {
                    return;
                }
                let key = key as u32;
                let down = value["down"].as_bool().unwrap_or(false);
                let changed = if down {
                    self.pressed_keys.insert(key)
                } else {
                    self.pressed_keys.remove(&key)
                };
                if changed {
                    let k = self.seat.get_keyboard().unwrap();
                    let serial = SERIAL_COUNTER.next_serial();
                    if down {
                        self.last_interaction = Some((serial, self.root_id(id), Instant::now()));
                    }
                    k.input::<(), _>(
                        self,
                        (key + 8).into(),
                        if down {
                            KeyState::Pressed
                        } else {
                            KeyState::Released
                        },
                        serial,
                        self.time(),
                        |_, _, _| FilterResult::Forward,
                    );
                }
            }
            "scroll" if self.input_allowed(id) && self.window(self.root_id(id)).is_some() => {
                let x = value["x"].as_f64().unwrap_or(0.0).clamp(-1000.0, 1000.0);
                let y = value["y"].as_f64().unwrap_or(0.0).clamp(-1000.0, 1000.0);
                let p = self.seat.get_pointer().unwrap();
                use smithay::backend::input::{Axis, AxisSource};
                let frame = AxisFrame::new(self.time())
                    .source(AxisSource::Wheel)
                    .value(Axis::Horizontal, x)
                    .value(Axis::Vertical, y);
                p.axis(self, frame);
                p.frame(self);
            }
            "viewport" => self.update_viewport(id, &value),
            "resize" if self.windows.iter().any(|w| w.id == id && w.mapped) => {
                let started = Instant::now();
                self.update_viewport(id, &value);
                self.set_scale(
                    id,
                    dpi_scale(&value),
                    monitor_mode(&value, self.output.current_mode().unwrap().refresh),
                );
                if let Some(window) = self.window(id) {
                    let (width, height) = if self.semantics_enabled() {
                        self.native_resize(id, &value).unwrap()
                    } else {
                        (
                            value["width"].as_i64().unwrap_or(960).clamp(64, 3840) as i32,
                            value["height"].as_i64().unwrap_or(640).clamp(64, 2160) as i32,
                        )
                    };
                    if let Some(peer) = &self.peer {
                        let scale = dpi_scale(&value);
                        peer.begin_resize(
                            id,
                            (f64::from(width) * scale).round() as u32,
                            (f64::from(height) * scale).round() as u32,
                            started,
                        );
                    }
                    if let Some(x) = window.x11_surface() {
                        let mut geometry = x.geometry();
                        geometry.size = (width, height).into();
                        let _ = x.configure(geometry);
                        if let Some(maximized) = value["maximized"].as_bool() {
                            let _ = x.set_maximized(maximized);
                        }
                        return;
                    }
                    let top = window.toplevel().unwrap();
                    top.with_pending_state(|s| {
                        s.size = Some((width, height).into());
                        if let Some(maximized) = value["maximized"].as_bool() {
                            if maximized && !s.states.contains(xdg_toplevel::State::Fullscreen) {
                                s.states.set(xdg_toplevel::State::Maximized);
                            } else {
                                s.states.unset(xdg_toplevel::State::Maximized);
                            }
                        }
                    });
                    top.send_pending_configure();
                }
            }
            "codec_fallback" => {
                if let Some(peer) = &self.peer {
                    peer.fallback(id);
                }
                if let Some(w) = self.windows.iter_mut().find(|w| w.id == id) {
                    w.dirty = true;
                }
                self.encoder.forget(id);
            }
            "popup_cancel" => {
                if let Some((root, _)) = &self.popup_grab {
                    if *root == id {
                        if let Some(surface) = self
                            .window(id)
                            .and_then(|w| w.wl_surface().map(|s| s.into_owned()))
                        {
                            for (p, _) in
                                PopupManager::popups_for_surface(&surface).collect::<Vec<_>>()
                            {
                                let _ = PopupManager::dismiss_popup(&surface, &p);
                            }
                        }
                        self.release_input();
                    }
                }
                self.x11_cancel_popup(id);
            }
            "cancel_input"
                if self.focused_window == Some(id) || self.pointer_window == Some(id) =>
            {
                self.release_input()
            }
            "close" => {
                if self.focused_window == Some(id) {
                    self.release_input();
                }
                if let Some(window) = self.window(id) {
                    if let Some(top) = window.toplevel() {
                        top.send_close();
                    }
                    if let Some(x) = window.x11_surface() {
                        let _ = x.close();
                    }
                }
            }
            "clipboard" => {
                let mut contents = BTreeMap::new();
                if let Some(items) = value["items"].as_object() {
                    for (mime, data) in items {
                        if !matches!(
                            mime.as_str(),
                            "text/plain;charset=utf-8" | "text/plain" | "image/png"
                        ) {
                            continue;
                        }
                        if let Some(text) = data.as_str() {
                            if text.len() <= 16 * 1024 * 1024 {
                                if let Ok(bytes) = B64.decode(text) {
                                    contents.insert(mime.clone(), bytes);
                                }
                            }
                        }
                    }
                }
                self.clipboard_generation += 1; // discard reads from a superseded Linux selection
                self.clipboard = contents.clone();
                self.x11.clipboard_generation = None;
                if let Some(wm) = self.x11.wm.as_mut() {
                    let _ = wm.new_selection(
                        SelectionTarget::Clipboard,
                        Some(contents.keys().cloned().collect()),
                    );
                }
                let seat = self.seat.clone();
                set_data_device_selection(
                    &self.dh,
                    &seat,
                    contents.keys().cloned().collect(),
                    Arc::new(contents),
                );
            }
            "refresh" => {
                for window in &mut self.windows {
                    window.announced = false;
                    window.dirty = true;
                }
            }
            "upload_begin" | "upload_chunk" | "upload_end" | "upload_commit" => {
                let transfer_id = value["transfer"].as_u64().unwrap_or(0);
                let result = (|| -> Result<()> {
                    match value["type"].as_str().unwrap() {
                        "upload_begin" => self.transfers.begin(
                            transfer_id,
                            value["name"].as_str().context("missing name")?,
                            value["size"].as_u64().context("missing size")?,
                        )?,
                        "upload_chunk" => self.transfers.chunk(
                            transfer_id,
                            value["data"].as_str().context("missing chunk")?,
                        )?,
                        "upload_end" => self.transfers.end(transfer_id)?,
                        "upload_commit" => {
                            let ids: Vec<u64> = serde_json::from_value(value["transfers"].clone())?;
                            let uris = self.transfers.finish(&ids)?;
                            if value["drop"].as_bool() == Some(true) {
                                let (x, y) = self.message_position(id, &value);
                                self.begin_file_drop(id, x, y, uris);
                                self.control(json!({"type":"upload_ready","files":ids.len()}));
                                return Ok(());
                            }
                            let contents = BTreeMap::from([("text/uri-list".to_string(), uris)]);
                            self.clipboard_generation += 1;
                            self.clipboard = contents.clone();
                            self.x11.clipboard_generation = None;
                            if let Some(wm) = self.x11.wm.as_mut() {
                                let _ = wm.new_selection(
                                    SelectionTarget::Clipboard,
                                    Some(vec!["text/uri-list".into()]),
                                );
                            }
                            set_data_device_selection(
                                &self.dh,
                                &self.seat,
                                vec!["text/uri-list".into()],
                                Arc::new(contents),
                            );
                            self.control(json!({"type":"upload_ready","files":ids.len()}));
                        }
                        _ => {}
                    }
                    Ok(())
                })();
                if let Err(error) = result {
                    self.transfers.cancel();
                    self.control(
                        json!({"type":"error","message":format!("File transfer: {error}")}),
                    );
                }
            }
            _ => {}
        }
    }

    fn render(&mut self) -> Result<()> {
        self.popups.cleanup();
        if self
            .popup_grab
            .as_ref()
            .is_some_and(|(_, grab)| grab.has_ended())
        {
            if let Some((root, _)) = self.popup_grab.take() {
                self.control(json!({"type":"popup_grab","root":root,"active":false}));
            }
        }
        let dead_popups: Vec<_> = self
            .remote_popups
            .iter()
            .filter(|p| !p.surface.alive() || self.window(p.root).is_none())
            .map(|p| p.id)
            .collect();
        for id in dead_popups {
            self.remote_popups.retain(|p| p.id != id);
            self.encoder.forget(id);
            if let Some(peer) = &self.peer {
                peer.forget_window(id);
            }
            self.control(json!({"type":"window_closed","id":id}));
        }
        let dead: Vec<_> = self
            .windows
            .iter()
            .filter(|w| !w.window.alive())
            .map(|w| w.id)
            .collect();
        for id in dead {
            if let Some(top) = self.window(id).and_then(|w| w.toplevel().cloned()) {
                self.reparent_children(top.wl_surface(), top.parent());
            }
            if self.focused_window == Some(id) {
                let parent = self
                    .windows
                    .iter()
                    .find(|w| w.id == id)
                    .and_then(|w| w.last_info["parent"].as_u64())
                    .and_then(|p| u32::try_from(p).ok())
                    .filter(|parent| {
                        self.windows
                            .iter()
                            .any(|w| w.id == *parent && w.window.alive())
                    });
                self.focus(parent);
            }
            if let Some(remote) = self.windows.iter().find(|w| w.id == id) {
                self.dh.remove_global::<App>(remote.output_global.clone());
            }
            self.windows.retain(|w| w.id != id);
            self.icons.forget(id);
            self.encoder.forget(id);
            if let Some(peer) = &self.peer {
                peer.forget_window(id);
            }
            self.control(json!({"type":"window_closed", "id":id}));
        }
        let semantics = self.semantics_enabled();
        let parent_ids: Vec<_> = self
            .windows
            .iter()
            .filter_map(|w| w.window.wl_surface().map(|s| (s.into_owned(), w.id)))
            .collect();
        let x11_windows: Vec<_> = self
            .windows
            .iter()
            .filter_map(|w| w.window.x11_surface().map(|x| (x.clone(), w.id)))
            .collect();
        let focused = self.focused_window;
        for (index, remote) in self.windows.iter_mut().enumerate() {
            let Some(surface) = remote.window.wl_surface().map(|s| s.into_owned()) else {
                continue;
            };
            let bbox = stream_geometry(&remote.window);
            let size = bbox.size;
            let pixels: smithay::utils::Size<i32, smithay::utils::Physical> =
                size.to_physical_precise_round(remote.scale);
            tracing::trace!(
                id = remote.id,
                ?size,
                ?pixels,
                mapped = remote.mapped,
                dirty = remote.dirty,
                "render candidate"
            );
            // Bound both dimensions and total allocation before touching a framebuffer.
            if !remote.mapped
                || pixels.w <= 0
                || pixels.h <= 0
                || pixels.w > 8192
                || pixels.h > 8192
                || pixels.w as i64 * pixels.h as i64 > 16_777_216
            {
                continue;
            }
            let mut info = if let Some(x) = remote.window.x11_surface() {
                x11::info(
                    x,
                    remote.id,
                    size,
                    remote.scale,
                    &x11_windows,
                    &self.x11.geometries,
                    focused,
                )
            } else {
                let top = remote.window.toplevel().unwrap();
                let native_decoration =
                    top.current_state().decoration_mode == Some(DecorationMode::ServerSide);
                let parent_id = top
                    .parent()
                    .and_then(|p| parent_ids.iter().find(|(s, _)| s == &p).map(|(_, id)| *id));
                with_states(top.wl_surface(), |states| {
                    let s = states
                        .data_map
                        .get::<XdgToplevelSurfaceData>()
                        .unwrap()
                        .lock()
                        .unwrap();
                    json!({"type":"window", "id":remote.id, "title":s.title.clone().unwrap_or_default(), "app_id":s.app_id.clone().unwrap_or_default(), "width":size.w, "height":size.h,
                    "scale": remote.scale,
                    "parent": parent_id,
                    "decoration":if native_decoration {"server"}else{"client"}})
                })
            };
            if semantics {
                remote.state.input = window_semantics::input_region(&surface,bbox);
                if let Some(top)=remote.window.toplevel() {
                    let (min,max)=window_semantics::limits(top.wl_surface());
                    info["min_size"]=json!(min);info["max_size"]=json!(max);
                    info["modal"]=with_states(top.wl_surface(),|s|json!(s.data_map.get::<XdgToplevelSurfaceData>().unwrap().lock().unwrap().modal));
                } else if let Some(x)=remote.window.x11_surface() {
                    info["min_size"]=json!(x.min_size().map(|s|[s.w,s.h]).unwrap_or([0,0]));
                    info["max_size"]=json!(x.max_size().map(|s|[s.w,s.h]).unwrap_or([0,0]));
                    info["modal"]=json!(x.is_popup());
                }
                info["fullscreen"]=json!(remote.state.fullscreen);
                info["fullscreen_epoch"]=json!(remote.state.fullscreen_epoch);
                info["mapping_epoch"]=json!(remote.state.mapping_epoch);
                info["input_region"]=remote.state.input.clone();
            }
            remote.icon.observe(info["app_id"].as_str().unwrap_or_default());
            info["icon_revision"]=json!(remote.icon.revision);
            if let Some(peer) = &self.peer {
                if !remote.announced || remote.last_info != info {
                    peer.control(info.clone());
                }
                if info["type"] == "popup" && info["input_popup"] == true {
                    if let Some(root) = info["root"].as_u64().and_then(|n| u32::try_from(n).ok()) {
                        if self.x11.grab != Some(root) {
                            if let Some(old) = self.x11.grab.replace(root) {
                                peer.control(
                                    json!({"type":"popup_grab","root":old,"active":false}),
                                );
                            }
                            peer.control(json!({"type":"popup_grab","root":root,"active":true}));
                        }
                    }
                }
                remote.last_info = info;
                remote.announced = true;
                if peer.icons_enabled() && index < 128 {
                    if !remote.icon.requested && !remote.icon.ready {
                        remote.icon.requested = self.icons.request(
                            remote.id,
                            remote.icon.revision,
                            &remote.icon.app_id,
                        );
                    }
                    if remote.icon.ready && !remote.icon.sent {
                        remote.icon.sent =
                            peer.window_icon(remote.id, remote.icon.message(remote.id));
                    }
                }
                if remote.dirty && (!semantics || remote.state.visible) {
                    remote.frame_sequence += 1;
                    let mut frame = capture_surface(
                        &mut self.renderer,
                        &surface,
                        remote.id,
                        remote.frame_sequence,
                        bbox,
                        remote.scale,
                        &mut remote.target,
                        &mut remote.target_size,
                        peer.clone(),
                    )?;
                    frame.video_eligible = remote.last_info["type"] == "window";
                    self.encoder.submit(frame);
                }
            }
            remote.dirty = false;
            // Continue frame callbacks even with no viewer, so clients never deadlock.
            remote.window.send_frame(
                &remote.output,
                self.start.elapsed(),
                Some(if semantics && !remote.state.visible {
                    Duration::from_millis(250)
                } else {
                    Duration::ZERO
                }),
                |_, _| {
                    if semantics && !remote.state.visible {
                        None
                    } else {
                        Some(remote.output.clone())
                    }
                },
            );
        }
        self.refresh_x11_grab();
        for popup in &mut self.remote_popups {
            let Some(root) = self.windows.iter().find(|w| w.id == popup.root) else {
                continue;
            };
            let bbox = bbox_from_surface_tree(popup.surface.wl_surface(), (0, 0));
            if bbox.size.w <= 0 || bbox.size.h <= 0 {
                continue;
            }
            let location = popup_location(&root.window, &popup.surface) + bbox.loc
                - root.window.geometry().loc;
            let mut info = json!({"type":"popup","id":popup.id,"root":popup.root,"parent":popup.parent,
                "x":location.x,"y":location.y,"width":bbox.size.w,"height":bbox.size.h,"scale":root.scale});
            if semantics {
                info["input_region"] =
                    window_semantics::input_region(popup.surface.wl_surface(), bbox);
            }
            if let Some(peer) = &self.peer {
                if popup.last_info != info {
                    peer.control(info.clone());
                    popup.last_info = info;
                }
                if popup.dirty && (!semantics || root.state.visible) {
                    popup.sequence += 1;
                    let frame = capture_surface(
                        &mut self.renderer,
                        popup.surface.wl_surface(),
                        popup.id,
                        popup.sequence,
                        bbox,
                        root.scale,
                        &mut popup.target,
                        &mut popup.target_size,
                        peer.clone(),
                    )?;
                    self.encoder.submit(frame);
                }
            }
            popup.dirty = false;
        }
        self.render_cursor();
        Ok(())
    }
}

fn stream_geometry(window: &Window) -> Rectangle<i32, Logical> {
    if window.is_x11() {
        if let Some(surface) = window.wl_surface() {
            return bbox_from_surface_tree(&surface, (0, 0));
        }
    }
    window.geometry()
}

fn popup_location(window: &Window, popup: &PopupSurface) -> Point<i32, Logical> {
    let root = window.wl_surface().unwrap();
    let location = PopupManager::popups_for_surface(&root)
        .find(|(p, _)| p.wl_surface() == popup.wl_surface())
        .map_or((0, 0).into(), |(_, p)| p);
    window.geometry().loc + location - PopupKind::Xdg(popup.clone()).geometry().loc
}

#[allow(clippy::too_many_arguments)]
fn capture_surface(
    renderer: &mut GlesRenderer,
    surface: &WlSurface,
    id: u32,
    sequence: u64,
    bbox: Rectangle<i32, Logical>,
    scale: f64,
    target: &mut Option<GlesRenderbuffer>,
    target_size: &mut (i32, i32),
    peer: Arc<Peer>,
) -> Result<video::Frame> {
    let size = bbox.size;
    let pixels = size.to_physical_precise_round(scale);
    if pixels.w <= 0
        || pixels.h <= 0
        || pixels.w > 8192
        || pixels.h > 8192
        || pixels.w as i64 * pixels.h as i64 > 16_777_216
    {
        bail!("surface {id} exceeds render allocation limit");
    }
    if *target_size != (pixels.w, pixels.h) {
        *target = Some(renderer.create_buffer(Fourcc::Abgr8888, (pixels.w, pixels.h).into())?);
        *target_size = (pixels.w, pixels.h);
    }
    let started = Instant::now();
    let elements: Vec<WaylandSurfaceRenderElement<GlesRenderer>> =
        render_elements_from_surface_tree(
            renderer,
            surface,
            Point::from((-bbox.loc.x, -bbox.loc.y)).to_physical_precise_round(scale),
            scale,
            1.0,
            Kind::Unspecified,
        );
    let mut bound = renderer.bind(target.as_mut().unwrap())?;
    let damage = Rectangle::from_size(pixels);
    {
        let mut frame = renderer.render(&mut bound, pixels, Transform::Flipped180)?;
        frame.clear(Color32F::new(0., 0., 0., 0.), &[damage])?;
        draw_render_elements(&mut frame, scale, &elements, &[damage])?;
        frame.finish()?.wait()?;
    }
    metrics::record("composite", started.elapsed());
    let started = Instant::now();
    let mapping = renderer.copy_framebuffer(
        &bound,
        Rectangle::from_size((pixels.w, pixels.h).into()),
        Fourcc::Abgr8888,
    )?;
    let flipped = mapping.flipped();
    let bytes = renderer.map_texture(&mapping)?;
    let stride = pixels.w as usize * 4;
    let mut rgba = vec![0; stride * pixels.h as usize];
    for row in 0..pixels.h as usize {
        let source = if flipped {
            pixels.h as usize - 1 - row
        } else {
            row
        };
        rgba[row * stride..(row + 1) * stride]
            .copy_from_slice(&bytes[source * stride..(source + 1) * stride]);
    }
    metrics::record("readback", started.elapsed());
    Ok(video::Frame {
        id,
        width: pixels.w as u32,
        height: pixels.h as u32,
        sequence,
        geometry: [size.w, size.h, bbox.loc.x, bbox.loc.y],
        rgba,
        peer,
        video_eligible: false,
        enqueued: Instant::now(),
    })
}

impl App {
    fn window_action(&self, surface: &ToplevelSurface, action: &str, edge: Option<u32>) {
        if let Some(window) = self
            .windows
            .iter()
            .find(|w| w.window.toplevel() == Some(surface))
        {
            self.control(
                json!({"type":"window_action","id":window.id,"action":action,"edge":edge,"fullscreen_epoch":window.state.fullscreen_epoch}),
            );
        }
    }
    fn has_pointer_grab(
        &self,
        surface: &ToplevelSurface,
        seat: &wl_seat::WlSeat,
        serial: Serial,
    ) -> bool {
        Seat::<Self>::from_resource(seat).is_some_and(|s| s == self.seat)
            && self.seat.get_pointer().is_some_and(|p| {
                p.has_grab(serial)
                    && p.current_focus()
                        .is_some_and(|focus| focus.id().same_client_as(&surface.wl_surface().id()))
            })
    }
}

impl XdgDecorationHandler for App {
    fn new_decoration(&mut self, surface: ToplevelSurface) {
        self.request_mode(surface, DecorationMode::ServerSide);
    }
    fn request_mode(&mut self, surface: ToplevelSurface, _: DecorationMode) {
        // The actual server-side decoration is drawn by the native Windows client.
        surface.with_pending_state(|s| s.decoration_mode = Some(DecorationMode::ServerSide));
        surface.send_pending_configure();
    }
    fn unset_mode(&mut self, surface: ToplevelSurface) {
        self.request_mode(surface, DecorationMode::ServerSide);
    }
}

impl CompositorHandler for App {
    fn compositor_state(&mut self) -> &mut CompositorState {
        &mut self.compositor
    }
    fn client_compositor_state<'a>(&self, client: &'a Client) -> &'a CompositorClientState {
        if let Some(x) = client.get_data::<smithay::xwayland::XWaylandClientData>() {
            &x.compositor_state
        } else {
            &client.get_data::<ClientState>().unwrap().compositor
        }
    }
    fn commit(&mut self, surface: &WlSurface) {
        on_commit_buffer_handler::<Self>(surface);
        if let smithay::input::pointer::CursorImageStatus::Surface(cursor) = &self.cursor.image {
            let mut root = surface.clone();
            while let Some(parent) = get_parent(&root) {
                root = parent;
            }
            if &root == cursor {
                self.cursor.dirty = true;
            }
        }
        self.popups.commit(surface);
        let semantics = self.semantics_enabled();
        if let Some(PopupKind::Xdg(p)) = self.popups.find_popup(surface) {
            if !p.is_initial_configure_sent() {
                let _ = p.send_configure();
            }
        }
        let mut surface_root = surface.clone();
        while let Some(parent) = get_parent(&surface_root) {
            surface_root = parent;
        }
        if let Some(popup) = self
            .remote_popups
            .iter_mut()
            .find(|p| p.surface.wl_surface() == &surface_root)
        {
            popup.dirty = true;
            return;
        }
        let root = self.root_for(surface);
        let mut newly_mapped_focus = None;
        let mut unmapped = None;
        for remote in &mut self.windows {
            if remote.window.wl_surface().as_deref() != Some(&root) {
                continue;
            }
            remote.window.on_commit();
            remote.dirty = true;
            let mapped = remote.window.bbox().size.w > 0 && remote.window.bbox().size.h > 0;
            if mapped != remote.mapped && self.focused_window == Some(remote.id) {
                newly_mapped_focus = Some(remote.id);
            }
            tracing::debug!(id=remote.id, mapped, bbox=?remote.window.bbox(), "surface commit");
            if mapped != remote.mapped {
                remote.state.mapping_epoch += 1;
                if !mapped {
                    remote.state.fullscreen(false);
                    remote.target = None;
                    remote.target_size = (0, 0);
                    unmapped = Some(remote.id);
                }
                if mapped {
                    remote.state.visible = true;
                    let top = remote.window.toplevel().unwrap();
                    top.with_pending_state(|s| {
                        s.states.unset(xdg_toplevel::State::Suspended);
                    });
                    top.send_pending_configure();
                }
                if semantics && remote.announced {
                    self.peer.as_ref().unwrap().control(json!({"type":"window_visibility","id":remote.id,"mapped":mapped,"mapping_epoch":remote.state.mapping_epoch,"fullscreen":remote.state.fullscreen,"fullscreen_epoch":remote.state.fullscreen_epoch}));
                }
            }
            remote.mapped = mapped;
            if unmapped != Some(remote.id)
                && !remote
                    .window
                    .toplevel()
                    .unwrap()
                    .is_initial_configure_sent()
            {
                remote.window.toplevel().unwrap().send_configure();
            }
            set_surface_scale(surface, &remote.output, remote.scale);
        }
        if let Some(id) = unmapped {
            self.unmap_toplevel(id);
        }
        if let Some(id) = newly_mapped_focus {
            self.focus(Some(id));
        }
    }
}
impl BufferHandler for App {
    fn buffer_destroyed(&mut self, _: &wl_buffer::WlBuffer) {}
}
impl ShmHandler for App {
    fn shm_state(&self) -> &ShmState {
        &self.shm
    }
}
impl DmabufHandler for App {
    fn dmabuf_state(&mut self) -> &mut DmabufState {
        &mut self.dmabuf
    }
    fn dmabuf_imported(&mut self, _: &DmabufGlobal, dmabuf: Dmabuf, notifier: ImportNotifier) {
        if self.renderer.import_dmabuf(&dmabuf, None).is_ok() {
            let _ = notifier.successful::<Self>();
        } else {
            notifier.failed();
        }
    }
}
impl OutputHandler for App {}
impl XdgShellHandler for App {
    fn xdg_shell_state(&mut self) -> &mut XdgShellState {
        &mut self.shell
    }
    fn new_toplevel(&mut self, surface: ToplevelSurface) {
        surface.with_pending_state(|s| {
            s.size = Some((960, 640).into());
            s.states.set(xdg_toplevel::State::Activated);
        });
        surface.send_configure();
        let id = self.next_id;
        self.next_id += 1;
        let window_scale = surface
            .parent()
            .and_then(|p| {
                self.windows
                    .iter()
                    .find(|w| w.window.wl_surface().as_deref() == Some(&p))
            })
            .map_or(self.default_scale, |w| w.scale);
        let output = Output::new(format!("Waybridge-{id}"), self.output.physical_properties());
        let output_global = output.create_global::<App>(&self.dh);
        let mode = surface
            .parent()
            .and_then(|p| {
                self.windows
                    .iter()
                    .find(|w| w.window.wl_surface().as_deref() == Some(&p))
            })
            .and_then(|w| w.output.current_mode())
            .unwrap_or_else(|| self.output.current_mode().unwrap());
        output.change_current_state(
            Some(mode),
            Some(Transform::Normal),
            Some(Scale::Fractional(window_scale)),
            Some((0, 0).into()),
        );
        output.set_preferred(mode);
        set_surface_scale(surface.wl_surface(), &output, window_scale);
        self.windows.push(RemoteWindow {
            id,
            icon: app_icon::WindowIcon::default(),
            state: window_semantics::State::default(),
            window: Window::new_wayland_window(surface),
            dirty: true,
            announced: false,
            last_info: Value::Null,
            frame_sequence: 0,
            target: None,
            target_size: (0, 0),
            output,
            output_global,
            scale: window_scale,
            mapped: false,
            work_area: Rectangle::from_size((3840, 2160).into()),
        });
        self.focus(Some(id));
        tracing::info!(id, "new application window");
    }
    fn new_popup(&mut self, surface: PopupSurface, positioner: PositionerState) {
        let Ok(root) = find_popup_root_surface(&PopupKind::Xdg(surface.clone())) else {
            return;
        };
        let Some(remote) = self
            .windows
            .iter()
            .find(|w| w.window.wl_surface().as_deref() == Some(&root))
        else {
            return;
        };
        set_surface_scale(surface.wl_surface(), &remote.output, remote.scale);
        let root_id = remote.id;
        let parent = surface
            .get_parent_surface()
            .and_then(|s| {
                self.remote_popups
                    .iter()
                    .find(|p| p.surface.wl_surface() == &s)
                    .map(|p| p.id)
            })
            .unwrap_or(root_id);
        self.constrain_popup(&surface, positioner);
        let id = self.next_id;
        self.next_id += 1;
        self.remote_popups.push(RemotePopup {
            id,
            surface: surface.clone(),
            root: root_id,
            parent,
            dirty: true,
            last_info: Value::Null,
            sequence: 0,
            target: None,
            target_size: (0, 0),
        });
        if let Err(e) = self.popups.track_popup(PopupKind::Xdg(surface)) {
            tracing::warn!(%e,"popup tracking failed");
        }
    }
    fn popup_destroyed(&mut self, surface: PopupSurface) {
        let Ok(root) = find_popup_root_surface(&PopupKind::Xdg(surface)) else {
            return;
        };
        let mut active = false;
        for remote in &mut self.windows {
            if remote.window.wl_surface().as_deref() == Some(&root) {
                active = self.focused_window == Some(remote.id);
            }
        }
        self.popups.cleanup();
        if active {
            // The popup grab forwards this only once the last grabbed popup has ended.
            self.seat.get_keyboard().unwrap().set_focus(
                self,
                Some(root.into()),
                SERIAL_COUNTER.next_serial(),
            );
        }
    }

    fn parent_changed(&mut self, surface: ToplevelSurface) {
        if surface.parent().is_some_and(|parent| {
            !self.windows.iter().any(|w| {
                w.mapped && w.window.alive() && w.window.wl_surface().as_deref() == Some(&parent)
            })
        }) {
            with_states(surface.wl_surface(), |s| {
                s.data_map
                    .get::<XdgToplevelSurfaceData>()
                    .unwrap()
                    .lock()
                    .unwrap()
                    .parent = None
            });
        }
        if let Some(focused) = self.focused_window {
            let target = self.modal_target(focused);
            if target != focused {
                self.focus(Some(target));
            }
        }
    }

    fn move_request(&mut self, surface: ToplevelSurface, seat: wl_seat::WlSeat, serial: Serial) {
        if self.has_pointer_grab(&surface, &seat, serial) {
            self.window_action(&surface, "move", None);
        }
    }
    fn resize_request(
        &mut self,
        surface: ToplevelSurface,
        seat: wl_seat::WlSeat,
        serial: Serial,
        edge: xdg_toplevel::ResizeEdge,
    ) {
        if self.has_pointer_grab(&surface, &seat, serial) {
            self.window_action(&surface, "resize", Some(edge as u32));
        }
    }
    fn fullscreen_request(
        &mut self,
        surface: ToplevelSurface,
        _: Option<smithay::reexports::wayland_server::protocol::wl_output::WlOutput>,
    ) {
        App::fullscreen_request(self, &surface, true);
    }
    fn unfullscreen_request(&mut self, surface: ToplevelSurface) {
        App::fullscreen_request(self, &surface, false);
    }
    fn minimize_request(&mut self, surface: ToplevelSurface) {
        self.window_action(&surface, "minimize", None);
    }
    fn maximize_request(&mut self, surface: ToplevelSurface) {
        self.clear_fullscreen_request(&surface);
        surface.with_pending_state(|s| s.states.set(xdg_toplevel::State::Maximized));
        surface.send_pending_configure();
        self.window_action(&surface, "maximize", None);
    }
    fn unmaximize_request(&mut self, surface: ToplevelSurface) {
        self.clear_fullscreen_request(&surface);
        surface.with_pending_state(|s| s.states.unset(xdg_toplevel::State::Maximized));
        surface.send_pending_configure();
        self.window_action(&surface, "restore", None);
    }
    fn grab(&mut self, surface: PopupSurface, seat: wl_seat::WlSeat, serial: Serial) {
        if Seat::<Self>::from_resource(&seat).as_ref() != Some(&self.seat) {
            return;
        }
        let popup = PopupKind::Xdg(surface);
        let Ok(root) = find_popup_root_surface(&popup) else {
            return;
        };
        let pointer = self.seat.get_pointer().unwrap();
        let keyboard = self.seat.get_keyboard().unwrap();
        let recent = self.last_interaction.is_some_and(|(s, id, time)| {
            s == serial
                && time.elapsed() < Duration::from_secs(10)
                && self.focused_window == Some(id)
                && self
                    .window(id)
                    .and_then(|w| w.wl_surface().map(|s| s.into_owned()))
                    .as_ref()
                    == Some(&root)
        });
        if !recent && !pointer.has_grab(serial) && !keyboard.has_grab(serial) {
            return;
        }
        let seat = self.seat.clone();
        if let Ok(grab) = self
            .popups
            .grab_popup::<Self>(root.clone().into(), popup, &seat, serial)
        {
            let root_id = self
                .windows
                .iter()
                .find(|w| w.window.wl_surface().as_deref() == Some(&root))
                .map(|w| w.id);
            if let Some(id) = root_id {
                self.popup_grab = Some((id, grab.clone()));
                self.control(json!({"type":"popup_grab","root":id,"active":true}));
            }
            self.unlock_pointer(true);
            keyboard.set_focus(self, grab.current_grab(), serial);
            keyboard.set_grab(self, PopupKeyboardGrab::new(&grab), serial);
            pointer.set_grab(
                self,
                PopupPointerGrab::new(&grab),
                serial,
                smithay::input::pointer::Focus::Keep,
            );
        }
    }
    fn reposition_request(
        &mut self,
        surface: PopupSurface,
        positioner: PositionerState,
        token: u32,
    ) {
        self.constrain_popup(&surface, positioner);
        surface.send_repositioned(token);
    }
}
impl SeatHandler for App {
    type KeyboardFocus = x11::KeyboardFocus;
    type PointerFocus = WlSurface;
    type TouchFocus = WlSurface;
    fn seat_state(&mut self) -> &mut SeatState<Self> {
        &mut self.seats
    }
    fn cursor_image(&mut self, _: &Seat<Self>, image: smithay::input::pointer::CursorImageStatus) {
        self.cursor.image=image; self.cursor.dirty=true;
    }
    fn focus_changed(&mut self, seat: &Seat<Self>, surface: Option<&Self::KeyboardFocus>) {
        use smithay::wayland::seat::WaylandFocus;
        set_data_device_focus(
            &self.dh,
            seat,
            surface
                .and_then(|s| s.wl_surface())
                .and_then(|s| s.client()),
        );
        self.ime.focus(surface.and_then(|s| s.wl_surface()).map(|s| s.into_owned()));
        self.publish_ime();
    }
}
impl smithay::wayland::tablet_manager::TabletSeatHandler for App {}
smithay::delegate_cursor_shape!(App);

impl SelectionHandler for App {
    type SelectionUserData = Clipboard;
    fn new_selection(
        &mut self,
        ty: SelectionTarget,
        source: Option<SelectionSource>,
        _: Seat<Self>,
    ) {
        if ty != SelectionTarget::Clipboard {
            return;
        }
        self.clipboard_generation += 1;
        let generation = self.clipboard_generation;
        self.clipboard.clear();
        self.control(json!({"type":"clipboard", "items":{}}));
        self.x11.clipboard_generation = None;
        let Some(source) = source else {
            if let Some(wm) = self.x11.wm.as_mut() {
                let _ = wm.new_selection(SelectionTarget::Clipboard, None);
            }
            return;
        };
        let types = source.mime_types();
        self.x11.clipboard_generation = None;
        if let Some(wm) = self.x11.wm.as_mut() {
            let _ = wm.new_selection(SelectionTarget::Clipboard, Some(types.clone()));
        }
        for mime in [
            "text/plain;charset=utf-8",
            "text/plain",
            "image/png",
            "text/uri-list",
        ] {
            if !types.iter().any(|s| s == mime) {
                continue;
            }
            self.clipboard_reads.push((generation, mime.to_string()));
        }
    }
    fn send_selection(
        &mut self,
        _: SelectionTarget,
        mime: String,
        fd: OwnedFd,
        _: Seat<Self>,
        data: &Clipboard,
    ) {
        if let Some(bytes) = data.get(&mime).cloned() {
            std::thread::spawn(move || {
                let _ = fs::File::from(fd).write_all(&bytes);
            });
        }
    }
}
impl DataDeviceHandler for App {
    fn data_device_state(&self) -> &DataDeviceState {
        &self.data_device
    }
}
impl DataControlHandler for App {
    fn data_control_state(&self) -> &DataControlState {
        &self.data_control
    }
}
impl ClientDndGrabHandler for App {}
impl ServerDndGrabHandler for App {
    fn accept(&mut self, mime: Option<String>, _: Seat<Self>) {
        self.drop_accepted = mime.as_deref() == Some("text/uri-list");
    }
    fn send(&mut self, mime: String, fd: OwnedFd, _: Seat<Self>) {
        if mime == "text/uri-list" {
            if let Some(bytes) = self.drop_data.clone() {
                std::thread::spawn(move || {
                    let _ = fs::File::from(fd).write_all(&bytes);
                });
            }
        }
    }
    fn finished(&mut self, _: Seat<Self>) {
        self.drop_data = None;
    }
    fn cancelled(&mut self, _: Seat<Self>) {
        self.drop_data = None;
    }
}

#[derive(Default)]
struct ClientState {
    compositor: CompositorClientState,
}
impl ClientData for ClientState {
    fn initialized(&self, _: ClientId) {}
    fn disconnected(&self, _: ClientId, _: DisconnectReason) {}
}
smithay::delegate_pointer_constraints!(App);
smithay::delegate_relative_pointer!(App);
smithay::delegate_compositor!(App);
smithay::delegate_shm!(App);
smithay::delegate_dmabuf!(App);
smithay::delegate_output!(App);
smithay::delegate_seat!(App);
smithay::delegate_xdg_shell!(App);
smithay::delegate_xdg_decoration!(App);
smithay::delegate_data_device!(App);
smithay::delegate_data_control!(App);

fn make_renderer(requested: Option<&PathBuf>) -> Result<(GlesRenderer, Option<PathBuf>)> {
    let mut errors = Vec::new();
    for device in EGLDevice::enumerate()? {
        let path = device.render_device_path().ok();
        if requested.is_some() && path.as_ref() != requested {
            continue;
        }
        match (|| -> Result<GlesRenderer> {
            let display = unsafe { EGLDisplay::new(device) }?;
            let context = EGLContext::new(&display)?;
            Ok(unsafe { GlesRenderer::new(context) }?)
        })() {
            Ok(renderer) => {
                tracing::info!(?path, "headless EGL renderer initialized");
                return Ok((renderer, path));
            }
            Err(e) => errors.push(format!("{path:?}: {e:#}")),
        }
    }
    bail!("no usable headless EGL device: {}", errors.join("; "))
}

fn token(path: &PathBuf) -> Result<String> {
    if !path.exists() {
        let mut random = [0; 32];
        fs::File::open("/dev/urandom")?.read_exact(&mut random)?;
        let token: String = random.iter().map(|b| format!("{b:02x}")).collect();
        OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(path)?
            .write_all(token.as_bytes())?;
    }
    let token = fs::read_to_string(path)?.trim().to_string();
    if token.len() < 32 {
        bail!("token must contain at least 32 characters");
    }
    Ok(token)
}

fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| "waybridge=info".into()),
        )
        .init();
    let args = Args::parse();
    metrics::init(args.stats);
    let runtime = std::env::var_os("XDG_RUNTIME_DIR")
        .context("XDG_RUNTIME_DIR must name a private runtime directory")?;
    let token_path = args
        .token_file
        .clone()
        .unwrap_or_else(|| PathBuf::from(&runtime).join("waybridge.token"));
    let auth = token(&token_path)?;
    let (renderer, render_node) = make_renderer(args.render_node.as_ref())?;
    let mut display = Display::<App>::new()?;
    let dh = display.handle();
    let socket = ListeningSocket::bind(&args.socket)
        .context("binding private Wayland socket (already running?)")?;
    let mut xloop = smithay::reexports::calloop::EventLoop::<App>::try_new()?;
    let x11 = x11::start(&dh, xloop.handle(), args.xwayland)?;
    let (events, tx) = network::listen(&args.listen, auth)?;
    let compositor = CompositorState::new_v6::<App>(&dh);
    let shell = XdgShellState::new_with_capabilities::<App>(
        &dh,
        [
            xdg_toplevel::WmCapabilities::Maximize,
            xdg_toplevel::WmCapabilities::Minimize,
        ],
    );
    let decorations = XdgDecorationState::new::<App>(&dh);
    let shm = ShmState::new::<App>(&dh, vec![]);
    let mut seats = SeatState::new();
    let mut seat = seats.new_wl_seat(&dh, "waybridge");
    seat.add_keyboard(
        smithay::input::keyboard::XkbConfig {
            layout: &args.layout,
            ..Default::default()
        },
        300,
        30,
    )?;
    seat.add_pointer();
    let mut dmabuf = DmabufState::new();
    if let Some(node) = render_node {
        let feedback =
            DmabufFeedbackBuilder::new(fs::metadata(node)?.rdev(), renderer.dmabuf_formats())
                .build()?;
        dmabuf.create_global_with_default_feedback::<App>(&dh, &feedback);
    } else {
        dmabuf.create_global::<App>(&dh, renderer.dmabuf_formats());
    }
    let output = Output::new(
        "Waybridge".into(),
        PhysicalProperties {
            size: (0, 0).into(),
            subpixel: Subpixel::Unknown,
            make: "Waybridge".into(),
            model: "Remote applications".into(),
        },
    );
    output.create_global::<App>(&dh);
    let mode = Mode {
        size: (3840, 2160).into(),
        refresh: args.fps as i32 * 1000,
    };
    output.change_current_state(
        Some(mode),
        Some(Transform::Normal),
        None,
        Some((0, 0).into()),
    );
    output.set_preferred(mode);
    let transfers = transfer::Transfers::new(PathBuf::from(&runtime).join("waybridge-inbox"))?;
    let fractional_scale = FractionalScaleManagerState::new::<App>(&dh);
    let viewporter = ViewporterState::new::<App>(&dh);
    let outputs = OutputManagerState::new_with_xdg_output::<App>(&dh);
    let mut app = App {
        cursor: cursor::Cursor::default(),
        _cursor_shapes: smithay::wayland::cursor_shape::CursorShapeManagerState::new::<App>(&dh),
        icons: app_icon::Icons::new(tx.clone()),
        _dialogs: smithay::wayland::shell::xdg::dialog::XdgDialogState::new::<App>(&dh),
        relative: relative_input::RelativeInput::default(),
        _relative_manager: RelativePointerManagerState::new::<App>(&dh),
        _constraints: PointerConstraintsState::new::<App>(&dh),
        ime: ime::Ime::new(&dh),
        data_device: DataDeviceState::new::<App>(&dh),
        data_control: DataControlState::new::<App, _>(&dh, None, |_| true),
        dh,
        compositor,
        x11,
        shell,
        _decorations: decorations,
        _fractional_scale: fractional_scale,
        _viewporter: viewporter,
        _outputs: outputs,
        shm,
        seats,
        seat,
        dmabuf,
        renderer,
        output,
        popups: PopupManager::default(),
        remote_popups: vec![],
        popup_grab: None,
        windows: vec![],
        next_id: 1,
        peer: None,
        events: tx,
        clipboard_generation: 0,
        clipboard: BTreeMap::new(),
        clipboard_reads: vec![],
        transfers,
        drop_data: None,
        drop_deadline: None,
        drop_accepted: false,
        pressed_keys: HashSet::new(),
        pressed_buttons: HashSet::new(),
        pointer_window: None,
        start: Instant::now(),
        default_scale: 1.0,
        focused_window: None,
        last_interaction: None,
        encoder: video::Encoder::new(args.codec != "png", args.fps, args.bitrate),
    };
    let mut children: Vec<Child> = Vec::new();
    if args.xwayland {
        let deadline = Instant::now() + Duration::from_secs(15);
        while app.x11.display.is_none() && !app.x11.failed && Instant::now() < deadline {
            display.dispatch_clients(&mut app)?;
            display.flush_clients()?;
            xloop.dispatch(Some(Duration::from_millis(1)), &mut app)?;
        }
        if app.x11.display.is_none() {
            bail!("private Xwayland startup failed; see Xwayland logs");
        }
    }
    if let Some(program) = args.command.first() {
        let mut command = Command::new(program);
        command
            .args(&args.command[1..])
            .env("WAYLAND_DISPLAY", &args.socket);
        if let Some(number) = app.x11.display {
            command.env("DISPLAY", format!(":{number}"));
        } else {
            command.env_remove("DISPLAY");
        }
        children.push(
            command
                .spawn()
                .with_context(|| format!("launching {program}"))?,
        );
    }
    tracing::info!(socket = %args.socket, token_file = %token_path.display(), "ready; launch apps with WAYLAND_DISPLAY set to this socket");
    let interval = Duration::from_secs_f64(1.0 / args.fps as f64);
    let mut next_frame = Instant::now();
    while !x11::stopping() {
        xloop.dispatch(Some(Duration::ZERO), &mut app)?;
        while let Some(stream) = socket.accept()? {
            app.dh
                .insert_client(stream, Arc::new(ClientState::default()))?;
        }
        display.dispatch_clients(&mut app)?;
        app.sync_pointer_lock();
        app.finish_file_drop();
        app.read_pending_clipboard();
        // Bound input work per cycle so a busy peer cannot starve Wayland or rendering.
        for _ in 0..256 {
            let Ok(event) = events.try_recv() else { break };
            match event {
                Event::Connected(peer) => {
                    app.x11.grab = None;
                    app.release_input();
                    app.peer = Some(peer);
                    app.cursor.dirty = true;
                    app.configure_capabilities();
                    app.ime.republish();
                    app.publish_ime();
                    for p in &mut app.remote_popups {
                        p.last_info = Value::Null;
                        p.dirty = true;
                    }
                    if let Some((root, grab)) = &app.popup_grab {
                        if !grab.has_ended() {
                            app.control(json!({"type":"popup_grab","root":root,"active":true}));
                        }
                    }
                    for w in &mut app.windows {
                        w.state.visible = true;
                        w.state.minimized = false;
                        w.window.toplevel().unwrap().with_pending_state(|s| {
                            s.states.unset(xdg_toplevel::State::Suspended);
                        });
                        w.window.toplevel().unwrap().send_pending_configure();
                        w.announced = false;
                        w.icon.sent = false;
                        w.dirty = true;
                    }
                    let items: BTreeMap<_, _> = app
                        .clipboard
                        .iter()
                        .map(|(mime, data)| (mime.clone(), B64.encode(data)))
                        .collect();
                    if !items.is_empty() {
                        app.control(json!({"type":"clipboard", "items":items}));
                    }
                }
                Event::Message(value) => app.message(value),
                Event::Icon {
                    id,
                    revision,
                    app_id,
                    png,
                } => {
                    if let Some(window) = app.windows.iter_mut().find(|w| w.id == id) {
                        window.icon.complete(revision, &app_id, png);
                    }
                }
                Event::Disconnected => {
                    if let Some(root) = app.x11.grab {
                        app.x11_cancel_popup(root);
                        app.x11.grab = None;
                    }
                    app.relative_available(false);
                    app.release_input();
                    app.encoder.reset();
                    app.focus(None);
                    app.transfers.cancel();
                    app.peer = None;
                    app.configure_capabilities();
                }
                Event::Clipboard {
                    generation,
                    mime,
                    bytes,
                } if generation == app.clipboard_generation => {
                    if mime == "text/uri-list" {
                        if app.x11.clipboard_generation == Some(generation) {
                            app.clipboard.insert(mime.clone(), bytes.clone());
                            set_data_device_selection(
                                &app.dh,
                                &app.seat,
                                app.clipboard.keys().cloned().collect(),
                                Arc::new(app.clipboard.clone()),
                            );
                        }
                        if let Some(peer) = &app.peer {
                            transfer::export(peer.clone(), generation, bytes);
                        }
                        continue;
                    }
                    app.clipboard.insert(mime, bytes);
                    if app.x11.clipboard_generation == Some(generation) {
                        set_data_device_selection(
                            &app.dh,
                            &app.seat,
                            app.clipboard.keys().cloned().collect(),
                            Arc::new(app.clipboard.clone()),
                        );
                    }
                    let items: BTreeMap<_, _> = app
                        .clipboard
                        .iter()
                        .map(|(mime, data)| (mime.clone(), B64.encode(data)))
                        .collect();
                    app.control(json!({"type":"clipboard", "items":items}));
                }
                Event::Clipboard { .. } => {}
            }
        }
        if app
            .ime
            .focus(app.seat.get_keyboard().unwrap().current_focus())
        {
            app.publish_ime();
        }
        if Instant::now() >= next_frame {
            app.render()?;
            next_frame += interval;
            if next_frame < Instant::now() {
                next_frame = Instant::now() + interval;
            }
        }
        display.flush_clients()?;
        // Reap launched apps without terminating a session when one app exits.
        children.retain_mut(|child| child.try_wait().ok().flatten().is_none());
        std::thread::sleep(Duration::from_millis(2));
    }
    if let Some(token) = app.x11.token.take() {
        xloop.handle().remove(token);
    }
    drop(xloop);
    Ok(())
}

fn dpi_scale(value: &Value) -> f64 {
    value["dpi"]
        .as_f64()
        .filter(|dpi| dpi.is_finite())
        .unwrap_or(96.0)
        .clamp(96.0, 384.0)
        / 96.0
}

fn set_surface_scale(surface: &WlSurface, output: &Output, scale: f64) {
    with_surface_tree_downward(
        surface,
        (),
        |_, _, _| TraversalAction::DoChildren(()),
        |surface, states, _| {
            output.enter(surface);
            send_surface_state(surface, states, scale.ceil() as i32, Transform::Normal);
            with_fractional_scale(states, |state| state.set_preferred_scale(scale));
        },
        |_, _, _| true,
    );
}

impl FractionalScaleHandler for App {
    fn new_fractional_scale(&mut self, surface: WlSurface) {
        let root = self.root_for(&surface);
        let scale = self
            .windows
            .iter()
            .find(|w| w.window.wl_surface().as_deref() == Some(&root))
            .map_or(self.default_scale, |w| w.scale);
        with_states(&surface, |s| {
            with_fractional_scale(s, |f| f.set_preferred_scale(scale))
        });
    }
}
smithay::delegate_fractional_scale!(App);
smithay::delegate_viewporter!(App);

fn monitor_mode(value: &Value, refresh: i32) -> Option<Mode> {
    let width = value["work_width"].as_i64()?.clamp(64, 16384) as i32;
    let height = value["work_height"].as_i64()?.clamp(64, 16384) as i32;
    Some(Mode {
        size: (width, height).into(),
        refresh,
    })
}

impl App {
    fn constrain_popup(&self, popup: &PopupSurface, positioner: PositionerState) {
        let Ok(root) = find_popup_root_surface(&PopupKind::Xdg(popup.clone())) else {
            return;
        };
        let Some(remote) = self
            .windows
            .iter()
            .find(|w| w.window.wl_surface().as_deref() == Some(&root))
        else {
            return;
        };
        let mut area = remote.work_area;
        area.loc -= smithay::desktop::get_popup_toplevel_coords(&PopupKind::Xdg(popup.clone()));
        let geometry = positioner.get_unconstrained_geometry(area);
        popup.with_pending_state(|s| {
            s.geometry = geometry;
            s.positioner = positioner;
        });
    }
}

impl App {
    fn update_viewport(&mut self, id: u32, value: &Value) {
        if let Some(remote) = self.windows.iter_mut().find(|w| w.id == id) {
            remote.output.change_current_state(
                monitor_mode(value, remote.output.current_mode().unwrap().refresh),
                None,
                None,
                None,
            );
            if let (Some(x), Some(y), Some(w), Some(h)) = (
                value["work_x"].as_i64(),
                value["work_y"].as_i64(),
                value["work_width"].as_i64(),
                value["work_height"].as_i64(),
            ) {
                let scale = dpi_scale(value);
                remote.work_area = Rectangle::new(
                    (x.clamp(-32768, 32768) as i32, y.clamp(-32768, 32768) as i32).into(),
                    (
                        (w.clamp(64, 16384) as f64 / scale).round() as i32,
                        (h.clamp(64, 16384) as f64 / scale).round() as i32,
                    )
                        .into(),
                );
            }
        }
        for popup in self.remote_popups.iter().filter(|p| p.root == id) {
            let state = with_states(popup.surface.wl_surface(), |s| {
                s.data_map
                    .get::<smithay::wayland::shell::xdg::XdgPopupSurfaceData>()
                    .unwrap()
                    .lock()
                    .unwrap()
                    .current
                    .positioner
            });
            if state.reactive {
                self.constrain_popup(&popup.surface, state);
                let _ = popup.surface.send_pending_configure();
            }
        }
    }
}
