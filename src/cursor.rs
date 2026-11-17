//! Seat-local cursor state. Bitmaps never enter the video queue.
use super::*;
use smithay::{
    desktop::utils::send_frames_surface_tree,
    input::pointer::{CursorImageStatus, CursorImageSurfaceData},
};

pub(crate) const MAX_CURSOR_EDGE: i32 = 256;
const INTERVAL: Duration = Duration::from_millis(33);

pub(crate) struct Cursor {
    pub image: CursorImageStatus,
    pub dirty: bool,
    generation: u64,
    last_root: u32,
    last_tick: Instant,
    target: Option<capture::Target>,
    target_size: (i32, i32),
}
impl Default for Cursor {
    fn default() -> Self {
        Self {
            image: CursorImageStatus::default_named(),
            dirty: true,
            generation: 0,
            last_root: 0,
            last_tick: Instant::now() - INTERVAL,
            target: None,
            target_size: (0, 0),
        }
    }
}

// Include the hotspot in the bitmap, including negative/outside surface hotspots.
// Use wide arithmetic before bounding so no client-controlled allocation is unbounded.
fn bounds(
    bbox: Rectangle<i32, Logical>,
    hotspot: Point<i32, Logical>,
    scale: f64,
) -> Option<Rectangle<i32, Logical>> {
    let x = i64::from(bbox.loc.x).min(i64::from(hotspot.x));
    let y = i64::from(bbox.loc.y).min(i64::from(hotspot.y));
    let right = (i64::from(bbox.loc.x) + i64::from(bbox.size.w)).max(i64::from(hotspot.x) + 1);
    let bottom = (i64::from(bbox.loc.y) + i64::from(bbox.size.h)).max(i64::from(hotspot.y) + 1);
    let width = right - x;
    let height = bottom - y;
    if bbox.size.w <= 0
        || bbox.size.h <= 0
        || !(1.0..=4.0).contains(&scale)
        || width <= 0
        || height <= 0
        || (width as f64 * scale).round() > f64::from(MAX_CURSOR_EDGE)
        || (height as f64 * scale).round() > f64::from(MAX_CURSOR_EDGE)
    {
        return None;
    }
    Some(Rectangle::new(
        (i32::try_from(x).ok()?, i32::try_from(y).ok()?).into(),
        (i32::try_from(width).ok()?, i32::try_from(height).ok()?).into(),
    ))
}

// Bound source imports too: a tiny viewport over a huge buffer must not bypass
// the output allocation limit. At most 16 surfaces and 256 KiB of source RGBA.
fn source_bounded(surface: &WlSurface) -> bool {
    use smithay::backend::renderer::{buffer_dimensions, utils::RendererSurfaceStateUserData};
    let mut count = 0usize;
    let mut pixels = 0i64;
    let mut valid = true;
    with_surface_tree_downward(
        surface,
        (),
        |_, _, _| TraversalAction::DoChildren(()),
        |_, states, _| {
            count += 1;
            if count > 16 {
                valid = false;
                return;
            }
            if let Some(data) = states.data_map.get::<RendererSurfaceStateUserData>() {
                if let Some(size) = data
                    .lock()
                    .unwrap()
                    .buffer()
                    .and_then(|b| buffer_dimensions(b))
                {
                    pixels = pixels.saturating_add(i64::from(size.w) * i64::from(size.h));
                    valid &= size.w > 0
                        && size.h > 0
                        && size.w <= MAX_CURSOR_EDGE
                        && size.h <= MAX_CURSOR_EDGE
                        && pixels <= 65_536;
                }
            }
        },
        |_, _, _| true,
    );
    valid
}

impl App {
    pub(crate) fn render_cursor(&mut self) {
        if matches!(&self.cursor.image, CursorImageStatus::Surface(surface) if !surface.alive()) {
            self.cursor.image = CursorImageStatus::default_named();
            self.cursor.dirty = true;
        }
        let surface = match &self.cursor.image {
            CursorImageStatus::Surface(surface) if surface.alive() => Some(surface.clone()),
            _ => None,
        };
        if surface.is_some() && self.cursor.last_tick.elapsed() < INTERVAL {
            return;
        }
        self.cursor.last_tick = Instant::now();
        // Pointer focus is independent of keyboard activation and release_input.
        let focus = self
            .seat
            .get_pointer()
            .unwrap()
            .current_focus()
            .and_then(|s| s.wl_surface().map(|s| s.into_owned()));
        let root = focus.as_ref().map(|s| self.root_for(s));
        let id = self
            .windows
            .iter()
            .find(|w| w.window.wl_surface().as_deref() == root.as_ref())
            .map_or(0, |w| w.id);
        if id != self.cursor.last_root {
            self.cursor.last_root = id;
            self.cursor.dirty = true;
        }
        let (scale, output) = self
            .windows
            .iter()
            .find(|w| w.id == id)
            .map(|w| (w.scale, w.output.clone()))
            .unwrap_or((self.default_scale, self.output.clone()));
        if let Some(surface) = &surface {
            set_surface_scale(surface, &output, scale);
        }
        if self.cursor.dirty {
            self.cursor.dirty = false;
            if let Some(peer) = self.peer.clone().filter(|p| p.cursor_enabled()) {
                self.cursor.generation += 1;
                let mut message = json!({"type":"cursor","id":id,"generation":self.cursor.generation,
                    "kind":"named","name":"default"});
                if id != 0 {
                    match &self.cursor.image {
                        CursorImageStatus::Hidden => message["kind"] = json!("hidden"),
                        CursorImageStatus::Named(name) => message["name"] = json!(name.to_string()),
                        CursorImageStatus::Surface(_) => {
                            if let Some(surface) = &surface {
                                let hotspot = with_states(surface, |s| {
                                    s.data_map
                                        .get::<CursorImageSurfaceData>()
                                        .map(|s| s.lock().unwrap().hotspot)
                                })
                                .unwrap_or_default();
                                let bbox = bbox_from_surface_tree(surface, (0, 0));
                                if bbox.size.w <= 0 || bbox.size.h <= 0 {
                                    message["kind"] = json!("hidden");
                                } else if let Some(bbox) = source_bounded(surface)
                                    .then(|| bounds(bbox, hotspot, scale))
                                    .flatten()
                                {
                                    match capture::surface(
                                        &mut self.renderer,
                                        surface,
                                        id,
                                        0,
                                        bbox,
                                        scale,
                                        &mut self.cursor.target,
                                        &mut self.cursor.target_size,
                                        peer.clone(),
                                        None,
                                        &mut None,
                                        self.color_programs.as_ref(),
                                        None,
                                        self.encoder.budget(),
                                        None,
                                    ) {
                                        Ok(Some(mut frame)) => {
                                            if frame.straight {
                                                for pixel in frame.rgba.chunks_exact_mut(4) {
                                                    let alpha = u16::from(pixel[3]);
                                                    for channel in &mut pixel[..3] {
                                                        *channel = ((u16::from(*channel) * alpha
                                                            + 127)
                                                            / 255)
                                                            as u8;
                                                    }
                                                }
                                            }
                                            let hotspot = (hotspot - bbox.loc)
                                                .to_physical_precise_round::<f64, i32>(scale);
                                            message = json!({"type":"cursor","id":id,"generation":self.cursor.generation,
                                                "kind":"bitmap","format":"rgba8-premultiplied",
                                                "width":frame.width,"height":frame.height,"scale":scale,
                                                "hotspot_x":hotspot.x.clamp(0, frame.width as i32 - 1),
                                                "hotspot_y":hotspot.y.clamp(0, frame.height as i32 - 1),
                                                "pixels":B64.encode(&frame.rgba)});
                                        }
                                        Ok(None) => {
                                            self.cursor.dirty = true;
                                        }
                                        Err(error) => {
                                            tracing::warn!(%error, "cursor capture failed; using default")
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                peer.cursor(message);
            }
        }
        if let Some(surface) = &surface {
            // Animated wl_pointer surfaces rely on callbacks even if the app window is static,
            // the viewer is disconnected, or an oversized cursor falls back to the arrow.
            send_frames_surface_tree(
                surface,
                &output,
                self.start.elapsed(),
                Some(Duration::ZERO),
                |_, _| Some(output.clone()),
            );
        } else {
            self.cursor.target = None;
            self.cursor.target_size = (0, 0);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn hotspot_padding_and_dpi_are_bounded() {
        let bbox = Rectangle::from_size((24, 16).into());
        assert_eq!(
            bounds(bbox, (-3, 20).into(), 2.0).unwrap(),
            Rectangle::new((-3, 0).into(), (27, 21).into())
        );
        assert!(bounds(bbox, (i32::MAX, 0).into(), 1.0).is_none());
        assert!(bounds(Rectangle::from_size((65, 64).into()), (0, 0).into(), 4.0).is_none());
        assert!(bounds(bbox, (0, 0).into(), f64::NAN).is_none());
    }
}
