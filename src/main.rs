//! Headless compositor. It never connects to a host desktop or injects host input.
mod app_icon;
mod audio;
mod color;
mod color_management;
mod cursor;
mod desktop_notifications;
mod drag;
mod file_tree;
mod ime;
mod launch;
mod metrics;
mod network;
mod protocol;
mod readonly_export;
mod relative_input;
mod session_guard;
mod session_services;
mod shutdown;
mod stream_config;
mod toplevel_drag;
mod transfer;
mod transfer_worker;
mod udp;
mod video;
mod window_semantics;
mod x11;

use anyhow::{bail, Context, Result};
use base64::{engine::general_purpose::STANDARD as B64, Engine};
use clap::{parser::ValueSource, CommandFactory, FromArgMatches, Parser};
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
            protocol::{
                wl_buffer, wl_data_device_manager::DndAction, wl_data_source::WlDataSource,
                wl_seat, wl_surface::WlSurface,
            },
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
                set_data_device_selection, start_dnd, with_source_metadata, ClientDndGrabHandler,
                DataDeviceHandler, DataDeviceState, ServerDndGrabHandler, SourceMetadata,
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
        io::{AsFd, OwnedFd},
        net::UnixStream,
    },
    path::PathBuf,
    process::{Child, Command},
    rc::Rc,
    sync::{
        atomic::{AtomicBool, Ordering},
        mpsc::SyncSender,
        Arc,
    },
    time::{Duration, Instant},
};

#[derive(Parser, Debug)]
#[command(version, about)]
struct Args {
    /// Replace the standard XDG user configuration file.
    #[arg(long)]
    config: Option<PathBuf>,
    /// Validate configuration without starting graphics, services, or listeners.
    #[arg(long)]
    check_config: bool,
    /// Print effective configuration and per-field sources as JSON, then exit.
    #[arg(long)]
    print_config: bool,
    /// Inspect real render/codec precision without opening a viewer listener or changing a display.
    #[arg(long)]
    check_hdr: bool,
    /// Experimental explicitly tagged Windows-scRGB surfaces (not general ICC HDR).
    #[arg(long,default_value="off",value_parser=["off","experimental-scrgb"])]
    hdr: String,
    /// Require private session audio (otherwise enabled automatically when available).
    #[arg(long)]
    audio: bool,
    /// Disable private session audio.
    #[arg(long, conflicts_with = "audio")]
    no_audio: bool,
    /// Use an SSH tunnel to this loopback listener for the initial prototype.
    #[arg(long, default_value = "127.0.0.1:49090")]
    listen: String,
    #[arg(long, default_value = "waybridge-0")]
    socket: String,
    /// Optional encrypted UDP media endpoint; SSH forwards only TCP.
    #[arg(long)]
    udp_listen: Option<String>,
    /// UDP pacing ceiling, including parity shards (not adaptive congestion control).
    #[arg(long, default_value_t=200, value_parser=clap::value_parser!(u32).range(1..=1000))]
    udp_mbps: u32,
    /// Require Xwayland (otherwise enabled automatically when installed).
    #[arg(long)]
    xwayland: bool,
    /// Disable X11 application compatibility.
    #[arg(long, conflicts_with = "xwayland")]
    no_xwayland: bool,
    /// Launch an application inside the running --socket session, including its audio and D-Bus.
    #[arg(long)]
    launch: bool,
    /// Run the explicitly selected freedesktop notification provider (user service).
    #[arg(long, conflicts_with = "launch")]
    notification_provider: bool,
    /// Absolute working directory for an application (defaults to HOME).
    #[arg(long)]
    cwd: Option<PathBuf>,
    /// Check whether a new session is allowed, without starting graphics or services.
    #[arg(long, conflicts_with = "launch")]
    check_session: bool,
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
    /// Media codec. Auto remains H.264; HEVC/AV1 require explicit selection and viewer support.
    #[arg(long,default_value="auto",value_parser=["auto","png","h264-nvenc","hevc-nvenc","av1-nvenc"])]
    codec: String,
    /// Color input path; CUDA GL interop is optional and requires the same NVIDIA GPU.
    #[arg(long,default_value="cpu",value_parser=["cpu","cuda"])]
    gpu_input: String,
    /// Per-stream video bitrate in kilobits/s.
    #[arg(long,default_value_t=30000,value_parser=clap::value_parser!(u32).range(1000..=200000))]
    bitrate: u32,
    /// Log same-clock pipeline stage average/max durations every two seconds.
    #[arg(long)]
    stats: bool,
    /// Optional program to launch in the private Wayland session (after --).
    #[arg(last = true)]
    command: Vec<String>,
}

impl Args {
    fn configured() -> Result<(Self, waybridge_config::Loaded)> {
        let matches = Self::command().get_matches();
        let mut args = Self::from_arg_matches(&matches)?;
        let explicit_socket = matches.value_source("socket") == Some(ValueSource::CommandLine);
        let mut loaded = if args.notification_provider
            || (args.launch && explicit_socket && !args.check_config && !args.print_config)
        {
            // Attaching to a named existing session does not create or change
            // its stream configuration. A newly broken file cannot block it.
            waybridge_config::Loaded::load_path(None, false)?
        } else {
            waybridge_config::Loaded::load(args.config.as_deref())?
        };
        // Clap defaults preserve --help compatibility, but only explicit command
        // line values may replace the file's values.
        macro_rules! explicit {
            ($field:ident) => {
                (matches.value_source(stringify!($field)) == Some(ValueSource::CommandLine))
                    .then(|| args.$field.clone())
            };
        }
        loaded.apply(waybridge_config::Overrides {
            listen: explicit!(listen),
            socket: explicit!(socket),
            fps: explicit!(fps),
            bitrate: explicit!(bitrate),
            codec: explicit!(codec),
            gpu_input: explicit!(gpu_input),
            hdr: explicit!(hdr),
            layout: explicit!(layout),
            ..Default::default()
        })?;
        let session = &loaded.config.session;
        args.listen = session.listen.clone();
        args.socket = session.socket.clone();
        args.fps = session.fps;
        args.bitrate = session.bitrate;
        args.codec = session.codec.clone();
        args.gpu_input = session.gpu_input.clone();
        args.hdr = session.hdr.clone();
        args.layout = session.layout.clone();
        if let Some(cwd) = &args.cwd {
            if !cwd.is_absolute() || !cwd.is_dir() {
                bail!("--cwd must name an existing absolute directory");
            }
        }
        Ok((args, loaded))
    }
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
    gpu_source: Option<waybridge_codec::GpuSource>,
    target: Option<GlesRenderbuffer>,
    target_size: (i32, i32),
    output: Output,
    output_global: Option<GlobalId>,
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
    gpu_source: Option<waybridge_codec::GpuSource>,
    target: Option<GlesRenderbuffer>,
    target_size: (i32, i32),
}

type Clipboard = Arc<BTreeMap<String, Vec<u8>>>;

struct OutboundDrag {
    id: u64,
    window: u32,
    source: WlDataSource,
    cancelled: Arc<AtomicBool>,
    files: Option<drag::Export>,
    external: bool,
    ready: bool,
    dropped: bool,
    deferred_focus: Option<Option<u32>>,
    activity: Instant,
}

struct App {
    color_programs: Option<color::Programs>,
    _color_manager: Option<GlobalId>,
    gpu_device: Option<Rc<waybridge_codec::GpuDevice>>,
    video_codec: Option<waybridge_codec::Codec>,
    video_allowed: bool,
    hdr_requested: bool,
    stream_fps: u32,
    stream_bitrate: u32,
    stream_limits: waybridge_config::Limits,
    read_only: bool,
    audio: Option<audio::Session>,
    cursor: cursor::Cursor,
    _cursor_shapes: smithay::wayland::cursor_shape::CursorShapeManagerState,
    icons: app_icon::Icons,
    relative: relative_input::RelativeInput,
    _relative_manager: RelativePointerManagerState,
    _constraints: PointerConstraintsState,
    ime: ime::Ime,
    drag_sink: Option<drag::Sink>,
    drag_surface: Option<WlSurface>,
    drag_enabled: bool,
    next_drag: u64,
    outbound: Option<OutboundDrag>,
    window_drag: toplevel_drag::State,
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
    retired_outputs: Vec<(Instant, GlobalId)>,
    next_id: u32,
    peer: Option<Arc<Peer>>,
    events: SyncSender<Event>,
    clipboard_generation: u64,
    clipboard: BTreeMap<String, Vec<u8>>,
    clipboard_reads: Vec<(u64, String)>,
    transfers: transfer_worker::Worker,
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

impl Drop for App {
    fn drop(&mut self) {
        self.encoder.shutdown();
        // Unregister every GL resource while the renderer/context still exists.
        let windows = &mut self.windows;
        let popups = &mut self.remote_popups;
        let _ = self.renderer.with_context(|_| {
            windows.clear();
            popups.clear();
        });
        self.windows.clear();
        self.remote_popups.clear();
        self.gpu_device.take();
    }
}

impl App {
    fn release_gpu_sources(&mut self) {
        let windows = &mut self.windows;
        let _ = self.renderer.with_context(|_| {
            for window in windows {
                window.gpu_source.take();
            }
        });
    }
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
        if self.window_drag.defer_focus(id) {
            return;
        }
        // Native OLE capture/focus changes must not cancel the Wayland source.
        if self.external_drag_input() {
            if let Some(drag) = &mut self.outbound {
                drag.deferred_focus = Some(id);
            }
            self.release_keys();
            return;
        }
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
        let id = id
            .map(|id| self.modal_target(id))
            .filter(|id| self.window(*id).is_some());
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
        self.ime.focus(
            keyboard
                .current_focus()
                .and_then(|s| s.wl_surface().map(|s| s.into_owned())),
        );
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
        if remote.window.is_x11() {
            // X11 shares a RandR desktop. Resample this stream without changing
            // the shared output or its DPI when one native window changes size.
            return;
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
        if self.pointer_locked() {
            return;
        }
        if id == 0 && self.window_drag.active() {
            self.pointer_window = None;
            let pointer = self.seat.get_pointer().unwrap();
            pointer.motion(
                self,
                None,
                &MotionEvent {
                    location: (x, y).into(),
                    serial: SERIAL_COUNTER.next_serial(),
                    time: self.time(),
                },
            );
            pointer.frame(self);
            return;
        }
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
        hit = hit.filter(|(surface, _)| !self.window_drag.excludes(surface));
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

    fn external_drag_input(&self) -> bool {
        self.outbound
            .as_ref()
            .is_some_and(|d| d.external && !d.dropped)
    }

    fn end_outbound(&mut self, success: bool) {
        let Some(drag) = self.outbound.take() else {
            return;
        };
        drag.cancelled.store(true, Ordering::Relaxed);
        let success = success && drag.external && drag.dropped;
        if drag.external && !drag.dropped {
            let pointer = self.seat.get_pointer().unwrap();
            pointer.motion(
                self,
                None,
                &MotionEvent {
                    location: pointer.current_location(),
                    serial: SERIAL_COUNTER.next_serial(),
                    time: self.time(),
                },
            );
            if self.pressed_buttons.remove(&0x110) {
                pointer.button(
                    self,
                    &ButtonEvent {
                        button: 0x110,
                        state: ButtonState::Released,
                        serial: SERIAL_COUNTER.next_serial(),
                        time: self.time(),
                    },
                );
            }
            pointer.frame(self);
        }
        if drag.external {
            self.release_keys();
            if let Some(focus) = drag.deferred_focus {
                self.focus(focus);
            }
            if let Some(sink) = &mut self.drag_sink {
                sink.finish(success);
            }
        }
        self.control(json!({"type":"drag_end","drag":drag.id,"success":success}));
        // Dropping Export cancels its bounded worker and closes source files.
    }

    fn drag_message(&mut self, message: &Value) {
        let Some(id) = message["drag"].as_u64() else {
            return;
        };
        let Some(drag) = self.outbound.as_mut().filter(|d| d.id == id) else {
            return;
        };
        drag.activity = Instant::now();
        match message["type"].as_str().unwrap_or("") {
            "drag_claim"
                if !drag.external
                    && drag.files.is_some()
                    && self.pressed_buttons.contains(&0x110) =>
            {
                let Some(surface) = self.drag_surface.clone() else {
                    self.end_outbound(false);
                    return;
                };
                if self.pressed_buttons.len() != 1 {
                    self.end_outbound(false);
                    return;
                }
                drag.external = true;
                self.release_keys();
                let serial = SERIAL_COUNTER.next_serial();
                if let Some(sink) = &mut self.drag_sink {
                    sink.arm(serial.into());
                }
                let pointer = self.seat.get_pointer().unwrap();
                let location = pointer.current_location();
                pointer.motion(
                    self,
                    Some((surface, location)),
                    &MotionEvent {
                        location,
                        serial,
                        time: self.time(),
                    },
                );
                pointer.frame(self);
            }
            "drag_drop" if drag.external && drag.ready && !drag.dropped => {
                drag.dropped = true;
                let deferred_focus = drag.deferred_focus.take();
                self.release_keys();
                if self.pressed_buttons.remove(&0x110) {
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
                if let Some(focus) = deferred_focus {
                    self.focus(focus);
                }
            }
            "drag_claim" if !drag.external => self.end_outbound(false),
            "drag_result" => self.end_outbound(message["success"].as_bool() == Some(true)),
            "drag_read" if drag.external && drag.ready => {
                let valid = (|| {
                    let request = message["request"].as_u64()?;
                    let file = usize::try_from(message["file"].as_u64()?).ok()?;
                    let offset = message["offset"].as_u64()?;
                    let length = usize::try_from(message["length"].as_u64()?).ok()?;
                    if length == 0 || length > drag::MAX_READ {
                        return None;
                    }
                    let files = drag.files.as_ref()?;
                    Some(files.read(drag::ReadRequest {
                        request,
                        file,
                        offset,
                        length,
                    }))
                })()
                .unwrap_or(false);
                if !valid {
                    self.control(json!({"type":"drag_data","drag":id,"request":message["request"],"error":"invalid read or busy transfer"}));
                }
            }
            _ => {}
        }
    }

    fn pump_outbound(&mut self) -> Result<()> {
        if let Some(sink) = &mut self.drag_sink {
            sink.pump()?;
        }
        let source_window_gone = self.outbound.as_ref().is_some_and(|drag| {
            !drag.dropped && !self.windows.iter().any(|w| w.id == drag.window && w.mapped)
        });
        if let Some(drag) = &mut self.outbound {
            if source_window_gone
                || !drag.source.alive()
                || drag.activity.elapsed() > Duration::from_secs(120)
            {
                self.end_outbound(false);
            } else if drag.external
                && !drag.ready
                && self.drag_sink.as_ref().is_some_and(|s| s.accepted())
            {
                drag.ready = true;
                let id = drag.id;
                self.control(json!({"type":"drag_ready","drag":id}));
            }
        }
        Ok(())
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

    fn release_keys(&mut self) {
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
    }
    fn release_input(&mut self) {
        self.release_keys();
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

    fn publish_uploaded_files(&mut self, value: &Value, uris: Vec<u8>) {
        let id = value["id"]
            .as_u64()
            .and_then(|id| u32::try_from(id).ok())
            .unwrap_or(0);
        if value["drop"].as_bool() == Some(true) {
            let (x, y) = self.message_position(id, value);
            self.begin_file_drop(id, x, y, uris);
        } else {
            let contents = BTreeMap::from([("text/uri-list".to_string(), uris)]);
            self.clipboard_generation += 1;
            if let Some(peer) = &self.peer {
                peer.begin_file_export(self.clipboard_generation);
            }
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
        }
    }

    fn message(&mut self, value: Value) {
        if self.read_only
            && matches!(
                value["type"].as_str(),
                Some("pointer" | "relative_pointer" | "button" | "scroll" | "key" | "ime")
            )
        {
            return;
        }
        let id = value["id"]
            .as_u64()
            .and_then(|n| u32::try_from(n).ok())
            .unwrap_or(0);
        match value["type"].as_str().unwrap_or("") {
            "stream_config" => self.stream_config(&value),
            "file_drop_out_enabled" => {
                self.drag_enabled = value["enabled"].as_bool().unwrap_or(false);
                if !self.drag_enabled {
                    self.end_outbound(false);
                }
            }
            "window_state" => self.native_window_state(id, &value),
            "ime" => self.ime_message(&value),
            "drag_claim" | "drag_drop" | "drag_result" | "drag_read" => {
                self.drag_message(&value);
            }
            "frame_ack" => {
                if let Some(peer) = &self.peer {
                    peer.acknowledge(id, value["sequence"].as_u64().unwrap_or(0));
                }
            }
            "ping" => self.control(json!({"type":"pong","nonce":value["nonce"]})),
            "hello" => {
                self.window_drag.available = value["toplevel_drag_v1"].as_bool().unwrap_or(false);
                self.relative_available(value["pointer_lock"].as_bool().unwrap_or(false));
                self.drag_enabled = value["file_drop_out"].as_bool().unwrap_or(false);
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
                self.x11_reflow();
            }
            "focus" => {
                if value["focused"].as_bool().unwrap_or(false) {
                    self.focus(Some(id));
                } else if self.focused_window == Some(id) {
                    self.focus(None);
                }
            }
            "pointer" if !self.external_drag_input() => self.pointer_message(id, &value),
            "relative_pointer" if !self.external_drag_input() => self.relative_motion(id, &value),
            "pointer_lock_ack" => self.pointer_lock_ack(id, &value),
            "toplevel_drag_cancel" => self.cancel_window_drag(),
            "leave"
                if self.pointer_window == Some(id)
                    && !self.pointer_locked()
                    && !self.external_drag_input() =>
            {
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
            "button"
                if (self.input_allowed(id) || self.window_drag.active())
                    && !self.external_drag_input() =>
            {
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
            "key"
                if self.input_allowed(id)
                    && !self.external_drag_input()
                    && self.focused_window == Some(self.root_id(id)) =>
            {
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
                        if self.semantics_enabled() {
                            let fullscreen = self
                                .windows
                                .iter()
                                .find(|w| w.id == id)
                                .is_some_and(|w| w.state.fullscreen);
                            let _ = x.set_fullscreen(fullscreen);
                        }
                        let mut geometry = x.geometry();
                        geometry.size = (width, height).into();
                        let _ = x.configure(self.x11_geometry(x, geometry));
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
            "color_mode" if self.windows.iter().any(|w| w.id == id) => {
                if let (Some(peer), Some(enabled)) = (&self.peer, value["hdr"].as_bool()) {
                    peer.set_hdr(id, enabled);
                }
                if let Some(w) = self.windows.iter_mut().find(|w| w.id == id) {
                    w.dirty = true;
                }
            }
            "codec_fallback" => {
                if let Some(peer) = &self.peer {
                    let codec = if value["codec"].is_null() {
                        None
                    } else {
                        let Some(codec) = waybridge_codec::Codec::ALL
                            .into_iter()
                            .find(|codec| value["codec"].as_str() == Some(codec.capability()))
                        else {
                            return;
                        };
                        Some(codec)
                    };
                    if !peer.fallback(id, codec) {
                        return;
                    }
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
                if let Some(peer) = &self.peer {
                    peer.begin_file_export(self.clipboard_generation);
                }
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
            "file_tree_epoch" if self.peer.as_ref().is_some_and(|p| p.file_tree()) => {
                if let (Some(peer), Some(epoch)) = (&self.peer, value["epoch"].as_u64()) {
                    if epoch > peer.file_epoch() {
                        peer.set_file_epoch(epoch);
                        self.clipboard_generation += 1;
                        peer.begin_file_export(self.clipboard_generation);
                    }
                }
            }
            "tree_upload_begin" | "tree_upload_chunk" | "tree_upload_end"
            | "tree_upload_commit" | "tree_upload_cancel" | "upload_begin" | "upload_chunk"
            | "upload_end" | "upload_commit" => {
                let Some(peer) = self.peer.clone() else {
                    return;
                };
                if matches!(
                    value["type"].as_str(),
                    Some("tree_upload_commit" | "upload_commit")
                ) && value["drop"].as_bool() == Some(true)
                    && !self.windows.iter().any(|w| w.id == id && w.mapped)
                {
                    peer.control(if value["type"] == "tree_upload_commit" {
                        json!({"type":"tree_upload_error","batch":value["batch"],"message":"File drop target is gone"})
                    } else {
                        json!({"type":"error","message":"File drop target is gone"})
                    });
                    return;
                }
                if !self
                    .transfers
                    .submit(peer.clone(), value, self.clipboard_generation)
                {
                    peer.control(json!({"type":"error","message":"File receiver queue is full; reconnect and retry"}));
                    peer.close();
                    self.transfers.cancel();
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
            if let Some(global) = self
                .windows
                .iter()
                .find(|w| w.id == id)
                .and_then(|w| w.output_global.clone())
            {
                self.retire_output(global);
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
                remote.state.input = window_semantics::input_region(&surface, bbox);
                if let Some(top) = remote.window.toplevel() {
                    let (min, max) = window_semantics::limits(top.wl_surface());
                    info["min_size"] = json!(min);
                    info["max_size"] = json!(max);
                    info["modal"] = with_states(top.wl_surface(), |s| {
                        json!(
                            s.data_map
                                .get::<XdgToplevelSurfaceData>()
                                .unwrap()
                                .lock()
                                .unwrap()
                                .modal
                        )
                    });
                } else if let Some(x) = remote.window.x11_surface() {
                    info["min_size"] = json!(x.min_size().map(|s| [s.w, s.h]).unwrap_or([0, 0]));
                    info["max_size"] = json!(x.max_size().map(|s| [s.w, s.h]).unwrap_or([0, 0]));
                    info["modal"] = json!(x.is_popup());
                }
                info["fullscreen"] = json!(remote.state.fullscreen);
                info["fullscreen_epoch"] = json!(remote.state.fullscreen_epoch);
                info["mapping_epoch"] = json!(remote.state.mapping_epoch);
                info["input_region"] = remote.state.input.clone();
            }
            remote
                .icon
                .observe(info["app_id"].as_str().unwrap_or_default());
            info["icon_revision"] = json!(remote.icon.revision);
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
                remote.dirty |= peer.take_gpu_refresh(remote.id);
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
                        self.gpu_device.as_ref().filter(|_| {
                            self.video_codec.is_some_and(|codec| {
                                peer.supports(codec, remote.id)
                                    || (self.hdr_requested
                                        && peer.hdr_allowed(remote.id)
                                        && codec
                                            .hdr_variant()
                                            .is_some_and(|c| peer.supports(c, remote.id)))
                            }) && peer.gpu_allowed(remote.id)
                        }),
                        &mut remote.gpu_source,
                        self.color_programs.as_ref(),
                        self.video_codec
                            .filter(|_| self.hdr_requested)
                            .and_then(|c| c.hdr_variant()),
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
                        None,
                        &mut popup.gpu_source,
                        self.color_programs.as_ref(),
                        None,
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
    gpu_device: Option<&Rc<waybridge_codec::GpuDevice>>,
    gpu_source: &mut Option<waybridge_codec::GpuSource>,
    color_programs: Option<&color::Programs>,
    hdr_codec: Option<waybridge_codec::Codec>,
) -> Result<video::Frame> {
    let color_ids = color_programs
        .map(|_| color::scrgb_ids(surface))
        .unwrap_or_default();
    let linear_scene = !color_ids.is_empty();
    let capture_format = if linear_scene {
        Fourcc::Abgr16161616f
    } else {
        Fourcc::Abgr8888
    };
    // FP16 goes through a color-aware path; the original CUDA RGBA8 kernel must
    // never silently reinterpret half-float samples as eight-bit data.
    let gpu_device = gpu_device.filter(|_| {
        !linear_scene || (peer.hdr_allowed(id) && hdr_codec.is_some_and(|c| peer.supports(c, id)))
    });
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
    if gpu_device.is_none() && gpu_source.is_some() {
        renderer.with_context(|_| drop(gpu_source.take()))?;
    }
    if *target_size != (pixels.w, pixels.h)
        || target.as_ref().and_then(|target| target.format()) != Some(capture_format)
    {
        renderer.with_context(|_| drop(gpu_source.take()))?;
        *target = Some(renderer.create_buffer(capture_format, (pixels.w, pixels.h).into())?);
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
    let mut source_object = (0i32, 0i32);
    let mut fp16 = if linear_scene {
        vec![0u8; pixels.w as usize * pixels.h as usize * 8]
    } else {
        vec![]
    };
    {
        let mut frame = renderer.render(&mut bound, pixels, Transform::Flipped180)?;
        frame.clear(Color32F::new(0., 0., 0., 0.), &[damage])?;
        if linear_scene {
            let colored = color::color_elements(elements, &color_ids, color_programs.unwrap());
            draw_render_elements(&mut frame, scale, &colored, &[damage])?;
            if gpu_device.is_none() {
                frame.with_context(|gl| unsafe {
                    use smithay::backend::renderer::gles::ffi;
                    let mut pack = 0;
                    gl.GetIntegerv(ffi::PIXEL_PACK_BUFFER_BINDING, &mut pack);
                    gl.BindBuffer(ffi::PIXEL_PACK_BUFFER, 0);
                    gl.ReadPixels(
                        0,
                        0,
                        pixels.w,
                        pixels.h,
                        ffi::RGBA,
                        ffi::HALF_FLOAT,
                        fp16.as_mut_ptr().cast(),
                    );
                    gl.BindBuffer(ffi::PIXEL_PACK_BUFFER, pack as u32);
                })?;
            }
        } else {
            draw_render_elements(&mut frame, scale, &elements, &[damage])?;
        }
        if gpu_device.is_some() {
            frame.with_context(|gl| unsafe {
                use smithay::backend::renderer::gles::ffi;
                gl.GetFramebufferAttachmentParameteriv(
                    ffi::FRAMEBUFFER,
                    ffi::COLOR_ATTACHMENT0,
                    ffi::FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,
                    &mut source_object.0,
                );
                gl.GetFramebufferAttachmentParameteriv(
                    ffi::FRAMEBUFFER,
                    ffi::COLOR_ATTACHMENT0,
                    ffi::FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,
                    &mut source_object.1,
                );
            })?;
        }
        frame.finish()?.wait()?;
    }
    metrics::record("composite", started.elapsed());
    if let Some(device) = gpu_device.filter(|_| {
        pixels.w >= 48
            && pixels.h >= 48
            && pixels.w <= 4096
            && pixels.h <= 2304
            && peer.gpu_allowed(id)
    }) {
        let started = Instant::now();
        let result = renderer.with_context(|_| -> std::result::Result<_, String> {
            if source_object.0 != smithay::backend::renderer::gles::ffi::RENDERBUFFER as i32
                || source_object.1 <= 0
            {
                return Err("expected a public GL renderbuffer framebuffer attachment".into());
            }
            if gpu_source.is_none() {
                *gpu_source = Some(waybridge_codec::GpuSource::new_mode(
                    device.clone(),
                    source_object.1 as u32,
                    pixels.w as u32,
                    pixels.h as u32,
                    linear_scene,
                )?);
                tracing::info!(
                    id,
                    width = pixels.w,
                    height = pixels.h,
                    "CUDA GL color input registered; independent alpha may still read back"
                );
            }
            gpu_source.as_mut().unwrap().capture(true)
        })?;
        match result {
            Ok(gpu) => {
                metrics::record("gpu_capture", started.elapsed());
                return Ok(video::Frame {
                    id,
                    width: pixels.w as u32,
                    height: pixels.h as u32,
                    sequence,
                    geometry: [size.w, size.h, bbox.loc.x, bbox.loc.y],
                    rgba: vec![],
                    gpu: Some(gpu),
                    straight: true,
                    peer,
                    video_eligible: false,
                    enqueued: Instant::now(),
                });
            }
            Err(error) => {
                peer.disable_gpu(id);
                renderer.with_context(|_| drop(gpu_source.take()))?;
                tracing::warn!(id,%error,"CUDA GL capture failed; using CPU color path for this viewer/window");
            }
        }
    }
    if linear_scene {
        if gpu_device.is_some() {
            let mut frame = renderer.render(&mut bound, pixels, Transform::Flipped180)?;
            frame.with_context(|gl| unsafe {
                use smithay::backend::renderer::gles::ffi;
                let mut pack = 0;
                gl.GetIntegerv(ffi::PIXEL_PACK_BUFFER_BINDING, &mut pack);
                gl.BindBuffer(ffi::PIXEL_PACK_BUFFER, 0);
                gl.ReadPixels(
                    0,
                    0,
                    pixels.w,
                    pixels.h,
                    ffi::RGBA,
                    ffi::HALF_FLOAT,
                    fp16.as_mut_ptr().cast(),
                );
                gl.BindBuffer(ffi::PIXEL_PACK_BUFFER, pack as u32);
            })?;
            frame.finish()?.wait()?;
        }
        let started = Instant::now();
        let rgba = color::tonemap_fp16(&fp16, pixels.w as usize, pixels.h as usize, true);
        metrics::record("hdr_tonemap_cpu", started.elapsed());
        return Ok(video::Frame {
            id,
            width: pixels.w as u32,
            height: pixels.h as u32,
            sequence,
            geometry: [size.w, size.h, bbox.loc.x, bbox.loc.y],
            rgba,
            gpu: None,
            straight: true,
            peer,
            video_eligible: false,
            enqueued: Instant::now(),
        });
    }
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
        gpu: None,
        straight: false,
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
    fn request_mode(&mut self, surface: ToplevelSurface, mode: DecorationMode) {
        // Chrome explicitly asks to keep its own tab-strip/window controls.
        // Server decoration remains the default for applications requesting it.
        surface.with_pending_state(|s| s.decoration_mode = Some(mode));
        surface.send_pending_configure();
    }
    fn unset_mode(&mut self, surface: ToplevelSurface) {
        self.request_mode(surface, DecorationMode::ServerSide);
    }
}

impl CompositorHandler for App {
    fn new_surface(&mut self, surface: &WlSurface) {
        if surface
            .client()
            .is_some_and(|c| c.get_data::<ClientState>().is_some_and(|s| s.drag_sink))
        {
            self.drag_surface = Some(surface.clone());
        }
    }
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
                    // CUDA registration must end before the GL attachment is destroyed.
                    // Retain both resources if the context cannot be made current.
                    if remote.gpu_source.is_none()
                        || self
                            .renderer
                            .with_context(|_| drop(remote.gpu_source.take()))
                            .is_ok()
                    {
                        remote.target = None;
                        remote.target_size = (0, 0);
                    } else {
                        tracing::warn!(id = remote.id, "deferring GPU target release after unmap");
                    }
                    unmapped = Some(remote.id);
                }
                if mapped {
                    remote.state.visible = true;
                    if let Some(top) = remote.window.toplevel() {
                        top.with_pending_state(|s| {
                            s.states.unset(xdg_toplevel::State::Suspended);
                        });
                        top.send_pending_configure();
                    }
                }
                if semantics && remote.announced {
                    self.peer.as_ref().unwrap().control(json!({"type":"window_visibility","id":remote.id,"mapped":mapped,"mapping_epoch":remote.state.mapping_epoch,"fullscreen":remote.state.fullscreen,"fullscreen_epoch":remote.state.fullscreen_epoch}));
                }
            }
            remote.mapped = mapped;
            if let Some(top) = remote.window.toplevel() {
                if unmapped != Some(remote.id) && !top.is_initial_configure_sent() {
                    top.send_configure();
                }
            }
            let density = if remote.window.is_x11() {
                self.default_scale
            } else {
                remote.scale
            };
            set_surface_scale(surface, &remote.output, density);
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
impl OutputHandler for App {
    fn can_view_output(client: &Client, output: &Output) -> bool {
        output.name() == "Waybridge"
            || client
                .get_data::<smithay::xwayland::XWaylandClientData>()
                .is_none()
    }
}
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
            gpu_source: None,
            target: None,
            target_size: (0, 0),
            output,
            output_global: Some(output_global),
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
            gpu_source: None,
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
        self.cursor.image = image;
        self.cursor.dirty = true;
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
        self.ime
            .focus(surface.and_then(|s| s.wl_surface()).map(|s| s.into_owned()));
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
        if let Some(peer) = &self.peer {
            peer.begin_file_export(generation);
        }
        self.clipboard.clear();
        self.control(json!({"type":"clipboard", "items":{}, "generation":generation}));
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
impl ClientDndGrabHandler for App {
    fn send_drop_performed(&mut self, source: &WlDataSource, validated: bool) -> bool {
        validated || self.window_drag.completes_move(source)
    }
    fn started(&mut self, source: Option<WlDataSource>, _: Option<WlSurface>, _: Seat<Self>) {
        let internal = source.as_ref().is_some_and(|s| {
            self.window_drag.is_toplevel_source(s)
                || with_source_metadata(s, |m| !m.mime_types.iter().any(|s| s == "text/uri-list"))
                    .unwrap_or(false)
        });
        if internal {
            self.start_window_drag(source);
            return;
        }
        if !self.drag_enabled
            || self.outbound.is_some()
            || self.drag_surface.is_none()
            || self.pressed_buttons.len() != 1
            || !self.pressed_buttons.contains(&0x110)
        {
            return;
        }
        let Some(source) = source.filter(|s| s.version() >= 3) else {
            return;
        };
        let supported = with_source_metadata(&source, |m| {
            m.mime_types.iter().any(|s| s == "text/uri-list")
                && m.dnd_action.contains(DndAction::Copy)
        })
        .unwrap_or(false);
        if !supported {
            return;
        }
        let Some(peer) = self.peer.clone() else {
            return;
        };
        let Ok((read, write)) = UnixStream::pair() else {
            return;
        };
        self.next_drag += 1;
        let id = self.next_drag;
        let window = self.focused_window.unwrap_or(0);
        let cancelled = Arc::new(AtomicBool::new(false));
        if !drag::prepare(id, read, peer, self.events.clone(), cancelled.clone()) {
            return;
        }
        source.send("text/uri-list".into(), write.as_fd());
        self.outbound = Some(OutboundDrag {
            id,
            window,
            source,
            cancelled,
            files: None,
            external: false,
            ready: false,
            dropped: false,
            deferred_focus: None,
            activity: Instant::now(),
        });
    }
    fn dropped(&mut self, _: Option<WlSurface>, validated: bool, _: Seat<Self>) {
        self.window_drag.ended();
        if let Some(drag) = &self.outbound {
            if !drag.dropped && (!drag.external || !validated) {
                self.end_outbound(false);
            }
        }
    }
}
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
    drag_sink: bool,
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
    let (args, configuration) = Args::configured()?;
    if args.notification_provider {
        return desktop_notifications::run_provider();
    }
    if args.print_config {
        println!("{}", serde_json::to_string_pretty(&configuration.report())?);
        return Ok(());
    }
    if args.check_config {
        println!(
            "{}",
            json!({"valid": true, "config_file": configuration.report()["config_file"]})
        );
        return Ok(());
    }
    if args.launch {
        return launch::attach(&args.socket, &args.command, args.cwd.as_deref());
    }
    if args.check_hdr {
        let (mut renderer, node) = make_renderer(args.render_node.as_ref())?;
        let mut report = color::preflight(&mut renderer);
        report["render_node"] = json!(node);
        println!("{}", report);
        return Ok(());
    }
    if args.check_session {
        let runtime = std::env::var_os("XDG_RUNTIME_DIR")
            .context("XDG_RUNTIME_DIR must name a private runtime directory")?;
        let result = session_guard::Guard::acquire(std::path::Path::new(&runtime));
        println!(
            "{}",
            match result {
                Ok(_) => json!({"available": true}),
                Err(error) => json!({"available": false, "error": format!("{error:#}")}),
            }
        );
        return Ok(());
    }
    // One signal flag drives capture workers and both Wayland/X11 session cleanup.
    shutdown::install();
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| "waybridge=info".into()),
        )
        .init();
    metrics::init(args.stats);
    let runtime = std::env::var_os("XDG_RUNTIME_DIR")
        .context("XDG_RUNTIME_DIR must name a private runtime directory")?;
    let _session_guard = session_guard::Guard::acquire(std::path::Path::new(&runtime))?;
    let token_path = args
        .token_file
        .clone()
        .unwrap_or_else(|| PathBuf::from(&runtime).join("waybridge.token"));
    let auth = token(&token_path)?;
    let (mut renderer, render_node) = make_renderer(args.render_node.as_ref())?;
    let video_codec = stream_config::resolve_codec(&args.codec, args.hdr == "experimental-scrgb")?;
    let gpu_device = if (args.gpu_input == "cuda" || args.hdr == "experimental-scrgb")
        && video_codec.is_some()
    {
        match renderer.with_context(|_| waybridge_codec::GpuDevice::new())? {
            Ok(device) => {
                tracing::info!("CUDA GL color input available (opt-in)");
                Some(device)
            }
            Err(error) => {
                tracing::warn!(%error,"CUDA GL input unavailable; retaining CPU input");
                None
            }
        }
    } else {
        None
    };
    let color_programs = if args.hdr == "experimental-scrgb" {
        match color::validate_fp16(&mut renderer).and_then(|()| color::Programs::new(&mut renderer))
        {
            Ok(programs) => Some(programs),
            Err(error) => {
                tracing::warn!(%error,"experimental color management unavailable; retaining SDR");
                None
            }
        }
    } else {
        None
    };
    let mut display = Display::<App>::new()?;
    let dh = display.handle();
    let mut xloop = smithay::reexports::calloop::EventLoop::<App>::try_new()?;
    let xwayland_enabled = args.xwayland || (!args.no_xwayland && launch::available("Xwayland"));
    let x11 = x11::start(&dh, xloop.handle(), xwayland_enabled)?;
    let audio = if !args.no_audio
        && (args.audio || (launch::available("pactl") && launch::available("parec")))
    {
        match audio::Session::new() {
            Ok(audio) => Some(audio),
            Err(error) if !args.audio => {
                tracing::warn!(%error, "automatic session audio unavailable; continuing without audio");
                None
            }
            Err(error) => return Err(error),
        }
    } else {
        None
    };
    let udp = args
        .udp_listen
        .as_deref()
        .map(|address| udp::Hub::new(address, args.udp_mbps))
        .transpose()?;
    let (events, tx) = network::listen(
        &args.listen,
        auth,
        network::Options {
            hdr_enabled: color_programs.is_some()
                && gpu_device.is_some()
                && video_codec.is_some_and(|c| c.hdr_variant().is_some()),
            audio: audio.is_some(),
            udp,
        },
    )?;
    // Publishing the Wayland socket is also the readiness signal used by local
    // launchers. Do it after optional backends and the viewer listener exist.
    let socket = ListeningSocket::bind(&args.socket)
        .context("binding private Wayland socket (already running?)")?;
    let compositor = CompositorState::new_v6::<App>(&dh);
    let shell = XdgShellState::new_with_capabilities::<App>(
        &dh,
        [
            xdg_toplevel::WmCapabilities::Maximize,
            xdg_toplevel::WmCapabilities::Minimize,
        ],
    );
    let decorations = XdgDecorationState::new::<App>(&dh);
    let shm = ShmState::new::<App>(
        &dh,
        if color_programs.is_some() {
            vec![smithay::reexports::wayland_server::protocol::wl_shm::Format::Abgr16161616f]
        } else {
            vec![]
        },
    );
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
    let transfers =
        transfer_worker::Worker::new(PathBuf::from(&runtime).join("waybridge-inbox"), tx.clone())?;
    let fractional_scale = FractionalScaleManagerState::new::<App>(&dh);
    let viewporter = ViewporterState::new::<App>(&dh);
    let outputs = OutputManagerState::new_with_xdg_output::<App>(&dh);
    let mut app = App {
        _color_manager: color_programs
            .as_ref()
            .map(|_| color_management::register(&dh)),
        color_programs,
        gpu_device,
        video_codec,
        video_allowed: video_codec.is_some(),
        hdr_requested: args.hdr == "experimental-scrgb",
        stream_fps: args.fps,
        stream_bitrate: args.bitrate,
        stream_limits: configuration.config.limits.clone(),
        read_only: false,
        audio,
        cursor: cursor::Cursor::default(),
        _cursor_shapes: smithay::wayland::cursor_shape::CursorShapeManagerState::new::<App>(&dh),
        icons: app_icon::Icons::new(tx.clone()),
        _dialogs: smithay::wayland::shell::xdg::dialog::XdgDialogState::new::<App>(&dh),
        relative: relative_input::RelativeInput::default(),
        _relative_manager: RelativePointerManagerState::new::<App>(&dh),
        _constraints: PointerConstraintsState::new::<App>(&dh),
        ime: ime::Ime::new(&dh),
        drag_sink: None,
        drag_surface: None,
        drag_enabled: false,
        next_drag: 0,
        outbound: None,
        window_drag: toplevel_drag::State::default(),
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
        retired_outputs: vec![],
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
        encoder: video::Encoder::new(video_codec, args.fps, args.bitrate),
    };
    toplevel_drag::State::register(&app.dh);
    let (sink_server, sink_client) = UnixStream::pair()?;
    app.dh.insert_client(
        sink_server,
        Arc::new(ClientState {
            drag_sink: true,
            ..ClientState::default()
        }),
    )?;
    app.drag_sink = Some(drag::Sink::new(sink_client)?);
    let mut children: Vec<Child> = Vec::new();
    if xwayland_enabled {
        let deadline = Instant::now() + Duration::from_secs(15);
        while app.x11.display.is_none() && !app.x11.failed && Instant::now() < deadline {
            display.dispatch_clients(&mut app)?;
            display.flush_clients()?;
            xloop.dispatch(Some(Duration::from_millis(1)), &mut app)?;
        }
        if app.x11.display.is_none() {
            if args.xwayland {
                bail!("private Xwayland startup failed; see Xwayland logs");
            }
            tracing::warn!(
                "automatic Xwayland unavailable; continuing with native Wayland applications"
            );
        }
    }
    let mut application_session = launch::Session::new(
        &args.socket,
        app.x11.display,
        app.audio.as_ref(),
        &args.listen,
        &token_path,
    )?;
    let mut notifications = desktop_notifications::Bridge::new(
        application_session.service_runtime(),
        application_session.service_bus_address(),
    )?;
    // Service shutdown must run for application-launch and event-loop errors too.
    // Keep the display alive and dispatchable until owned components have stopped.
    let run_result = (|| -> Result<()> {
        application_session.start_services()?;
        if let Some(program) = args.command.first() {
            let mut command = Command::new(program);
            command.args(&args.command[1..]);
            application_session.configure(&mut command, app.audio.as_ref());
            let cwd = args
                .cwd
                .clone()
                .or_else(|| std::env::var_os("HOME").map(PathBuf::from))
                .context("HOME is required for application launch without --cwd")?;
            if !cwd.is_absolute() || !cwd.is_dir() {
                bail!("application working directory must be an existing absolute directory");
            }
            command.current_dir(cwd);
            children.push(
                command
                    .spawn()
                    .with_context(|| format!("launching {program}"))?,
            );
        }
        tracing::info!(socket = %args.socket, token_file = %token_path.display(), "ready; launch apps with WAYLAND_DISPLAY set to this socket");
        let mut next_frame = Instant::now();
        while !shutdown::requested() {
            notifications.receive(|value| {
                if let Some(peer) = app.peer.as_ref().filter(|p| p.notifications_enabled()) {
                    peer.control(value);
                }
            });
            app.reap_retired_outputs();
            xloop.dispatch(Some(Duration::ZERO), &mut app)?;
            while let Some(stream) = socket.accept()? {
                app.dh
                    .insert_client(stream, Arc::new(ClientState::default()))?;
            }
            display.dispatch_clients(&mut app)?;
            app.poll_window_drag();
            app.sync_pointer_lock();
            app.pump_outbound()?;
            app.finish_file_drop();
            app.read_pending_clipboard();
            // Bound input work per cycle so a busy peer cannot starve Wayland or rendering.
            for _ in 0..256 {
                let Ok(event) = events.try_recv() else { break };
                match event {
                    Event::Connected(peer) => {
                        notifications.connection(peer.notifications_initial());
                        if peer.notifications_enabled() {
                            peer.control(notifications.status());
                        }
                        app.read_only = false;
                        if let Some(audio) = &app.audio {
                            audio.set_peer(Some(&peer));
                        }
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
                            if let Some(top) = w.window.toplevel() {
                                top.with_pending_state(|s| {
                                    s.states.unset(xdg_toplevel::State::Suspended);
                                });
                                top.send_pending_configure();
                            } else if let Some(x) = w.window.x11_surface() {
                                let _ = x.set_suspended(false);
                            }
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
                            app.control(json!({"type":"clipboard", "items":items, "generation":app.clipboard_generation}));
                        }
                    }
                    Event::Message(mut value) => {
                        if value["type"] == "notification_feedback" {
                            if app.read_only && value.get("action").is_some() {
                                if let Some(fields) = value.as_object_mut() {
                                    fields.remove("action");
                                    fields.insert("reason".into(), json!(2));
                                }
                            }
                            notifications.feedback(&value);
                        } else if value["type"] == "notifications_enabled" {
                            notifications.enabled(
                                value["enabled"] == true
                                    && app.peer.as_ref().is_some_and(|p| p.notifications_enabled()),
                            );
                        } else {
                            app.message(value);
                        }
                    }
                    Event::UploadedFiles(completed) => {
                        if !app.transfers.current(completed.epoch)
                            || !app
                                .peer
                                .as_ref()
                                .is_some_and(|peer| Arc::ptr_eq(peer, &completed.peer))
                        {
                            continue;
                        }
                        let value = &completed.message;
                        let tree = value["type"] == "tree_upload_commit";
                        let dropped = value["drop"].as_bool() == Some(true);
                        let valid = if dropped {
                            value["id"].as_u64().is_some_and(|id| {
                                app.windows
                                    .iter()
                                    .any(|w| u64::from(w.id) == id && w.mapped)
                            })
                        } else {
                            completed.clipboard_generation == app.clipboard_generation
                                && (!tree
                                    || value["epoch"]
                                        .as_u64()
                                        .is_none_or(|epoch| epoch == completed.peer.file_epoch()))
                        };
                        if !valid {
                            completed.peer.control(if tree {
                                json!({"type":"tree_upload_error","batch":value["batch"],"message":"Upload target or clipboard was superseded"})
                            } else {
                                json!({"type":"error","message":"Upload target or clipboard was superseded"})
                            });
                            continue;
                        }
                        app.publish_uploaded_files(value, completed.uris);
                        completed.peer.control(if tree {
                            json!({"type":"tree_upload_ready","batch":value["batch"],"stage":"commit"})
                        } else {
                            json!({"type":"upload_ready","files":value["transfers"].as_array().map_or(0, Vec::len)})
                        });
                    }
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
                    Event::DragFiles { id, result } => {
                        if let Some(drag) = app.outbound.as_mut().filter(|d| d.id == id) {
                            match result {
                                Ok(files) => {
                                    let manifest = files.manifest.clone();
                                    let tree = files.tree;
                                    let window = drag.window;
                                    drag.files = Some(files);
                                    app.control(json!({"type":"drag_offer","drag":id,"id":window,"files":manifest,"tree":tree,"action":"copy"}));
                                }
                                Err(error) => {
                                    tracing::debug!(%error, "outbound drag not supported");
                                    app.end_outbound(false);
                                }
                            }
                        }
                    }
                    Event::Disconnected => {
                        notifications.connection(false);
                        app.cancel_window_drag();
                        app.poll_window_drag();
                        app.window_drag.available = false;
                        app.release_gpu_sources();
                        if let Some(audio) = &app.audio {
                            audio.set_peer(None);
                        }
                        if let Some(root) = app.x11.grab {
                            app.x11_cancel_popup(root);
                            app.x11.grab = None;
                        }
                        app.relative_available(false);
                        app.end_outbound(false);
                        app.drag_enabled = false;
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
                        app.control(json!({"type":"clipboard", "items":items, "generation":app.clipboard_generation}));
                    }
                    Event::Clipboard { .. } => {}
                }
            }
            if app.ime.focus(
                app.seat
                    .get_keyboard()
                    .unwrap()
                    .current_focus()
                    .and_then(|s| s.wl_surface().map(|s| s.into_owned())),
            ) {
                app.publish_ime();
            }
            if Instant::now() >= next_frame {
                app.render()?;
                let interval = Duration::from_secs_f64(1.0 / app.stream_fps as f64);
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
        Ok(())
    })();
    let mut xloop_error_reported = false;
    let shutdown_result = application_session.stop_services(|| {
        // A failed Xwayland event source must not prevent native clients from
        // completing their final roundtrips on an otherwise healthy display.
        if let Err(error) = xloop.dispatch(Some(Duration::ZERO), &mut app) {
            if !xloop_error_reported {
                tracing::warn!(%error, "Xwayland event dispatch failed during session shutdown");
                xloop_error_reported = true;
            }
        }
        while let Some(stream) = socket
            .accept()
            .context("accepting shutdown Wayland clients")?
        {
            app.dh
                .insert_client(stream, Arc::new(ClientState::default()))?;
        }
        display.dispatch_clients(&mut app)?;
        display.flush_clients()?;
        Ok(())
    });
    if let Some(token) = app.x11.token.take() {
        xloop.handle().remove(token);
    }
    drop(xloop);
    if let Err(error) = shutdown_result {
        if run_result.is_ok() {
            return Err(error).context("stopping Waybridge session services");
        }
        // Keep the original failure as the returned error; record an independent
        // cleanup failure as well so neither cause is lost.
        tracing::error!(error = %format!("{error:#}"), "Waybridge session shutdown also failed");
    }
    run_result
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
            .map_or(self.default_scale, |w| {
                if w.window.is_x11() {
                    self.default_scale
                } else {
                    w.scale
                }
            });
        with_states(&surface, |s| {
            with_fractional_scale(s, |f| f.set_preferred_scale(scale))
        });
    }
}
smithay::delegate_fractional_scale!(App);
smithay::delegate_viewporter!(App);

fn monitor_mode(value: &Value, refresh: i32) -> Option<Mode> {
    let width = value
        .get("monitor_width")
        .unwrap_or(&value["work_width"])
        .as_i64()?
        .clamp(64, 16384) as i32;
    let height = value
        .get("monitor_height")
        .unwrap_or(&value["work_height"])
        .as_i64()?
        .clamp(64, 16384) as i32;
    Some(Mode {
        size: (width, height).into(),
        refresh,
    })
}

impl App {
    fn retire_output(&mut self, global: GlobalId) {
        // A client may already have queued a bind before global_remove reaches
        // it. Stop advertising now, but retain valid bind state for in-flight
        // requests instead of disconnecting that client with a protocol error.
        self.dh.disable_global::<App>(global.clone());
        self.retired_outputs
            .push((Instant::now() + Duration::from_secs(10), global));
    }

    fn reap_retired_outputs(&mut self) {
        let now = Instant::now();
        self.retired_outputs.retain(|(deadline, global)| {
            if *deadline > now {
                return true;
            }
            self.dh.remove_global::<App>(global.clone());
            false
        });
    }

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
            if !remote.window.is_x11() {
                remote.output.change_current_state(
                    monitor_mode(value, remote.output.current_mode().unwrap().refresh),
                    None,
                    None,
                    None,
                );
            }
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
