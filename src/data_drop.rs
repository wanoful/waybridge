//! Shared MIME delivery and target negotiation for Wayland and private Xwayland.
use crate::*;
pub(crate) struct XDrop {
    generation: u64,
    target: u32,
    dropped: bool,
}
/// One incoming logical offer owns negotiation, payload snapshots and delivery.
/// Native protocol adapters report facts; this state survives button release.
#[derive(Default)]
pub(crate) struct Coordinator {
    pub outbound: Option<OutboundDrag>,
    pub next_outbound: u64,
    pub payload: Option<data_offer::Offer>,
    pub x11: Option<XDrop>,
    pub window: u32,
    pub gesture: Option<u64>,
    pub target_epoch: u64,
    pub generation: u64,
    pub snapshots: BTreeMap<u64, (data_offer::Offer, Instant)>,
    pub deadline: Option<Instant>,
    pub synthetic_pressed: bool,
    pub decision: Decision,
    pub phase: Phase,
}
#[derive(Default, Clone, Copy, PartialEq, Debug)]
pub(crate) enum Decision {
    #[default]
    Pending,
    Accepted,
    Rejected,
}
#[derive(Default, Clone, Copy, PartialEq, Debug)]
pub(crate) enum Phase {
    #[default]
    Idle,
    Hover,
    Dropping,
    Delivering,
}
impl Coordinator {
    pub fn status(&self) -> Option<Value> {
        Some(
            json!({"type":"text_drag_status","drag":self.gesture?,"target_epoch":self.target_epoch,
            "accepted":self.accepted(),"status":match self.decision {Decision::Pending=>"pending",Decision::Accepted=>"accepted",Decision::Rejected=>"rejected"}}),
        )
    }
    pub fn accepted(&self) -> bool {
        self.decision == Decision::Accepted
    }
    pub fn negotiate(&mut self, accepted: bool, settled: bool) {
        self.decision = if accepted {
            Decision::Accepted
        } else if settled {
            Decision::Rejected
        } else {
            Decision::Pending
        };
    }
    pub fn complete(&mut self, generation: u64, success: bool) -> Option<Value> {
        self.snapshots.remove(&generation);
        if generation != self.generation {
            return None;
        }
        let result=self.gesture.map(|id|json!({"type":"text_drag_finished","drag":id,"target_epoch":self.target_epoch,"success":success && matches!(self.phase,Phase::Dropping|Phase::Delivering)}));
        self.gesture = None;
        self.payload = None;
        self.deadline = None;
        self.decision = Decision::Pending;
        self.phase = Phase::Idle;
        result
    }
}
impl App {
    pub(crate) fn begin_file_drop(&mut self, id: u32, x: f64, y: f64, uris: Vec<u8>) {
        self.begin_data_drop(
            id,
            x,
            y,
            data_offer::Offer::one("text/uri-list", uris),
            true,
        );
    }
    pub(crate) fn begin_data_drop(
        &mut self,
        id: u32,
        x: f64,
        y: f64,
        payload: data_offer::Offer,
        drop: bool,
    ) -> bool {
        if self.read_only || !self.input_allowed(id) || !x.is_finite() || !y.is_finite() {
            return false;
        }
        self.data_drop
            .snapshots
            .retain(|_, (_, created)| created.elapsed() < Duration::from_secs(120));
        if self.data_drop.snapshots.len() >= 16 {
            return false;
        }
        if self.data_drop.phase != Phase::Delivering {
            self.cancel_data_drop();
        }
        self.cancel_x11_drop();
        self.data_drop.gesture = None;
        self.data_drop.window = id;
        let pointer = self.seat.get_pointer().unwrap();
        // Discard old offers before releasing buttons; never turn cancellation
        // into a drop on the previous target. Synthetic setup must not click an app.
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
        self.data_drop.generation = self.data_drop.generation.wrapping_add(1).max(1);
        self.data_drop
            .snapshots
            .insert(self.data_drop.generation, (payload.clone(), Instant::now()));
        self.data_drop.payload = Some(payload.clone());
        self.data_drop.negotiate(false, false);
        self.data_drop.phase = if drop { Phase::Dropping } else { Phase::Hover };
        self.data_drop.deadline = drop.then(|| Instant::now() + Duration::from_secs(5));
        if let Some(queued) =
            self.xdnd_begin_data_drop(id, x, y, payload.snapshot(), self.data_drop.generation)
        {
            if queued {
                let target = self
                    .window(id)
                    .and_then(|w| w.x11_surface().map(|s| s.window_id()))
                    .unwrap();
                self.data_drop.x11 = Some(XDrop {
                    generation: self.data_drop.generation,
                    target,
                    dropped: false,
                });
            } else {
                self.data_drop.snapshots.remove(&self.data_drop.generation);
                self.data_drop.payload = None;
                self.data_drop.deadline = None;
            }
            return queued;
        }
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
        self.data_drop.synthetic_pressed = true;
        let start = smithay::input::pointer::GrabStartData {
            focus: None,
            button: 0x110,
            location: pointer.current_location(),
        };
        let mimes = payload.mime_types();
        let dh = self.dh.clone();
        let seat = self.seat.clone();
        start_dnd_with_id(
            &dh,
            &seat,
            self,
            serial,
            Some(start),
            None,
            SourceMetadata {
                mime_types: mimes,
                dnd_action: DndAction::Copy,
            },
            self.data_drop.generation,
        );
        self.pointer_motion(id, x, y);
        true
    }

    pub(crate) fn cancel_text_drag(&mut self) {
        if self.data_drop.gesture.is_some() {
            self.cancel_data_drop();
        }
    }
    pub(crate) fn cancel_data_drop(&mut self) {
        if self.data_drop.phase == Phase::Idle && !self.data_drop.synthetic_pressed {
            return;
        }
        let generation = self.data_drop.generation;
        let delivering = self.data_drop.phase == Phase::Delivering;
        if delivering {
            // A dropped offer may still read its snapshot after another gesture.
            self.data_drop.gesture = None;
            self.data_drop.payload = None;
            self.data_drop.phase = Phase::Idle;
            self.data_drop.deadline = None;
            self.data_drop.negotiate(false, false);
        } else if let Some(done) = self.data_drop.complete(generation, false) {
            self.control(done);
        }
        self.cancel_x11_drop();
        let pointer = self.seat.get_pointer().unwrap();
        if self.data_drop.synthetic_pressed {
            self.data_drop.synthetic_pressed = false;
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
    pub(crate) fn text_drag_message(&mut self, value: &Value) {
        if !self.text_drag_enabled || !self.peer.as_ref().is_some_and(|p| p.data_drag_enabled()) {
            return;
        }
        let Some(drag) = value["drag"].as_u64().filter(|id| *id > 0) else {
            return;
        };
        let kind = value["type"].as_str().unwrap_or("");
        if kind == "text_drag_cancel" {
            if self.data_drop.gesture == Some(drag)
                && value["target_epoch"].as_u64().unwrap_or(0) == self.data_drop.target_epoch
            {
                self.cancel_text_drag();
            }
            return;
        }
        let Some(id) = value["id"].as_u64().and_then(|v| u32::try_from(v).ok()) else {
            return;
        };
        if self.read_only || !self.input_allowed(id) {
            self.cancel_text_drag();
            return;
        }
        if kind == "text_drag_enter" {
            self.cancel_text_drag();
            let Some(data) = value["data"]
                .as_str()
                .filter(|s| s.len() <= 1_400_000)
                .and_then(|s| B64.decode(s).ok())
            else {
                return;
            };
            let (x, y) = self.message_position(id, value);
            let Ok(payload) = data_offer::Offer::text(data) else {
                return;
            };
            if self.begin_data_drop(id, x, y, payload, false) {
                self.data_drop.gesture = Some(drag);
                self.data_drop.target_epoch = value["target_epoch"].as_u64().unwrap_or(0);
            } else {
                self.control(json!({"type":"text_drag_status","drag":drag,"target_epoch":value["target_epoch"].as_u64().unwrap_or(0),"accepted":false,"status":"rejected","reason":"target_unavailable_or_busy"}));
            }
        } else if kind == "text_drag_retarget"
            && self.data_drop.gesture == Some(drag)
            && value["target_epoch"]
                .as_u64()
                .is_some_and(|epoch| epoch > self.data_drop.target_epoch)
            && self.data_drop.phase == Phase::Hover
        {
            self.data_drop.target_epoch = value["target_epoch"].as_u64().unwrap();
            self.data_drop.negotiate(false, false);
            if self.data_drop.x11.is_some() {
                let epoch = self.data_drop.target_epoch;
                if let Some(payload) = self.data_drop.payload.clone() {
                    self.data_drop.gesture = None;
                    let (x, y) = self.message_position(id, value);
                    if self.begin_data_drop(id, x, y, payload, false) {
                        self.data_drop.gesture = Some(drag);
                        self.data_drop.target_epoch = epoch;
                    }
                }
                return;
            }
            if self.data_drop.x11.is_none() {
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
            }
            let (x, y) = self.message_position(id, value);
            self.motion_data_drop(id, x, y);
        } else if self.data_drop.gesture == Some(drag)
            && value["target_epoch"].as_u64().unwrap_or(0) == self.data_drop.target_epoch
            && self.data_drop.phase == Phase::Hover
        {
            let (x, y) = self.message_position(id, value);
            self.motion_data_drop(id, x, y);
            if kind == "text_drag_drop" {
                self.data_drop.phase = Phase::Dropping;
                self.data_drop.deadline = Some(Instant::now() + Duration::from_secs(2));
            }
        }
    }

    pub(crate) fn finish_file_drop(&mut self) {
        if matches!(self.data_drop.phase, Phase::Hover | Phase::Dropping)
            && self.window(self.data_drop.window).is_none()
        {
            self.cancel_data_drop();
            return;
        }
        self.poll_x11_drop();
        if self.data_drop.x11.is_some() {
            if self
                .data_drop
                .deadline
                .is_some_and(|deadline| self.data_drop.accepted() || Instant::now() >= deadline)
            {
                self.data_drop.deadline = None;
                let accepted = self.data_drop.accepted();
                if let Some(active) = self.data_drop.x11.as_mut() {
                    if accepted && !active.dropped {
                        active.dropped = true;
                        self.data_drop.phase = Phase::Delivering;
                        let generation = active.generation;
                        self.xdnd_drop(generation);
                    } else if !self.data_drop.accepted() {
                        self.cancel_x11_drop();
                    }
                }
            }
            return;
        }
        if self
            .data_drop
            .deadline
            .is_some_and(|deadline| self.data_drop.accepted() || Instant::now() >= deadline)
        {
            self.data_drop.deadline = None;
            self.data_drop.synthetic_pressed = false;
            self.data_drop.phase = Phase::Delivering;
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

    pub(crate) fn cancel_x11_drop(&mut self) {
        if let Some(active) = self.data_drop.x11.take() {
            self.xdnd_cancel(active.generation);
            if let Some(done) = self.data_drop.complete(active.generation, false) {
                self.control(done);
            }
        }
    }
    fn motion_data_drop(&mut self, id: u32, x: f64, y: f64) {
        let target = self
            .window(id)
            .and_then(|w| w.x11_surface().map(|s| s.window_id()));
        if target.is_some() != self.data_drop.x11.is_some() {
            if let Some(payload) = self.data_drop.payload.clone() {
                let text = self.data_drop.gesture.take();
                let epoch = self.data_drop.target_epoch;
                let deadline = self.data_drop.deadline;
                if self.begin_data_drop(id, x, y, payload, false) {
                    self.data_drop.gesture = text;
                    self.data_drop.target_epoch = epoch;
                    self.data_drop.deadline = deadline;
                }
            }
            return;
        }
        if self.data_drop.window != id {
            self.data_drop.negotiate(false, false);
            if let Some(status) = self.data_drop.status() {
                self.control(status);
            }
        }
        self.data_drop.window = id;
        if let Some(active) = self.data_drop.x11.as_mut() {
            active.target = target.unwrap();
            let generation = active.generation;
            if self.xdnd_motion(id, x, y, generation) == Some(false) {
                self.cancel_x11_drop();
            }
        } else {
            self.pointer_motion(id, x, y);
        }
    }
    fn poll_x11_drop(&mut self) {
        use x11::xdnd::OutcomeKind;
        let target_alive = self.window(self.data_drop.window).is_some();
        if self.data_drop.x11.is_some() && (!target_alive || self.read_only) {
            self.cancel_data_drop();
        }
        for outcome in self.xdnd_poll() {
            if !self
                .data_drop
                .x11
                .as_ref()
                .is_some_and(|d| d.generation == outcome.generation && d.target == outcome.target)
            {
                continue;
            }
            match outcome.kind {
                OutcomeKind::Accepted(accepted) => {
                    self.data_drop.negotiate(accepted, true);
                    if let Some(status) = self.data_drop.status() {
                        self.control(status);
                    }
                }
                OutcomeKind::Dropped => {}
                OutcomeKind::Finished(success) => {
                    if !success {
                        tracing::debug!("X11 target rejected or abandoned data drop");
                    }
                    if let Some(done) = self.data_drop.complete(outcome.generation, success) {
                        self.control(done);
                    }
                    self.cancel_x11_drop();
                }
                OutcomeKind::Error(error) => {
                    tracing::debug!(%error,"X11 data drop failed");
                    if let Some(done) = self.data_drop.complete(outcome.generation, false) {
                        self.control(done);
                    }
                    self.cancel_x11_drop();
                }
            }
        }
    }
}
