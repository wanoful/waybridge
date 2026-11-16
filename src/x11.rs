//! Opt-in rootless Xwayland; each X11 surface uses the existing independent window transport.
use super::*;
use smithay::{
    reexports::calloop::LoopHandle,
    wayland::xwayland_shell::{XWaylandShellHandler, XWaylandShellState},
    xwayland::{
        xwm::{Reorder, ResizeEdge, WmWindowProperty, XwmId},
        X11Surface, X11Wm, XWayland, XWaylandEvent, XwmHandler,
    },
};
use std::{
    process::Stdio,
    sync::atomic::{AtomicUsize, Ordering},
};
static WRITERS: AtomicUsize = AtomicUsize::new(0);
use smithay::input::keyboard::{KeyboardTarget, KeysymHandle, ModifiersState};
use smithay::wayland::seat::WaylandFocus;
#[derive(Clone, Debug, PartialEq)]
pub enum InputFocus {
    Wayland(WlSurface),
    X11(Arc<X11Surface>),
}
impl From<WlSurface> for InputFocus {
    fn from(s: WlSurface) -> Self {
        Self::Wayland(s)
    }
}
impl From<PopupKind> for InputFocus {
    fn from(p: PopupKind) -> Self {
        Self::Wayland(p.wl_surface().clone())
    }
}
impl From<InputFocus> for WlSurface {
    fn from(f: InputFocus) -> Self {
        f.wl_surface()
            .expect("mapped keyboard surface")
            .into_owned()
    }
}
impl IsAlive for InputFocus {
    fn alive(&self) -> bool {
        match self {
            Self::Wayland(s) => s.alive(),
            Self::X11(s) => s.alive(),
        }
    }
}
impl WaylandFocus for InputFocus {
    fn wl_surface(&self) -> Option<std::borrow::Cow<'_, WlSurface>> {
        match self {
            Self::Wayland(s) => Some(std::borrow::Cow::Borrowed(s)),
            Self::X11(s) => s.wl_surface().map(std::borrow::Cow::Owned),
        }
    }
}
impl KeyboardTarget<App> for InputFocus {
    fn enter(&self, seat: &Seat<App>, app: &mut App, keys: Vec<KeysymHandle<'_>>, serial: Serial) {
        match self {
            Self::Wayland(s) => KeyboardTarget::enter(s, seat, app, keys, serial),
            Self::X11(s) => KeyboardTarget::enter(s.as_ref(), seat, app, keys, serial),
        }
    }
    fn leave(&self, seat: &Seat<App>, app: &mut App, serial: Serial) {
        match self {
            Self::Wayland(s) => KeyboardTarget::leave(s, seat, app, serial),
            Self::X11(s) => KeyboardTarget::leave(s.as_ref(), seat, app, serial),
        }
    }
    fn key(
        &self,
        seat: &Seat<App>,
        app: &mut App,
        key: KeysymHandle<'_>,
        state: KeyState,
        serial: Serial,
        time: u32,
    ) {
        match self {
            Self::Wayland(s) => KeyboardTarget::key(s, seat, app, key, state, serial, time),
            Self::X11(s) => KeyboardTarget::key(s.as_ref(), seat, app, key, state, serial, time),
        }
    }
    fn modifiers(&self, seat: &Seat<App>, app: &mut App, mods: ModifiersState, serial: Serial) {
        match self {
            Self::Wayland(s) => KeyboardTarget::modifiers(s, seat, app, mods, serial),
            Self::X11(s) => KeyboardTarget::modifiers(s.as_ref(), seat, app, mods, serial),
        }
    }
}
pub struct State {
    pub shell: XWaylandShellState,
    client: Option<Client>,
    pub wm: Option<X11Wm>,
    pub display: Option<u32>,
    pub failed: bool,
    pub clipboard_generation: Option<u64>,
    pub grab: Option<u32>,
    pub token: Option<smithay::reexports::calloop::RegistrationToken>,
    pub geometries: BTreeMap<u32, Rectangle<i32, Logical>>,
    handle: LoopHandle<'static, App>,
}
impl Drop for State {
    fn drop(&mut self) {
        if let Some(token) = self.token.take() {
            self.handle.remove(token);
        }
    }
}
pub fn start(dh: &DisplayHandle, handle: LoopHandle<'static, App>, enabled: bool) -> Result<State> {
    let mut state = State {
        shell: XWaylandShellState::new::<App>(dh),
        client: None,
        wm: None,
        display: None,
        failed: false,
        clipboard_generation: None,
        grab: None,
        token: None,
        geometries: BTreeMap::new(),
        handle: handle.clone(),
    };
    if !enabled {
        return Ok(state);
    }
    // Only filesystem Unix sockets, created private from the instant of bind; no abstract/TCP listener.
    // Displayfd X servers can omit lock files and clients prefer abstract sockets.
    // Use a separate range and reject either namespace without connecting to another desktop.
    let occupied = fs::read_to_string("/proc/net/unix").unwrap_or_default();
    let number = (1000..4096u32)
        .find(|n| {
            !std::path::Path::new(&format!("/tmp/.X11-unix/X{n}")).exists()
                && !occupied.lines().any(|line| {
                    line.split_whitespace().last() == Some(format!("@/tmp/.X11-unix/X{n}").as_str())
                })
                && !std::path::Path::new(&format!("/tmp/.X{n}-lock")).exists()
        })
        .context("no free private X11 display number")?;
    let mask = unsafe { libc::umask(0o077) };
    let spawned = XWayland::spawn(
        dh,
        Some(number),
        Vec::<(String, String)>::new(),
        false,
        Stdio::null(),
        Stdio::inherit(),
        |_| {},
    );
    unsafe {
        libc::umask(mask);
    }
    let (source, client) = spawned.context("starting Xwayland")?;
    state.client = Some(client.clone());
    let wm_handle = handle.clone();
    let dh = dh.clone();
    state.token = Some(
        handle
            .insert_source(source, move |event, _, app| match event {
                XWaylandEvent::Ready {
                    x11_socket,
                    display_number,
                } => match X11Wm::start_wm(wm_handle.clone(), &dh, x11_socket, client.clone()) {
                    Ok(wm) => {
                        app.x11.wm = Some(wm);
                        app.x11.display = Some(display_number);

                        tracing::info!(
                            display = format!(":{display_number}"),
                            "private rootless Xwayland ready"
                        );
                    }
                    Err(error) => {
                        app.x11.failed = true;
                        tracing::error!(%error,"Xwayland window manager failed");
                    }
                },
                XWaylandEvent::Error => {
                    app.x11.failed = true;
                    tracing::error!("Xwayland failed");
                }
            })
            .map_err(|e| anyhow::anyhow!("registering Xwayland: {}", e.error))?,
    );
    Ok(state)
}
pub(super) fn input_popup(x: &X11Surface) -> bool {
    // Xwayland translates SHAPE input regions to wl_surface input regions.
    // Transparent drag images must never claim menu capture.
    let empty_input = x.wl_surface().is_some_and(|surface| {
        with_states(&surface, |states| {
            states
                .cached_state
                .get::<smithay::wayland::compositor::SurfaceAttributes>()
                .current()
                .input_region
                .as_ref()
                .is_some_and(|r| r.rects.is_empty())
        })
    });
    !empty_input
        && !x.input_region_empty()
        && x.is_override_redirect()
        && !matches!(
            x.window_type(),
            Some(
                smithay::xwayland::xwm::WmWindowType::Tooltip
                    | smithay::xwayland::xwm::WmWindowType::Notification
            )
        )
}

pub fn info(
    x: &X11Surface,
    id: u32,
    size: smithay::utils::Size<i32, Logical>,
    scale: f64,
    windows: &[(X11Surface, u32)],
    geometries: &BTreeMap<u32, Rectangle<i32, Logical>>,
    focused: Option<u32>,
) -> Value {
    let parent = x
        .is_transient_for()
        .and_then(|xid| windows.iter().find(|(p, _)| p.window_id() == xid));
    if x.is_override_redirect() {
        let parent = parent
            .or_else(|| {
                windows
                    .iter()
                    .find(|(w, n)| Some(*n) == focused && !w.is_override_redirect())
            })
            .or_else(|| windows.iter().find(|(w, _)| !w.is_override_redirect()));
        if let Some((owner, parent)) = parent {
            let mut root = owner;
            let mut root_id = *parent;
            for _ in 0..16 {
                if !root.is_override_redirect() {
                    break;
                }
                let Some((p, n)) = root
                    .is_transient_for()
                    .and_then(|xid| windows.iter().find(|(p, _)| p.window_id() == xid))
                else {
                    break;
                };
                if *n == id || *n == root_id {
                    break;
                }
                root = p;
                root_id = *n;
            }
            let geometry = |x: &X11Surface| {
                geometries
                    .get(&x.window_id())
                    .copied()
                    .unwrap_or_else(|| x.geometry())
            };
            let location = geometry(x).loc - geometry(root).loc;
            return json!({"type":"popup","id":id,"parent":parent,"root":root_id,"x":location.x,"y":location.y,"width":size.w,"height":size.h,"x11":true,"input_popup":input_popup(x)});
        }
    }
    json!({"type":"window","id":id,"title":x.title(),"app_id":x.class(),"width":size.w,"height":size.h,"scale":scale,"parent":parent.map(|(_,id)|id),"decoration":if !x.is_decorated() && !x.is_override_redirect(){"server"}else{"client"},"x11":true})
}
impl App {
    pub(super) fn x11_geometry(
        &self,
        window: &X11Surface,
        mut geometry: Rectangle<i32, Logical>,
    ) -> Rectangle<i32, Logical> {
        if window.is_override_redirect() {
            return geometry;
        }
        // Native viewer windows have independent positions. Xwayland still
        // clips absolute pointer input to its shared root screen: an invisible
        // off-screen X11 position must not make a visible native button inert.
        let pixels = self.output.current_mode().unwrap().size;
        let width = (f64::from(pixels.w) / self.default_scale).floor().max(1.0) as i32;
        let height = (f64::from(pixels.h) / self.default_scale).floor().max(1.0) as i32;
        geometry.size.w = geometry.size.w.clamp(1, width);
        geometry.size.h = geometry.size.h.clamp(1, height);
        geometry.loc.x = geometry.loc.x.clamp(0, width - geometry.size.w);
        geometry.loc.y = geometry.loc.y.clamp(0, height - geometry.size.h);
        geometry
    }

    pub(super) fn x11_reflow(&self) {
        for window in self.windows.iter().filter_map(|w| w.window.x11_surface()) {
            let geometry = window.geometry();
            let fitted = self.x11_geometry(window, geometry);
            if fitted != geometry {
                let _ = window.configure(fitted);
            }
        }
    }

    fn x11_fullscreen(&mut self, window: &X11Surface, value: bool) {
        let _ = window.set_fullscreen(value);
        if let Some(remote) = self
            .windows
            .iter_mut()
            .find(|w| w.window.x11_surface() == Some(window))
        {
            remote.state.fullscreen(value);
            remote.dirty = true;
            let message = json!({"type":"window_action","id":remote.id,
                "action":if value {"fullscreen"} else {"unfullscreen"},
                "fullscreen_epoch":remote.state.fullscreen_epoch});
            if self.semantics_enabled() {
                self.control(message);
            }
        }
    }
    pub(super) fn poll_x11_dnd(&mut self) {
        if let Some(wm) = self.x11.wm.as_mut() {
            wm.poll_dnd(&self.x11.handle);
        }
    }
    pub(super) fn x11_scale(&mut self, scale: f64) {
        tracing::debug!(scale, "setting shared Xwayland DPI");
        if let Some(client) = &self.x11.client {
            client
                .get_data::<smithay::xwayland::XWaylandClientData>()
                .unwrap()
                .compositor_state
                .set_client_scale(scale);
        }
        if let Some(wm) = self.x11.wm.as_mut() {
            let _ = wm.set_xsettings(
                [(
                    String::from("Xft/DPI"),
                    smithay::xwayland::xwm::settings::Value::Integer(
                        (96.0 * 1024.0 * scale).round() as i32,
                    ),
                )]
                .into_iter(),
            );
        }
    }
    fn x11_add(&mut self, surface: X11Surface) {
        if surface.window_type() == Some(smithay::xwayland::xwm::WmWindowType::Dnd) {
            return;
        }
        if let Some(w) = self
            .windows
            .iter_mut()
            .find(|w| w.window.x11_surface() == Some(&surface))
        {
            w.dirty = true;
            w.mapped = surface.is_mapped() && surface.wl_surface().is_some();
            return;
        }
        let id = self.next_id;
        self.next_id += 1;
        let output = self.output.clone();
        if let Some(surface) = surface.wl_surface() {
            set_surface_scale(&surface, &output, self.default_scale);
        }
        let popup = surface.is_override_redirect();
        let mapped = surface.is_mapped() && surface.wl_surface().is_some();
        self.windows.push(RemoteWindow {
            id,
            icon: app_icon::WindowIcon::default(),
            state: window_semantics::State {
                fullscreen: surface.is_fullscreen(),
                ..Default::default()
            },
            window: Window::new_x11_window(surface),
            dirty: true,
            announced: false,
            last_info: Value::Null,
            frame_sequence: 0,
            gpu_source: None,
            target: None,
            target_size: (0, 0),
            output,
            output_global: None,
            scale: self.default_scale,
            mapped,
            work_area: Rectangle::from_size((3840, 2160).into()),
        });
        if !popup && !self.activation.negotiated {
            self.focus(Some(id));
        }
        tracing::info!(id, popup, "new X11 application surface");
    }
    pub(super) fn refresh_x11_grab(&mut self) {
        let Some(root) = self.x11.grab else {
            return;
        };
        let valid = self.data_drop.native.is_none()
            && self.window(root).is_some()
            && self.windows.iter().any(|w| {
                w.mapped
                    && w.window.alive()
                    && w.last_info["type"] == "popup"
                    && w.last_info["root"].as_u64() == Some(u64::from(root))
                    && w.window.x11_surface().is_some_and(input_popup)
            });
        if !valid {
            self.x11.grab = None;
            self.control(json!({"type":"popup_grab","root":root,"active":false}));
        }
    }
    pub(super) fn x11_cancel_popup(&mut self, id: u32) {
        if self.x11.grab != Some(id) {
            return;
        }
        self.x11.grab = None;
        self.control(json!({"type":"popup_grab","root":id,"active":false}));
        if self.focused_window != Some(id) {
            return;
        }
        self.release_input();
        let keyboard = self.seat.get_keyboard().unwrap();
        for state in [KeyState::Pressed, KeyState::Released] {
            keyboard.input::<(), _>(
                self,
                9u32.into(),
                state,
                SERIAL_COUNTER.next_serial(),
                self.time(),
                |_, _, _| FilterResult::Forward,
            );
        }
    }
    fn x11_remove(&mut self, surface: &X11Surface) {
        let Some(index) = self
            .windows
            .iter()
            .position(|w| w.window.x11_surface() == Some(surface))
        else {
            return;
        };
        let id = self.windows[index].id;
        if self.focused_window == Some(id) {
            let parent = surface.is_transient_for().and_then(|xid| {
                self.windows
                    .iter()
                    .find(|w| {
                        w.id != id
                            && w.mapped
                            && w.window.x11_surface().is_some_and(|x| x.window_id() == xid)
                    })
                    .map(|w| w.id)
            });
            self.focus(parent);
        }
        let remote = self.windows.remove(index);
        if let Some(global) = remote.output_global {
            self.retire_output(global);
        }
        self.encoder.forget(id);
        if let Some(peer) = &self.peer {
            peer.forget_window(id);
        }
        self.control(json!({"type":"window_closed","id":id}));
        self.refresh_x11_grab();
    }

    fn x11_action(&self, surface: &X11Surface, action: &str, edge: Option<u32>) {
        if let Some(w) = self
            .windows
            .iter()
            .find(|w| w.window.x11_surface() == Some(surface))
        {
            self.control(json!({"type":"window_action","id":w.id,"action":action,"edge":edge}));
        }
    }
}
impl XWaylandShellHandler for App {
    fn xwayland_shell_state(&mut self) -> &mut XWaylandShellState {
        &mut self.x11.shell
    }
    fn surface_associated(&mut self, _: XwmId, _: WlSurface, window: X11Surface) {
        self.x11_add(window);
    }
}
impl XwmHandler for App {
    fn x11_dnd_requested<S: smithay::input::dnd::Source>(
        &mut self,
        source: S,
        seat: Seat<Self>,
        serial: Serial,
        kind: smithay::input::dnd::GrabType,
    ) {
        self.begin_native_drag(source, None, seat, serial, kind);
    }

    fn activation_request(&mut self, _: XwmId, window: X11Surface, _: u32, _: u32) {
        self.activation_x11(window);
    }
    fn xwm_state(&mut self, _: XwmId) -> &mut X11Wm {
        self.x11.wm.as_mut().expect("active XWM callback")
    }
    fn new_window(&mut self, _: XwmId, window: X11Surface) {
        self.x11
            .geometries
            .insert(window.window_id(), window.geometry());
    }
    fn new_override_redirect_window(&mut self, _: XwmId, window: X11Surface) {
        self.x11
            .geometries
            .insert(window.window_id(), window.geometry());
    }

    fn map_window_request(&mut self, _: XwmId, window: X11Surface) {
        let _ = window.configure(self.x11_geometry(&window, window.geometry()));
        if window.set_mapped(true).is_ok() {
            self.x11_add(window);
        }
    }
    fn map_window_notify(&mut self, _: XwmId, window: X11Surface) {
        self.x11_add(window);
    }
    fn mapped_override_redirect_window(&mut self, _: XwmId, window: X11Surface) {
        self.x11_add(window);
    }
    fn unmapped_window(&mut self, _: XwmId, window: X11Surface) {
        self.x11_remove(&window);
    }
    fn destroyed_window(&mut self, _: XwmId, window: X11Surface) {
        self.x11.geometries.remove(&window.window_id());
        self.x11_remove(&window);
    }
    fn configure_request(
        &mut self,
        _: XwmId,
        window: X11Surface,
        x: Option<i32>,
        y: Option<i32>,
        w: Option<u32>,
        h: Option<u32>,
        _: Option<Reorder>,
    ) {
        let mut r = window.geometry();
        if let Some(x) = x {
            r.loc.x = x;
        }
        if let Some(y) = y {
            r.loc.y = y;
        }
        if let Some(w) = w {
            r.size.w = w.clamp(1, 8192) as i32;
        }
        if let Some(h) = h {
            r.size.h = h.clamp(1, 8192) as i32;
        }
        let _ = window.configure(self.x11_geometry(&window, r));
    }
    fn configure_notify(
        &mut self,
        _: XwmId,
        window: X11Surface,
        geometry: Rectangle<i32, Logical>,
        _: Option<u32>,
    ) {
        let fitted = self.x11_geometry(&window, geometry);
        if fitted != geometry {
            let _ = window.configure(fitted);
        }
        self.x11.geometries.insert(window.window_id(), geometry);
        if let Some(w) = self
            .windows
            .iter_mut()
            .find(|w| w.window.x11_surface() == Some(&window))
        {
            w.dirty = true;
        }
    }
    fn property_notify(&mut self, _: XwmId, window: X11Surface, property: WmWindowProperty) {
        if let Some(w) = self
            .windows
            .iter_mut()
            .find(|w| w.window.x11_surface() == Some(&window))
        {
            w.dirty = true;
            if matches!(property, WmWindowProperty::Icon | WmWindowProperty::Class) {
                w.icon.invalidate();
            }
        }
    }
    fn maximize_request(&mut self, _: XwmId, window: X11Surface) {
        let _ = window.set_maximized(true);
        self.x11_action(&window, "maximize", None);
    }
    fn unmaximize_request(&mut self, _: XwmId, window: X11Surface) {
        let _ = window.set_maximized(false);
        self.x11_action(&window, "restore", None);
    }
    fn minimize_request(&mut self, _: XwmId, window: X11Surface) {
        let _ = window.set_suspended(true);
        self.x11_action(&window, "minimize", None);
    }
    fn unminimize_request(&mut self, _: XwmId, window: X11Surface) {
        self.activation_x11(window.clone());
        let _ = window.set_suspended(false);
        self.x11_action(&window, "restore", None);
    }
    fn fullscreen_request(&mut self, _: XwmId, window: X11Surface) {
        self.x11_fullscreen(&window, true);
    }
    fn unfullscreen_request(&mut self, _: XwmId, window: X11Surface) {
        self.x11_fullscreen(&window, false);
    }
    fn resize_request(&mut self, _: XwmId, window: X11Surface, _: u32, edge: ResizeEdge) {
        let edge = match edge {
            ResizeEdge::Top => 1,
            ResizeEdge::Bottom => 2,
            ResizeEdge::Left => 4,
            ResizeEdge::TopLeft => 5,
            ResizeEdge::BottomLeft => 6,
            ResizeEdge::Right => 8,
            ResizeEdge::TopRight => 9,
            ResizeEdge::BottomRight => 10,
        };
        self.x11_action(&window, "resize", Some(edge));
    }
    fn move_request(&mut self, _: XwmId, window: X11Surface, _: u32) {
        self.x11_action(&window, "move", None);
    }
    fn allow_selection_access(&mut self, _: XwmId, selection: SelectionTarget) -> bool {
        selection == SelectionTarget::Clipboard
    }
    fn send_selection(&mut self, _: XwmId, _: SelectionTarget, mime: String, fd: OwnedFd) {
        tracing::debug!(%mime, cached=self.clipboard.contains_key(&mime), "X11 selection data requested");
        use std::os::fd::AsRawFd;
        let flags = unsafe { libc::fcntl(fd.as_raw_fd(), libc::F_GETFL) };
        if flags < 0 {
            return;
        }
        if let Some(bytes) = self.clipboard.get(&mime).cloned() {
            if WRITERS
                .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| {
                    (n < 16).then_some(n + 1)
                })
                .is_err()
            {
                return;
            }
            std::thread::spawn(move || {
                struct Guard;
                impl Drop for Guard {
                    fn drop(&mut self) {
                        WRITERS.fetch_sub(1, Ordering::Relaxed);
                    }
                }
                let _guard = Guard;
                if unsafe { libc::fcntl(fd.as_raw_fd(), libc::F_SETFL, flags | libc::O_NONBLOCK) }
                    < 0
                {
                    return;
                }
                let mut file = fs::File::from(fd);
                let mut offset = 0;
                let deadline = Instant::now() + Duration::from_secs(10);
                while offset < bytes.len() && !shutdown::requested() && Instant::now() < deadline {
                    match file.write(&bytes[offset..]) {
                        Ok(0) => break,
                        Ok(n) => offset += n,
                        Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                        Err(e) if e.kind() == std::io::ErrorKind::WouldBlock => {
                            let mut poll = libc::pollfd {
                                fd: file.as_raw_fd(),
                                events: libc::POLLOUT,
                                revents: 0,
                            };
                            unsafe {
                                libc::poll(&mut poll, 1, 100);
                            }
                        }
                        Err(_) => break,
                    }
                }
            });
        } else {
            // A regular Wayland source receives a blocking pipe, as in native data-device transfers.
            if unsafe { libc::fcntl(fd.as_raw_fd(), libc::F_SETFL, flags & !libc::O_NONBLOCK) } < 0
            {
                return;
            }
            let _ = request_data_device_client_selection::<Self>(&self.seat, mime, fd);
        }
    }

    fn new_selection(&mut self, _: XwmId, selection: SelectionTarget, types: Vec<String>) {
        if selection != SelectionTarget::Clipboard {
            return;
        }
        self.clipboard_generation += 1;
        let generation = self.clipboard_generation;
        self.x11.clipboard_generation = Some(generation);
        self.clipboard.clear();
        self.control(json!({"type":"clipboard","items":{}}));
        for mime in [
            "text/plain;charset=utf-8",
            "text/plain",
            "image/png",
            "text/uri-list",
        ] {
            if !types.iter().any(|m| m == mime) {
                continue;
            }
            let Ok((mut read, write)) = UnixStream::pair() else {
                continue;
            };
            let _ = read.set_read_timeout(Some(Duration::from_secs(10)));
            if self
                .x11
                .wm
                .as_mut()
                .unwrap()
                .send_selection(SelectionTarget::Clipboard, mime.into(), write.into())
                .is_err()
            {
                continue;
            }
            let events = self.events.clone();
            let mime = mime.to_string();
            std::thread::spawn(move || {
                let mut bytes = Vec::new();
                if (&mut read)
                    .take(12 * 1024 * 1024 + 1)
                    .read_to_end(&mut bytes)
                    .is_ok()
                    && bytes.len() <= 12 * 1024 * 1024
                {
                    let _ = events.send(Event::Clipboard {
                        generation,
                        mime,
                        bytes,
                    });
                }
            });
        }
    }
    fn cleared_selection(&mut self, _: XwmId, selection: SelectionTarget) {
        if selection == SelectionTarget::Clipboard && self.x11.clipboard_generation.is_some() {
            self.clipboard_generation += 1;
            self.x11.clipboard_generation = None;
            self.clipboard.clear();
            set_data_device_selection(&self.dh, &self.seat, vec![], Arc::new(BTreeMap::new()));
            self.control(json!({"type":"clipboard","items":{}}));
        }
    }
    fn disconnected(&mut self, _: XwmId) {
        let windows: Vec<_> = self
            .windows
            .iter()
            .filter_map(|w| w.window.x11_surface().cloned())
            .collect();
        for window in windows {
            self.x11_remove(&window);
        }
        self.x11.geometries.clear();
        if self.x11.clipboard_generation.take().is_some() {
            self.clipboard_generation += 1;
        }
        self.x11.display = None;
        self.x11.failed = true;
        tracing::warn!("private Xwayland disconnected; native Wayland session continues");
    }
}
smithay::delegate_xwayland_shell!(App);

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn existing_unlocked_x11_socket_is_never_removed() {
        use std::os::unix::net::UnixListener;
        let (number, path, listener) = (6000..6200)
            .find_map(|n| {
                let path = PathBuf::from(format!("/tmp/.X11-unix/X{n}"));
                if PathBuf::from(format!("/tmp/.X{n}-lock")).exists() {
                    return None;
                }
                UnixListener::bind(&path).ok().map(|l| (n, path, l))
            })
            .expect("free private test display");
        struct Cleanup(PathBuf);
        impl Drop for Cleanup {
            fn drop(&mut self) {
                let _ = fs::remove_file(&self.0);
            }
        }
        let _cleanup = Cleanup(path.clone());
        let inode = fs::metadata(&path).unwrap().ino();
        let display = Display::<App>::new().unwrap();
        let result = XWayland::spawn(
            &display.handle(),
            Some(number),
            Vec::<(String, String)>::new(),
            false,
            Stdio::null(),
            Stdio::null(),
            |_| {},
        );
        assert!(
            result.is_err(),
            "an existing socket without a lock is still occupied"
        );
        assert_eq!(fs::metadata(&path).unwrap().ino(), inode);
        assert!(!PathBuf::from(format!("/tmp/.X{number}-lock")).exists());
        drop(listener);
    }
}
