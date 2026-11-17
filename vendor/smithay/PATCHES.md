# Waybridge local patches to Smithay 0.7.0

Source: the published crates.io smithay 0.7.0 crate, copied without modifying the Cargo registry. Upstream: https://github.com/Smithay/smithay . Original MIT license is retained in LICENSE.txt.

Modified areas are Xwayland, GLES texture caching, Wayland output visibility and data-device drag completion:

1. CreateNotify converts client coordinates through f64 rather than truncating a fractional client scale to i32. This avoids double scaling initial 175% X11 windows.
2. Reparenting frames use the original client depth/visual and the original client colormap, transparent background and border. A 24-bit frame irreversibly flattens an ARGB32 child onto black before its Wayland buffer reaches the compositor. Matching the visual preserves alpha and the existing GPU capture path. The frame borrows the client colormap without allocating/freeing one: XFreeColormap would set referring windows to None, so immediate freeing is incorrect.

3. `new_selection` and `set_xsettings` flush their output immediately. Otherwise a newly attached application can observe old clipboard ownership or DPI until an unrelated XWM request happens to flush the connection.

4. Never unlink an already existing filesystem socket during bind, and only clean sockets actually created by this lock. An X server started with `-displayfd` may have no X lock file, so absence of a lock alone is insufficient evidence that a socket is stale.

5. Custom selection MIME atoms are resolved by their advertised atom name, without querying a nonexistent/destructively read TARGETS property. This enables PNG and URI selections.

6. SHM texture-cache reuse compares internal format and alpha interpretation as well as dimensions. Same-size RGBA16F/ARGB8888 transitions otherwise upload into an incompatible cached texture; the HDR fixture exercises this transition.

7. `OutputHandler::can_view_output` is a default-true global filter. Waybridge uses it to hide native Wayland per-window outputs from its Xwayland client. All X11 windows instead share one persistent output, so resizing/closing a game cannot change the Java/GLFW session's monitor topology.

8. The old data-device client/server grab hooks have been superseded by the
   generic `Source` / `DndFocus` / `OfferData` backport described below. The
   attached-toplevel normal-release exception is retained as the default-false
   `Source::allow_unaccepted_drop`; explicit cancellation never performs a drop.

9. EWMH `_NET_ACTIVE_WINDOW` ClientMessage is forwarded through a default-no-op
   `XwmHandler::activation_request`. The compositor applies its user-intent policy;
   arbitrary X11 source indications/timestamps are not treated as foreground
   permission. Existing consumers retain the previous behavior unless they opt in.

The X11/Qt and native Windows regressions in the parent project exercise the X11 patches; the HDR branch exercises the format-cache transition. The two hooks above add default-compatible public API; no private-layout access is used. `x11_pointer_smoke.py` checks real RandR monitor stability and GLFW input; `chrome_drag_smoke.py` and `windows_chrome_smoke.py` exercise actual Chrome detach, reattach and cancellation.

10. Selectively backport upstream generic DND (`2482320`) and its related
    data-device/XWM selection implementation from Smithay commit
    `cdba06d56f27036badac44b791c04da153407cc0` (before the Rust-2024/input API
    migration). Source: https://github.com/Smithay/smithay/tree/cdba06d56f27036badac44b791c04da153407cc0 .
    Files: `input/dnd/{mod,grab}.rs`, `wayland/selection/data_device/{mod,device,source}.rs`,
    `xwayland/xwm/{mod,surface,dnd,selection}.rs`, and the input module export.
    The old two grab modules are removed; the X11 WaylandFocus implementation
    moves from desktop/space to the surface module. Unrelated newer keyboard,
    pending-enter, Dispatch2, InputTime and touch API changes are not included.
    This intentionally changes the vendored DND handler API; it is not a claim
    of a source-compatible Smithay version upgrade. Existing Waybridge output,
    socket, ARGB reparenting, icon, activation, selection-flush and EGL patches
    are retained and checked by their regressions.

11. Local integration keeps MIME acceptance/nil reset, pending versus rejected
    negotiation and source-generation ownership. Inactive/finished offers cannot
    change source feedback or finish a newer gesture. Dropped active offers may
    still receive their own data. Pre-v3 implicit COPY and the Chrome toplevel
    release distinction are preserved. Generic callbacks supply feedback without
    re-entering compositor pointer locks.

12. Independent native windows require proxying X11→X11 as well as mixed backend
    drags. Keep original XdndSelection ownership, server timestamps and a gesture
    shadow XID; allocate independent child requestors for concurrent format reads.
    Foreign-source target offers use fresh source XIDs. Validate reply recipients,
    target identity, final v5 action/success and original-source messages. Keep
    requested versus accepted action separate; propagate action-list support,
    immediate Status updates and latest coalesced positions. Preserve an early
    terminal result until the original X11 Drop can receive Finished. Stalled
    conversions/terminal shadows expire after ten seconds; conversions are capped
    at eight and type lists at 64. These limits do not add socket deadlines to
    every synchronous XWM property query.

13. X11 pointer enter also forwards its current position before a following button,
    preventing an independent remote window from using the previous X sprite
    location. The compositor excludes empty-input visuals from popup capture and
    defers capture changes during native DND. Empty X SHAPE regions are cached
    and updated through ShapeNotify, so untyped transparent drag windows also
    export empty input metadata; arbitrary nonempty SHAPE composition is outside
    this patch. Real Qt Wayland/XCB, delayed-offer,
    native Windows/Wine and physical Windows regressions validate these paths;
    detailed current limits are in the parent `docs/XDND.md`.

14. Touch cancellation clears all active slot focus even after the latest
    `wl_touch.frame`. The previous pending-frame guard incorrectly skipped every
    contact when cancellation followed a delivered frame, leaving clients with
    held touches after compositor focus/policy/disconnect changes. Pending up
    focus is canceled as well and stale slots are removed. The existing
    per-resource sequence filter deduplicates cancellation across contacts on
    the same client. The parent project's real `tests/touch_smoke.py` verifies
    two/ten contacts and cancellation on focus, read-only, unmap, disconnect and
    surface destruction; fresh contacts remain usable afterwards.

15. Native touch DND sets `should_drop` only when the original grabbing slot is
    released. The generic touch grab previously always ran its cancellation
    path, even for an accepted target; secondary contacts still cannot finish
    the drag. Touch cancellation remains a cancellation, without a synthetic up.
    The parent project's real Wayland touch interaction regression exercises a
    data-device drag, target negotiation, payload receive and source completion.

16. `TouchHandle::client_touches` publicly exposes live `wl_touch` resources for
    one client. Waybridge uses this protocol subscription to select native touch
    automatically and updates metadata when the client binds/releases touch.
    This query does not claim a per-widget event-handled acknowledgement, which
    the Wayland touch protocol does not provide.

17. `TabletSeatHandle::client_tablet_seats` exposes live tablet-v2 seat resources
    for one client. Waybridge uses actual subscription to select native pen
    delivery and updates leases/metadata after bind or release. This reports
    protocol subscription rather than a per-widget event-handled result. The
    parent project's real `tests/pen_smoke.py` covers early device announcement,
    late bind/release and policy cancellation through actual tablet-v2 objects.
