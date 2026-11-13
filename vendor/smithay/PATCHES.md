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

8. `ClientDndGrabHandler::send_drop_performed` defaults to the existing accepted-offer behavior. An opted-in xdg-toplevel drag can additionally finish its window move on button release outside all accepted data targets, sending drop_performed before cancelled. Explicit Escape/disconnect cancellation retains cancellation-only behavior. This matches the distinction between ending a move and cancelling a drag; ordinary file drags are unchanged.

The X11/Qt and native Windows regressions in the parent project exercise the X11 patches; the HDR branch exercises the format-cache transition. The two hooks above add default-compatible public API; no private-layout access is used. `x11_pointer_smoke.py` checks real RandR monitor stability and GLFW input; `chrome_drag_smoke.py` and `windows_chrome_smoke.py` exercise actual Chrome detach, reattach and cancellation.
