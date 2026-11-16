//! One focus target routes ordinary input and DND to the native backend.
use crate::x11::InputFocus;
use crate::*;
use smithay::input::{
    dnd::{DndFocus, OfferData, Source},
    pointer::*,
    touch::{
        DownEvent, MotionEvent as TouchMotionEvent, OrientationEvent, ShapeEvent, TouchTarget,
        UpEvent,
    },
};
use smithay::wayland::selection::data_device::WlOfferData;
use smithay::xwayland::xwm::XwmOfferData;
impl PointerTarget<App> for InputFocus {
    fn enter(&self, seat: &Seat<App>, data: &mut App, event: &MotionEvent) {
        match self {
            Self::Wayland(s) => PointerTarget::enter(s, seat, data, event),
            Self::X11(s) => PointerTarget::enter(s.as_ref(), seat, data, event),
        }
    }
    fn motion(&self, seat: &Seat<App>, data: &mut App, event: &MotionEvent) {
        match self {
            Self::Wayland(s) => PointerTarget::motion(s, seat, data, event),
            Self::X11(s) => PointerTarget::motion(s.as_ref(), seat, data, event),
        }
    }
    fn relative_motion(&self, seat: &Seat<App>, data: &mut App, event: &RelativeMotionEvent) {
        match self {
            Self::Wayland(s) => PointerTarget::relative_motion(s, seat, data, event),
            Self::X11(s) => PointerTarget::relative_motion(s.as_ref(), seat, data, event),
        }
    }
    fn button(&self, seat: &Seat<App>, data: &mut App, event: &ButtonEvent) {
        match self {
            Self::Wayland(s) => PointerTarget::button(s, seat, data, event),
            Self::X11(s) => PointerTarget::button(s.as_ref(), seat, data, event),
        }
    }
    fn axis(&self, seat: &Seat<App>, data: &mut App, frame: AxisFrame) {
        match self {
            Self::Wayland(s) => PointerTarget::axis(s, seat, data, frame),
            Self::X11(s) => PointerTarget::axis(s.as_ref(), seat, data, frame),
        }
    }
    fn frame(&self, seat: &Seat<App>, data: &mut App) {
        match self {
            Self::Wayland(s) => PointerTarget::frame(s, seat, data),
            Self::X11(s) => PointerTarget::frame(s.as_ref(), seat, data),
        }
    }
    fn leave(&self, seat: &Seat<App>, data: &mut App, serial: Serial, time: u32) {
        match self {
            Self::Wayland(s) => PointerTarget::leave(s, seat, data, serial, time),
            Self::X11(s) => PointerTarget::leave(s.as_ref(), seat, data, serial, time),
        }
    }
    fn gesture_swipe_begin(
        &self,
        seat: &Seat<App>,
        data: &mut App,
        event: &GestureSwipeBeginEvent,
    ) {
        match self {
            Self::Wayland(s) => PointerTarget::gesture_swipe_begin(s, seat, data, event),
            Self::X11(s) => PointerTarget::gesture_swipe_begin(s.as_ref(), seat, data, event),
        }
    }
    fn gesture_swipe_update(
        &self,
        seat: &Seat<App>,
        data: &mut App,
        event: &GestureSwipeUpdateEvent,
    ) {
        match self {
            Self::Wayland(s) => PointerTarget::gesture_swipe_update(s, seat, data, event),
            Self::X11(s) => PointerTarget::gesture_swipe_update(s.as_ref(), seat, data, event),
        }
    }
    fn gesture_swipe_end(&self, seat: &Seat<App>, data: &mut App, event: &GestureSwipeEndEvent) {
        match self {
            Self::Wayland(s) => PointerTarget::gesture_swipe_end(s, seat, data, event),
            Self::X11(s) => PointerTarget::gesture_swipe_end(s.as_ref(), seat, data, event),
        }
    }
    fn gesture_pinch_begin(
        &self,
        seat: &Seat<App>,
        data: &mut App,
        event: &GesturePinchBeginEvent,
    ) {
        match self {
            Self::Wayland(s) => PointerTarget::gesture_pinch_begin(s, seat, data, event),
            Self::X11(s) => PointerTarget::gesture_pinch_begin(s.as_ref(), seat, data, event),
        }
    }
    fn gesture_pinch_update(
        &self,
        seat: &Seat<App>,
        data: &mut App,
        event: &GesturePinchUpdateEvent,
    ) {
        match self {
            Self::Wayland(s) => PointerTarget::gesture_pinch_update(s, seat, data, event),
            Self::X11(s) => PointerTarget::gesture_pinch_update(s.as_ref(), seat, data, event),
        }
    }
    fn gesture_pinch_end(&self, seat: &Seat<App>, data: &mut App, event: &GesturePinchEndEvent) {
        match self {
            Self::Wayland(s) => PointerTarget::gesture_pinch_end(s, seat, data, event),
            Self::X11(s) => PointerTarget::gesture_pinch_end(s.as_ref(), seat, data, event),
        }
    }
    fn gesture_hold_begin(&self, seat: &Seat<App>, data: &mut App, event: &GestureHoldBeginEvent) {
        match self {
            Self::Wayland(s) => PointerTarget::gesture_hold_begin(s, seat, data, event),
            Self::X11(s) => PointerTarget::gesture_hold_begin(s.as_ref(), seat, data, event),
        }
    }
    fn gesture_hold_end(&self, seat: &Seat<App>, data: &mut App, event: &GestureHoldEndEvent) {
        match self {
            Self::Wayland(s) => PointerTarget::gesture_hold_end(s, seat, data, event),
            Self::X11(s) => PointerTarget::gesture_hold_end(s.as_ref(), seat, data, event),
        }
    }
}
impl TouchTarget<App> for InputFocus {
    fn down(&self, seat: &Seat<App>, data: &mut App, event: &DownEvent, seq: Serial) {
        match self {
            Self::Wayland(s) => TouchTarget::down(s, seat, data, event, seq),
            Self::X11(s) => TouchTarget::down(s.as_ref(), seat, data, event, seq),
        }
    }
    fn up(&self, seat: &Seat<App>, data: &mut App, event: &UpEvent, seq: Serial) {
        match self {
            Self::Wayland(s) => TouchTarget::up(s, seat, data, event, seq),
            Self::X11(s) => TouchTarget::up(s.as_ref(), seat, data, event, seq),
        }
    }
    fn motion(&self, seat: &Seat<App>, data: &mut App, event: &TouchMotionEvent, seq: Serial) {
        match self {
            Self::Wayland(s) => TouchTarget::motion(s, seat, data, event, seq),
            Self::X11(s) => TouchTarget::motion(s.as_ref(), seat, data, event, seq),
        }
    }
    fn frame(&self, seat: &Seat<App>, data: &mut App, seq: Serial) {
        match self {
            Self::Wayland(s) => TouchTarget::frame(s, seat, data, seq),
            Self::X11(s) => TouchTarget::frame(s.as_ref(), seat, data, seq),
        }
    }
    fn cancel(&self, seat: &Seat<App>, data: &mut App, seq: Serial) {
        match self {
            Self::Wayland(s) => TouchTarget::cancel(s, seat, data, seq),
            Self::X11(s) => TouchTarget::cancel(s.as_ref(), seat, data, seq),
        }
    }
    fn shape(&self, seat: &Seat<App>, data: &mut App, event: &ShapeEvent, seq: Serial) {
        match self {
            Self::Wayland(s) => TouchTarget::shape(s, seat, data, event, seq),
            Self::X11(s) => TouchTarget::shape(s.as_ref(), seat, data, event, seq),
        }
    }
    fn orientation(&self, seat: &Seat<App>, data: &mut App, event: &OrientationEvent, seq: Serial) {
        match self {
            Self::Wayland(s) => TouchTarget::orientation(s, seat, data, event, seq),
            Self::X11(s) => TouchTarget::orientation(s.as_ref(), seat, data, event, seq),
        }
    }
}
pub enum FocusOffer<S: Source> {
    Wayland(WlOfferData<S>),
    X11(XwmOfferData<S>),
}
impl<S: Source> OfferData for FocusOffer<S> {
    fn disable(&self) {
        match self {
            Self::Wayland(o) => o.disable(),
            Self::X11(o) => o.disable(),
        }
    }
    fn drop(&self) {
        match self {
            Self::Wayland(o) => o.drop(),
            Self::X11(o) => o.drop(),
        }
    }
    fn validated(&self) -> bool {
        match self {
            Self::Wayland(o) => o.validated(),
            Self::X11(o) => o.validated(),
        }
    }
}
impl DndFocus<App> for InputFocus {
    type OfferData<S: Source> = FocusOffer<S>;
    fn enter<S: Source>(
        &self,
        data: &mut App,
        dh: &DisplayHandle,
        source: Arc<S>,
        seat: &Seat<App>,
        location: Point<f64, Logical>,
        serial: &Serial,
    ) -> Option<FocusOffer<S>> {
        match self {
            Self::Wayland(s) => DndFocus::enter(s, data, dh, source, seat, location, serial)
                .map(FocusOffer::Wayland),
            Self::X11(s) => {
                let s = s.as_ref();
                DndFocus::enter(s, data, dh, source, seat, location, serial).map(FocusOffer::X11)
            }
        }
    }
    fn motion<S: Source>(
        &self,
        data: &mut App,
        offer: Option<&mut FocusOffer<S>>,
        seat: &Seat<App>,
        location: Point<f64, Logical>,
        time: u32,
    ) {
        match self {
            Self::Wayland(s) => {
                let o = match offer {
                    Some(FocusOffer::Wayland(o)) => Some(o),
                    _ => None,
                };
                DndFocus::motion(s, data, o, seat, location, time)
            }
            Self::X11(s) => {
                let s = s.as_ref();
                let o = match offer {
                    Some(FocusOffer::X11(o)) => Some(o),
                    _ => None,
                };
                DndFocus::motion(s, data, o, seat, location, time)
            }
        }
    }
    fn leave<S: Source>(
        &self,
        data: &mut App,
        offer: Option<&mut FocusOffer<S>>,
        seat: &Seat<App>,
    ) {
        match self {
            Self::Wayland(s) => {
                let o = match offer {
                    Some(FocusOffer::Wayland(o)) => Some(o),
                    _ => None,
                };
                DndFocus::leave(s, data, o, seat)
            }
            Self::X11(s) => {
                let s = s.as_ref();
                let o = match offer {
                    Some(FocusOffer::X11(o)) => Some(o),
                    _ => None,
                };
                DndFocus::leave(s, data, o, seat)
            }
        }
    }
    fn drop<S: Source>(&self, data: &mut App, offer: Option<&mut FocusOffer<S>>, seat: &Seat<App>) {
        match self {
            Self::Wayland(s) => {
                let o = match offer {
                    Some(FocusOffer::Wayland(o)) => Some(o),
                    _ => None,
                };
                DndFocus::drop(s, data, o, seat)
            }
            Self::X11(s) => {
                let s = s.as_ref();
                let o = match offer {
                    Some(FocusOffer::X11(o)) => Some(o),
                    _ => None,
                };
                DndFocus::drop(s, data, o, seat)
            }
        }
    }
}
impl App {
    pub(crate) fn pointer_focus(&self, surface: WlSurface) -> InputFocus {
        self.windows
            .iter()
            .find_map(|w| {
                w.window
                    .x11_surface()
                    .filter(|x| x.wl_surface().as_ref() == Some(&surface))
                    .cloned()
            })
            .map_or(InputFocus::Wayland(surface), |s| {
                InputFocus::X11(Arc::new(s))
            })
    }
}
