//! Shared MIME delivery and target negotiation for Wayland and private Xwayland.
use crate::*;
pub(crate) struct XDrop {
    generation: u64,
    target: u32,
    dropped: bool,
}
impl App {
    pub(crate) fn begin_file_drop(&mut self, id: u32, x: f64, y: f64, uris: Vec<u8>) {
        self.begin_data_drop(
            id,
            x,
            y,
            Arc::new(BTreeMap::from([("text/uri-list".into(), uris)])),
            true,
        );
    }
    pub(crate) fn begin_data_drop(
        &mut self,
        id: u32,
        x: f64,
        y: f64,
        payload: Clipboard,
        drop: bool,
    ) -> bool {
        if self.read_only || !self.input_allowed(id) || !x.is_finite() || !y.is_finite() {
            return false;
        }
        self.server_drop_payloads
            .retain(|_, (_, created)| created.elapsed() < Duration::from_secs(120));
        if self.server_drop_payloads.len() >= 16 {
            return false;
        }
        self.cancel_x11_drop();
        self.incoming_text_drag = None;
        self.drop_window = id;
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
        self.server_drop_generation = self.server_drop_generation.wrapping_add(1).max(1);
        self.server_drop_payloads.insert(
            self.server_drop_generation,
            (payload.clone(), Instant::now()),
        );
        self.drop_data = Some(payload.clone());
        self.drop_accepted = false;
        self.drop_deadline = drop.then(|| Instant::now() + Duration::from_secs(5));
        if let Some(queued) =
            self.xdnd_begin_data_drop(id, x, y, payload.clone(), self.server_drop_generation)
        {
            if queued {
                let target = self
                    .window(id)
                    .and_then(|w| w.x11_surface().map(|s| s.window_id()))
                    .unwrap();
                self.x11_drop = Some(XDrop {
                    generation: self.server_drop_generation,
                    target,
                    dropped: false,
                });
            } else {
                self.server_drop_payloads
                    .remove(&self.server_drop_generation);
                self.drop_data = None;
                self.drop_deadline = None;
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
        self.pressed_buttons.insert(0x110);
        let start = smithay::input::pointer::GrabStartData {
            focus: None,
            button: 0x110,
            location: pointer.current_location(),
        };
        let mimes = payload.keys().cloned().collect();
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
            self.server_drop_generation,
        );
        self.pointer_motion(id, x, y);
        true
    }

    pub(crate) fn cancel_text_drag(&mut self) {
        if self.incoming_text_drag.take().is_none() {
            return;
        }
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
        self.cancel_x11_drop();
        self.release_input();
        self.drop_data = None;
    }
    pub(crate) fn text_drag_message(&mut self, value: &Value) {
        if !self.peer.as_ref().is_some_and(|p| p.data_drag_enabled()) {
            return;
        }
        let Some(drag) = value["drag"].as_u64().filter(|id| *id > 0) else {
            return;
        };
        let kind = value["type"].as_str().unwrap_or("");
        if kind == "text_drag_cancel" {
            if self.incoming_text_drag == Some(drag) {
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
            if data.len() > drag::MAX_TEXT
                || data.contains(&0)
                || std::str::from_utf8(&data).is_err()
            {
                return;
            }
            let (x, y) = self.message_position(id, value);
            let payload = Arc::new(BTreeMap::from([
                ("text/plain;charset=utf-8".into(), data.clone()),
                ("text/plain".into(), data),
            ]));
            if self.begin_data_drop(id, x, y, payload, false) {
                self.incoming_text_drag = Some(drag);
            }
        } else if self.incoming_text_drag == Some(drag) {
            let (x, y) = self.message_position(id, value);
            self.motion_data_drop(id, x, y);
            if kind == "text_drag_drop" {
                self.drop_deadline = Some(Instant::now() + Duration::from_secs(2));
            }
        }
    }

    pub(crate) fn finish_file_drop(&mut self) {
        self.poll_x11_drop();
        if self.x11_drop.is_some() {
            if self
                .drop_deadline
                .is_some_and(|deadline| self.drop_accepted || Instant::now() >= deadline)
            {
                self.drop_deadline = None;
                if let Some(active) = self.x11_drop.as_mut() {
                    if self.drop_accepted && !active.dropped {
                        active.dropped = true;
                        let generation = active.generation;
                        self.xdnd_drop(generation);
                    } else if !self.drop_accepted {
                        self.cancel_x11_drop();
                    }
                }
            }
            return;
        }
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

    pub(crate) fn cancel_x11_drop(&mut self) {
        if let Some(active) = self.x11_drop.take() {
            self.xdnd_cancel(active.generation);
            self.server_drop_payloads.remove(&active.generation);
            if self.server_drop_generation == active.generation {
                self.drop_data = None;
                self.drop_deadline = None;
                self.drop_accepted = false;
            }
        }
    }
    fn motion_data_drop(&mut self, id: u32, x: f64, y: f64) {
        let target = self
            .window(id)
            .and_then(|w| w.x11_surface().map(|s| s.window_id()));
        if target.is_some() != self.x11_drop.is_some() {
            if let Some(payload) = self.drop_data.clone() {
                let text = self.incoming_text_drag;
                let deadline = self.drop_deadline;
                if self.begin_data_drop(id, x, y, payload, false) {
                    self.incoming_text_drag = text;
                    self.drop_deadline = deadline;
                }
            }
            return;
        }
        if self.drop_window != id {
            self.drop_accepted = false;
            if let Some(drag) = self.incoming_text_drag {
                self.control(json!({"type":"text_drag_status","drag":drag,"accepted":false}));
            }
        }
        self.drop_window = id;
        if let Some(active) = self.x11_drop.as_mut() {
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
        let target_alive = self.window(self.drop_window).is_some();
        if self.x11_drop.is_some() && (!target_alive || self.read_only) {
            self.cancel_x11_drop();
        }
        for outcome in self.xdnd_poll() {
            if !self
                .x11_drop
                .as_ref()
                .is_some_and(|d| d.generation == outcome.generation && d.target == outcome.target)
            {
                continue;
            }
            match outcome.kind {
                OutcomeKind::Accepted(accepted) => {
                    self.drop_accepted = accepted;
                    if let Some(id) = self.incoming_text_drag {
                        self.control(
                            json!({"type":"text_drag_status","drag":id,"accepted":accepted}),
                        );
                    }
                }
                OutcomeKind::Dropped => {}
                OutcomeKind::Finished(success) => {
                    if !success {
                        tracing::debug!("X11 target rejected or abandoned data drop");
                    }
                    self.cancel_x11_drop();
                    self.incoming_text_drag = None;
                }
                OutcomeKind::Error(error) => {
                    tracing::debug!(%error,"X11 data drop failed");
                    self.cancel_x11_drop();
                    self.incoming_text_drag = None;
                }
            }
        }
    }
}
