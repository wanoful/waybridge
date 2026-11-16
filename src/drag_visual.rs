//! Bounded rendering of the standard wl_data_device.start_drag icon surface.
//! This is visual state only: input capture, offers and application activation
//! remain in the ordinary DND state machine.
use super::*;
use smithay::{
    backend::renderer::{buffer_dimensions, utils::RendererSurfaceStateUserData},
    desktop::utils::send_frames_surface_tree,
    wayland::compositor::SurfaceAttributes,
};

const INTERVAL: Duration = Duration::from_millis(34);
const MAX_EDGE: i32 = 512;
const MAX_PIXELS: i64 = 65_536;

pub(crate) struct DragVisual {
    pub enabled: bool,
    icon: Option<WlSurface>,
    source: Option<WlDataSource>,
    running: bool,
    generation: u64,
    revision: u64,
    dirty: bool,
    offset: Point<i32, Logical>,
    last_tick: Instant,
    target: Option<GlesRenderbuffer>,
    target_size: (i32, i32),
}
impl Default for DragVisual {
    fn default() -> Self {
        Self {
            enabled: false,
            icon: None,
            source: None,
            running: false,
            generation: 0,
            revision: 0,
            dirty: false,
            offset: (0, 0).into(),
            last_tick: Instant::now() - INTERVAL,
            target: None,
            target_size: (0, 0),
        }
    }
}
fn bounded(surface: &WlSurface, bbox: Rectangle<i32, Logical>) -> bool {
    if bbox.size.w <= 0
        || bbox.size.h <= 0
        || bbox.size.w > MAX_EDGE
        || bbox.size.h > MAX_EDGE
        || i64::from(bbox.size.w) * i64::from(bbox.size.h) > MAX_PIXELS
    {
        return false;
    }
    let mut count = 0usize;
    let mut pixels = 0i64;
    let mut valid = true;
    with_surface_tree_downward(
        surface,
        (),
        |_, _, _| TraversalAction::DoChildren(()),
        |_, states, _| {
            count += 1;
            valid &= count <= 16;
            if let Some(size) = states
                .data_map
                .get::<RendererSurfaceStateUserData>()
                .and_then(|s| {
                    s.lock()
                        .unwrap()
                        .buffer()
                        .and_then(|b| buffer_dimensions(b))
                })
            {
                pixels = pixels.saturating_add(i64::from(size.w) * i64::from(size.h));
                valid &= size.w > 0
                    && size.h > 0
                    && size.w <= MAX_EDGE
                    && size.h <= MAX_EDGE
                    && pixels <= MAX_PIXELS;
            }
        },
        |_, _, _| true,
    );
    valid
}
fn capture_scale(size: smithay::utils::Size<i32, Logical>, scale: f64) -> f64 {
    let pixels = f64::from(size.w) * f64::from(size.h);
    let mut value = scale
        .clamp(1.0, 4.0)
        .min(f64::from(MAX_EDGE) / f64::from(size.w))
        .min(f64::from(MAX_EDGE) / f64::from(size.h))
        .min((MAX_PIXELS as f64 / pixels).sqrt());
    if (f64::from(size.w) * value).round() * (f64::from(size.h) * value).round() > MAX_PIXELS as f64
    {
        // Leave rounding room for asymmetric dimensions at the area boundary.
        value *= ((MAX_PIXELS - 1024) as f64 / MAX_PIXELS as f64).sqrt();
    }
    value.max(1.0)
}
impl App {
    pub(crate) fn start_drag_visual(
        &mut self,
        source: Option<&WlDataSource>,
        icon: Option<WlSurface>,
    ) {
        self.stop_drag_visual();
        self.drag_visual.generation += 1;
        self.drag_visual.revision = 0;
        self.drag_visual.running = true;
        self.drag_visual.icon = icon;
        self.drag_visual.source = source.cloned();
        self.drag_visual.offset = (0, 0).into();
        self.drag_visual.dirty = true;
        self.drag_visual.last_tick = Instant::now() - INTERVAL;
        if let Some(icon) = self.drag_visual.icon.clone() {
            self.commit_drag_visual(&icon);
        }
    }
    pub(crate) fn stop_drag_visual(&mut self) {
        if self.drag_visual.running {
            self.drag_visual.revision += 1;
            if self.drag_visual.enabled {
                if let Some(peer) = &self.peer {
                    peer.drag_visual(
                        json!({"type":"drag_visual","generation":self.drag_visual.generation,
                        "revision":self.drag_visual.revision,"active":false}),
                    );
                }
            }
        }
        self.drag_visual.running = false;
        self.drag_visual.icon = None;
        self.drag_visual.source = None;
        self.drag_visual.target = None;
        self.drag_visual.target_size = (0, 0);
    }
    pub(crate) fn commit_drag_visual(&mut self, surface: &WlSurface) {
        let Some(icon) = self.drag_visual.icon.as_ref() else {
            return;
        };
        let mut root = surface.clone();
        while let Some(parent) = get_parent(&root) {
            root = parent;
        }
        if &root != icon {
            return;
        }
        self.drag_visual.dirty = true;
        if surface == icon {
            let delta = with_states(surface, |s| {
                s.cached_state
                    .get::<SurfaceAttributes>()
                    .current()
                    .buffer_delta
                    .take()
            });
            if let Some(delta) = delta {
                self.drag_visual.offset.x = self
                    .drag_visual
                    .offset
                    .x
                    .saturating_add(delta.x)
                    .clamp(-32768, 32768);
                self.drag_visual.offset.y = self
                    .drag_visual
                    .offset
                    .y
                    .saturating_add(delta.y)
                    .clamp(-32768, 32768);
            }
        }
    }
    pub(crate) fn render_drag_visual(&mut self) {
        if !self.drag_visual.running {
            return;
        }
        if self.drag_visual.icon.as_ref().is_some_and(|s| !s.alive())
            || self
                .drag_visual
                .source
                .as_ref()
                .is_some_and(|s| !s.is_alive())
        {
            self.stop_drag_visual();
            return;
        }
        if self.drag_visual.last_tick.elapsed() < INTERVAL {
            return;
        }
        self.drag_visual.last_tick = Instant::now();
        let Some(surface) = self.drag_visual.icon.clone() else {
            return;
        };
        let (scale, output) = self
            .windows
            .iter()
            .find(|w| Some(w.id) == self.focused_window)
            .map(|w| (w.scale, w.output.clone()))
            .unwrap_or((self.default_scale, self.output.clone()));
        set_surface_scale(&surface, &output, scale);
        if self.drag_visual.dirty {
            self.drag_visual.dirty = false;
            self.drag_visual.revision += 1;
            if let Some(peer) = self.peer.clone().filter(|_| self.drag_visual.enabled) {
                let mut message = json!({"type":"drag_visual","generation":self.drag_visual.generation,
                    "revision":self.drag_visual.revision,"active":true,"visible":false});
                let bbox = bbox_from_surface_tree(&surface, (0, 0));
                if bounded(&surface, bbox) {
                    let scale = capture_scale(bbox.size, scale);
                    match capture_surface(
                        &mut self.renderer,
                        &surface,
                        0,
                        0,
                        bbox,
                        scale,
                        &mut self.drag_visual.target,
                        &mut self.drag_visual.target_size,
                        peer.clone(),
                        None,
                        &mut None,
                        self.color_programs.as_ref(),
                        None,
                    ) {
                        Ok(mut frame) => {
                            if frame.straight {
                                for pixel in frame.rgba.chunks_exact_mut(4) {
                                    let alpha = u16::from(pixel[3]);
                                    for channel in &mut pixel[..3] {
                                        *channel =
                                            ((u16::from(*channel) * alpha + 127) / 255) as u8;
                                    }
                                }
                            }
                            // A surface's attach/offset delta positions its origin
                            // relative to the pointer; subsurfaces extend the bbox.
                            let x = self
                                .drag_visual
                                .offset
                                .x
                                .saturating_add(bbox.loc.x)
                                .clamp(-32768, 32768);
                            let y = self
                                .drag_visual
                                .offset
                                .y
                                .saturating_add(bbox.loc.y)
                                .clamp(-32768, 32768);
                            message = json!({"type":"drag_visual","generation":self.drag_visual.generation,
                                "revision":self.drag_visual.revision,"active":true,"visible":true,
                                "format":"rgba8-premultiplied","width":frame.width,"height":frame.height,
                                "scale":scale,"offset_x":x,"offset_y":y,"pixels":B64.encode(&frame.rgba)});
                        }
                        Err(error) => tracing::debug!(%error,"DND icon capture unavailable"),
                    }
                }
                peer.drag_visual(message);
            }
        }
        // Keep animated icons progressing even when the window is static,
        // forwarding is disabled or a malformed/oversized icon is hidden.
        send_frames_surface_tree(
            &surface,
            &output,
            self.start.elapsed(),
            Some(Duration::ZERO),
            |_, _| Some(output.clone()),
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn dpi_capture_stays_bounded() {
        for (w, h) in [(512, 128), (256, 256), (128, 128), (16, 512), (191, 341)] {
            let scale = capture_scale((w, h).into(), 4.0);
            assert!((f64::from(w) * scale).round() <= 512.0);
            assert!((f64::from(h) * scale).round() <= 512.0);
            assert!((f64::from(w) * scale).round() * (f64::from(h) * scale).round() <= 65536.0);
        }
    }
}
