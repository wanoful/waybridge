//! Private Xwayland XDND adapter. The compositor wrappers only translate an
//! already-authorized independent window's coordinates and enqueue bounded work.
#[path = "xdnd/core.rs"]
mod core;
use crate::*;
#[allow(unused_imports)] // Event kind is consumed by the shared drop handler.
pub use core::{Outcome, OutcomeKind, Payload, Xdnd};

impl App {
    fn xdnd_target_position(&self, id: u32, x: f64, y: f64) -> Option<(u32, (i32, i32))> {
        let window = self.window(id)?;
        let surface = window.x11_surface()?;
        let root = self
            .x11
            .geometries
            .get(&surface.window_id())
            .copied()
            .unwrap_or_else(|| surface.geometry());
        // Callers pass message_position() coordinates, exactly as pointer_motion
        // does. Add the current geometry/crop origin once, then the actual X11
        // root placement. Native HWND positions have no relation to this origin.
        let crop = window.geometry().loc;
        let point = (
            f64::from(root.loc.x + crop.x) + x,
            f64::from(root.loc.y + crop.y) + y,
        );
        Some((
            surface.window_id(),
            (
                point.0.round().clamp(i32::MIN as f64, i32::MAX as f64) as i32,
                point.1.round().clamp(i32::MIN as f64, i32::MAX as f64) as i32,
            ),
        ))
    }
    /// None: not X11. Some(false): handled X11 target, but source unavailable or
    /// input denied. Some(true): enqueued; acceptance/Finished arrive via poll.
    #[allow(dead_code)] // Called by the shared data-drop handler integration.
    pub(crate) fn xdnd_begin_data_drop(
        &mut self,
        id: u32,
        x: f64,
        y: f64,
        payload: Payload,
        generation: u64,
    ) -> Option<bool> {
        let (target, position) = self.xdnd_target_position(id, x, y)?;
        if !self.input_allowed(id) || !x.is_finite() || !y.is_finite() {
            return Some(false);
        }
        Some(
            self.x11
                .xdnd
                .as_ref()
                .is_some_and(|source| source.begin(generation, target, position, payload)),
        )
    }
    #[allow(dead_code)]
    pub(crate) fn xdnd_motion(&self, id: u32, x: f64, y: f64, generation: u64) -> Option<bool> {
        let (target, position) = self.xdnd_target_position(id, x, y)?;
        if !self.input_allowed(id) || !x.is_finite() || !y.is_finite() {
            return Some(false);
        }
        if let Some(source) = self.x11.xdnd.as_ref() {
            source.motion(generation, target, position);
            Some(source.available())
        } else {
            Some(false)
        }
    }
    #[allow(dead_code)]
    pub(crate) fn xdnd_drop(&self, generation: u64) {
        if let Some(source) = &self.x11.xdnd {
            source.drop(generation);
        }
    }
    #[allow(dead_code)]
    pub(crate) fn xdnd_cancel(&self, generation: u64) {
        if let Some(source) = &self.x11.xdnd {
            source.cancel(generation);
        }
    }
    #[allow(dead_code)]
    pub(crate) fn xdnd_cancel_all(&self) {
        if let Some(source) = &self.x11.xdnd {
            source.cancel_all();
        }
    }
    #[allow(dead_code)]
    pub(crate) fn xdnd_poll(&self) -> Vec<Outcome> {
        self.x11.xdnd.as_ref().map_or_else(Vec::new, Xdnd::poll)
    }
}
