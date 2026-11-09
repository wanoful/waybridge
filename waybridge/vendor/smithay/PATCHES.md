# Waybridge local patches to Smithay 0.7.0

Source: the published crates.io smithay 0.7.0 crate, copied without modifying the Cargo registry. Upstream: https://github.com/Smithay/smithay . Original MIT license is retained in LICENSE.txt.

Modified files are `src/xwayland/xwm/mod.rs`, `src/xwayland/x11_sockets.rs` and `src/backend/renderer/gles/mod.rs`:

1. CreateNotify converts client coordinates through f64 rather than truncating a fractional client scale to i32. This avoids double scaling initial 175% X11 windows.
2. Reparenting frames use the original client depth/visual and an associated colormap, transparent background and border. A 24-bit frame irreversibly flattens an ARGB32 child onto black before its Wayland buffer reaches the compositor. Matching the visual preserves alpha and the existing GPU capture path. Colormap XID is freed after the frame takes its reference.

3. `new_selection` and `set_xsettings` flush their output immediately. Otherwise a newly attached application can observe old clipboard ownership or DPI until an unrelated XWM request happens to flush the connection.

4. Never unlink an already existing filesystem socket during bind, and only clean sockets actually created by this lock. An X server started with `-displayfd` may have no X lock file, so absence of a lock alone is insufficient evidence that a socket is stale.

5. Custom selection MIME atoms are resolved by their advertised atom name, without querying a nonexistent/destructively read TARGETS property. This enables PNG and URI selections.

6. SHM texture-cache reuse compares internal format and alpha interpretation as well as dimensions. Same-size RGBA16F/ARGB8888 transitions otherwise upload into an incompatible cached texture; the HDR fixture exercises this transition.

The X11/Qt and native Windows regressions in the parent project exercise the X11 patches; the HDR branch exercises the format-cache transition. No new Smithay public API or private-layout access is used.
