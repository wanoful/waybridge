//! Shared process shutdown flag for signal-safe worker and compositor teardown.
use std::sync::atomic::{AtomicBool, Ordering};
static REQUESTED: AtomicBool = AtomicBool::new(false);
extern "C" fn request(_: libc::c_int) {
    REQUESTED.store(true, Ordering::Relaxed);
}
pub(crate) fn install() {
    unsafe {
        libc::signal(libc::SIGINT, request as *const () as libc::sighandler_t);
        libc::signal(libc::SIGTERM, request as *const () as libc::sighandler_t);
    }
}
pub(crate) fn requested() -> bool {
    REQUESTED.load(Ordering::Relaxed)
}
