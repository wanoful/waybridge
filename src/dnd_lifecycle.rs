//! Source ownership and feedback shared by Wayland, X11 and external deliveries.
//! Native callbacks publish facts; they never re-enter a locked pointer grab.
use crate::*;
use smithay::input::dnd::{
    DnDGrab, DndAction as Action, DndGrabHandler, DndTarget, GrabType, Source, SourceMetadata,
};
use smithay::wayland::selection::data_device::WaylandDndGrabHandler;
use std::{
    any::Any,
    sync::{atomic::AtomicBool, Mutex},
};

#[derive(Default, Clone)]
pub(crate) struct Feedback {
    pub accepted: bool,
    pub settled: bool,
    pub action: Action,
    pub finished: Option<bool>,
}
pub(crate) type FeedbackQueue = Arc<Mutex<BTreeMap<u64, Feedback>>>;
pub(crate) struct TrackedSource {
    pub id: u64,
    inner: Arc<dyn Source>,
    feedback: FeedbackQueue,
    done: AtomicBool,
    dropped: AtomicBool,
    allow_unaccepted: bool,
}
impl TrackedSource {
    fn new<S: Source>(
        id: u64,
        inner: S,
        feedback: FeedbackQueue,
        allow_unaccepted: bool,
    ) -> Arc<Self> {
        Arc::new(Self {
            id,
            inner: Arc::new(inner),
            feedback,
            done: AtomicBool::new(false),
            dropped: AtomicBool::new(false),
            allow_unaccepted,
        })
    }
    fn update(&self, f: impl FnOnce(&mut Feedback)) {
        let mut q = self.feedback.lock().unwrap();
        if q.len() >= 64 && !q.contains_key(&self.id) {
            return;
        }
        f(q.entry(self.id).or_default());
    }
    fn finish(&self, success: bool) {
        if !self.done.swap(true, Ordering::AcqRel) {
            if success {
                self.inner.finished()
            } else {
                self.inner.cancel()
            };
            self.update(|f| f.finished = Some(success));
        }
    }
    pub fn is_files(&self) -> bool {
        self.metadata()
            .is_some_and(|m| m.mime_types.iter().any(|s| s == "text/uri-list"))
    }
}
impl IsAlive for TrackedSource {
    fn alive(&self) -> bool {
        !self.done.load(Ordering::Acquire) && self.inner.alive()
    }
}
impl Source for TrackedSource {
    fn metadata(&self) -> Option<SourceMetadata> {
        self.inner.metadata()
    }
    fn preferred_action(&self) -> Option<Action> {
        self.inner.preferred_action()
    }
    fn is_client_local(&self, target: &dyn Any) -> bool {
        self.inner.is_client_local(target)
    }
    fn accepted(&self, mime: Option<String>) {
        if !self.done.load(Ordering::Acquire) {
            self.inner.accepted(mime)
        }
    }
    fn choose_action(&self, action: Action) {
        if !self.done.load(Ordering::Acquire) {
            self.inner.choose_action(action)
        }
    }
    fn offer_status(&self, accepted: bool, action: Action, settled: bool) {
        if !self.done.load(Ordering::Acquire) {
            self.inner.offer_status(accepted, action, settled);
            self.update(|f| {
                f.accepted = accepted;
                f.action = action;
                f.settled = settled;
            });
        }
    }
    fn send(&self, mime: &str, fd: OwnedFd) {
        if self.alive() {
            self.inner.send(mime, fd)
        }
    }
    fn drop_performed(&self) {
        if !self.done.load(Ordering::Acquire) && !self.dropped.swap(true, Ordering::AcqRel) {
            self.inner.drop_performed()
        }
    }
    fn cancel(&self) {
        self.finish(false)
    }
    fn finished(&self) {
        self.finish(true)
    }
    fn allow_unaccepted_drop(&self) -> bool {
        self.allow_unaccepted
    }
}
#[derive(Clone)]
pub(crate) struct SharedSource(pub Arc<TrackedSource>);
impl IsAlive for SharedSource {
    fn alive(&self) -> bool {
        self.0.alive()
    }
}
impl Source for SharedSource {
    fn metadata(&self) -> Option<SourceMetadata> {
        self.0.metadata()
    }
    fn preferred_action(&self) -> Option<Action> {
        self.0.preferred_action()
    }
    fn is_client_local(&self, target: &dyn Any) -> bool {
        self.0.is_client_local(target)
    }
    fn accepted(&self, mime: Option<String>) {
        self.0.accepted(mime)
    }
    fn choose_action(&self, action: Action) {
        self.0.choose_action(action)
    }
    fn offer_status(&self, accepted: bool, action: Action, settled: bool) {
        self.0.offer_status(accepted, action, settled)
    }
    fn send(&self, mime: &str, fd: OwnedFd) {
        self.0.send(mime, fd)
    }
    fn drop_performed(&self) {
        self.0.drop_performed()
    }
    fn cancel(&self) {
        self.0.cancel()
    }
    fn finished(&self) {
        self.0.finished()
    }
    fn allow_unaccepted_drop(&self) -> bool {
        self.0.allow_unaccepted_drop()
    }
}
pub(crate) struct NativeDrag {
    pub source: Arc<TrackedSource>,
    pub window: u32,
    pub exported: bool,
    pub metadata_ready: bool,
    pub toplevel: bool,
    pub touch: bool,
}
struct MimeSource {
    payload: std::sync::Weak<BTreeMap<String, Vec<u8>>>,
    mimes: Vec<String>,
    created: Instant,
}
impl IsAlive for MimeSource {
    fn alive(&self) -> bool {
        self.created.elapsed() < Duration::from_secs(120) && self.payload.strong_count() > 0
    }
}
impl Source for MimeSource {
    fn metadata(&self) -> Option<SourceMetadata> {
        Some(SourceMetadata {
            mime_types: self.mimes.clone(),
            dnd_actions: [Action::Copy].into_iter().collect(),
        })
    }
    fn choose_action(&self, _: Action) {}
    fn send(&self, mime: &str, fd: OwnedFd) {
        if self.alive() {
            if let Some(payload) = self.payload.upgrade() {
                drag::send_payload(fd, payload, mime.to_owned());
            }
        }
    }
    fn drop_performed(&self) {}
    fn cancel(&self) {}
    fn finished(&self) {}
}
impl App {
    pub(crate) fn next_source(&mut self) -> u64 {
        self.data_drop.source_sequence = self.data_drop.source_sequence.wrapping_add(1).max(1);
        self.data_drop.source_sequence
    }
    pub(crate) fn mime_source(&self, id: u64, payload: data_offer::Offer) -> SharedSource {
        SharedSource(TrackedSource::new(
            id,
            MimeSource {
                payload: Arc::downgrade(&payload.snapshot()),
                mimes: payload.mime_types(),
                created: Instant::now(),
            },
            self.data_drop.feedback.clone(),
            false,
        ))
    }
    pub(crate) fn begin_native_drag<S: Source>(
        &mut self,
        source: S,
        icon: Option<WlSurface>,
        seat: Seat<Self>,
        serial: Serial,
        kind: GrabType,
    ) {
        if self.read_only {
            source.cancel();
            return;
        }
        let resource = (&source as &dyn Any)
            .downcast_ref::<WlDataSource>()
            .cloned();
        let attached = resource
            .as_ref()
            .is_some_and(|s| self.window_drag.is_toplevel_source(s));
        let contact_drag = matches!(kind, GrabType::Touch | GrabType::Tablet);
        let id = self.next_source();
        let tablet_start = if kind == GrabType::Tablet {
            let Some(start) = self
                .pen_drag_start(serial)
                .and_then(|pen| self.start_pen_drag(pen, id))
            else {
                source.cancel();
                return;
            };
            Some(start)
        } else {
            None
        };
        if kind == GrabType::Touch && !self.start_touch_interaction(serial, "dnd", None, Some(id)) {
            source.cancel();
            return;
        }
        self.start_drag_visual(resource.as_ref(), icon);
        self.start_window_drag(resource);
        let source = TrackedSource::new(id, source, self.data_drop.feedback.clone(), attached);
        self.data_drop.native = Some(NativeDrag {
            source: source.clone(),
            toplevel: attached,
            window: if contact_drag {
                self.focused_window
            } else {
                self.pointer_window.or(self.focused_window)
            }
            .unwrap_or(0),
            metadata_ready: source
                .metadata()
                .is_some_and(|m| !m.mime_types.is_empty() && !m.dnd_actions.is_empty()),
            exported: false,
            touch: contact_drag,
        });
        let dh = self.dh.clone();
        match kind {
            GrabType::Pointer => {
                let p = seat.get_pointer().unwrap();
                if let Some(start) = p.grab_start_data() {
                    p.set_grab(
                        self,
                        DnDGrab::new_pointer(&dh, start, SharedSource(source), seat),
                        serial,
                        smithay::input::pointer::Focus::Keep,
                    )
                }
            }
            GrabType::Touch => {
                let p = seat.get_touch().unwrap();
                if let Some(start) = p.grab_start_data() {
                    p.cancel(self);
                    p.set_grab(
                        self,
                        DnDGrab::new_touch(&dh, start, SharedSource(source), seat),
                        serial,
                    )
                }
            }
            GrabType::Tablet => {
                if let (Some(p), Some(start)) = (seat.get_touch(), tablet_start) {
                    p.set_grab(
                        self,
                        DnDGrab::new_touch(&dh, start, SharedSource(source), seat),
                        SERIAL_COUNTER.next_serial(),
                    )
                }
            }
        }
    }
    pub(crate) fn poll_dnd_feedback(&mut self) {
        self.data_drop
            .snapshots
            .retain(|_, (_, created)| created.elapsed() < Duration::from_secs(120));
        if let Some(id) = self.data_drop.cancel_outbound.take() {
            if self.data_drop.outbound.as_ref().is_some_and(|d| d.id == id) {
                self.end_outbound(false);
            }
        }
        if self.data_drop.native.as_ref().is_some_and(|n| {
            !n.source.alive()
                || (!n.toplevel
                    && (self.window(n.window).is_none()
                        || self.data_drop.point.is_some_and(|(id, _, _)| {
                            id != 0 && self.window(self.root_id(id)).is_none()
                        })))
        }) {
            self.cancel_window_drag();
        }
        if self.data_drop.native.as_ref().is_some_and(|n| {
            !n.metadata_ready
                && n.source
                    .metadata()
                    .is_some_and(|m| !m.mime_types.is_empty() && !m.dnd_actions.is_empty())
        }) {
            self.data_drop.native.as_mut().unwrap().metadata_ready = true;
            if let Some((id, x, y)) = self.data_drop.point {
                if self.data_drop.native.as_ref().is_some_and(|n| n.touch) {
                    self.touch_drag_motion(id, x, y);
                } else {
                    self.pointer_motion(id, x, y);
                }
            }
        }
        let feedback = std::mem::take(&mut *self.data_drop.feedback.lock().unwrap());
        for (id, f) in feedback {
            if id == self.data_drop.generation {
                if let Some(success) = f.finished {
                    if let Some(done) = self.data_drop.complete(id, success) {
                        self.control(done)
                    }
                } else {
                    self.data_drop
                        .negotiate(f.accepted && f.action == Action::Copy, f.settled);
                    if let Some(status) = self.data_drop.status() {
                        self.control(status)
                    }
                }
            }
            let returned = self
                .data_drop
                .outbound
                .as_ref()
                .filter(|d| d.source.id == id && d.returned && !d.external)
                .map(|d| (d.id, d.return_token));
            if let Some((drag, token)) = returned {
                let touch = self.touch_source_active(id);
                if !touch || f.settled {
                    self.control(json!({"type":"drag_return_ready","drag":drag,"return_token":token,"accepted":f.accepted&&f.action!=Action::None}));
                }
                if f.finished.is_none() {
                    self.touch_return_feedback(id, f.settled);
                }
            }
            if let Some(success) = f.finished {
                self.touch_source_finished(id, success);
            }
            if f.finished.is_some() {
                if self
                    .data_drop
                    .outbound
                    .as_ref()
                    .is_some_and(|d| d.source.id == id)
                {
                    self.end_outbound(f.finished == Some(true));
                }
                if self
                    .data_drop
                    .native
                    .as_ref()
                    .is_some_and(|d| d.source.id == id)
                {
                    self.data_drop.native = None;
                    self.window_drag.ended();
                    self.stop_drag_visual();
                }
                if id != self.data_drop.generation {
                    self.data_drop.snapshots.remove(&id);
                }
            }
        }
    }
    pub(crate) fn prepare_native_export(&mut self) {
        let Some(native) = self.data_drop.native.as_ref().filter(|d| !d.exported) else {
            return;
        };
        if self.data_drop.outbound.is_some()
            || (native.touch && !self.touch_source_held(native.source.id))
            || (!native.touch
                && (!self.pressed_buttons.contains(&0x110) || self.pressed_buttons.len() != 1))
        {
            return;
        }
        let source = native.source.clone();
        let window = native.window;
        let Some(metadata) = source.metadata() else {
            return;
        };
        if !metadata.dnd_actions.contains(&Action::Copy) {
            return;
        }
        let uri = metadata.mime_types.iter().any(|m| m == "text/uri-list");
        if (uri && !self.drag_enabled) || (!uri && !self.text_drag_enabled) {
            return;
        }
        let Some(peer) = self.peer.clone() else {
            return;
        };
        let mime = if uri {
            Some("text/uri-list".to_owned())
        } else if peer.data_drag_enabled() {
            ["text/plain;charset=utf-8", "text/plain"]
                .iter()
                .find(|m| metadata.mime_types.iter().any(|s| s == **m))
                .map(|m| (*m).to_owned())
        } else {
            None
        };
        let Some(mime) = mime else { return };
        let Ok((read, write)) = UnixStream::pair() else {
            return;
        };
        self.data_drop.next_outbound += 1;
        let id = self.data_drop.next_outbound;
        let cancelled = Arc::new(AtomicBool::new(false));
        let scheduled = if uri {
            drag::prepare(id, read, peer, self.events.clone(), cancelled.clone())
        } else {
            drag::prepare_text(
                id,
                read,
                mime.clone(),
                peer,
                self.events.clone(),
                cancelled.clone(),
            )
        };
        if !scheduled {
            return;
        }
        source.send(&mime, write.into());
        self.data_drop.native.as_mut().unwrap().exported = true;
        self.data_drop.outbound = Some(OutboundDrag {
            id,
            window,
            source,
            cancelled,
            files: None,
            external: false,
            returned: false,
            return_token: 0,
            ready: false,
            dropped: false,
            deferred_focus: None,
            activity: Instant::now(),
        });
    }
}
impl WaylandDndGrabHandler for App {
    fn tablet_drag_origin(&self, seat: &Seat<Self>, origin: &WlSurface, serial: Serial) -> bool {
        seat == &self.seat
            && self.data_drop.native.is_none()
            && self.pressed_buttons.is_empty()
            && self
                .pen_drag_start(serial)
                .is_some_and(|start| start.surface == *origin)
    }

    fn dnd_requested<S: Source>(
        &mut self,
        source: S,
        icon: Option<WlSurface>,
        seat: Seat<Self>,
        serial: Serial,
        kind: GrabType,
    ) {
        self.begin_native_drag(source, icon, seat, serial, kind);
    }
}
impl DndGrabHandler for App {
    fn dropped(
        &mut self,
        _: Option<DndTarget<'_, Self>>,
        validated: bool,
        _: Seat<Self>,
        _: Point<f64, Logical>,
    ) {
        self.stop_drag_visual();
        self.window_drag.ended();
        if let Some(native) = &self.data_drop.native {
            self.touch_source_finished(native.source.id, validated);
        }
        self.data_drop.native = None;
        if self
            .data_drop
            .outbound
            .as_ref()
            .is_some_and(|d| !d.dropped && (!d.external || !validated))
        {
            self.data_drop.cancel_outbound = self.data_drop.outbound.as_ref().map(|d| d.id);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Read;

    #[test]
    fn retained_native_offer_does_not_pin_retired_payload() {
        let offer = data_offer::Offer::text(b"retired snapshot".to_vec()).unwrap();
        let mut coordinator = data_drop::Coordinator::default();
        coordinator
            .snapshots
            .insert(1, (offer.clone(), Instant::now()));
        let source = MimeSource {
            payload: Arc::downgrade(&offer.snapshot()),
            mimes: offer.mime_types(),
            created: Instant::now(),
        };
        drop(offer);
        assert!(source.alive());
        // A client may retain the protocol offer after its bounded snapshot is
        // retired. It must not retain bytes or read another gesture's payload.
        coordinator.snapshots.clear();
        assert!(!source.alive());
        let (mut reader, writer) = UnixStream::pair().unwrap();
        reader
            .set_read_timeout(Some(Duration::from_secs(1)))
            .unwrap();
        source.send("text/plain", writer.into());
        assert_eq!(reader.read(&mut [0; 1]).unwrap(), 0);
    }
}
