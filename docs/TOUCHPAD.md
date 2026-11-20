# Windows Precision Touchpad

On a negotiated connection, the Windows viewer can forward native two-finger
pan and pinch from a Precision Touchpad. The foreground streamed content
window accepts the gesture through Windows
[Direct Manipulation](https://learn.microsoft.com/en-us/windows/win32/directmanipulation/directmanipulation-processing-input-with-directmanipulation).
Two-finger pan delivers smooth horizontal and vertical Wayland finger-source
scroll with an explicit stop. Pinch delivers a two-finger scale gesture to
applications that use `zwp_pointer_gesture_pinch_v1`.

Ordinary pointer movement, tapping or clicking, physical wheel input, and
three- or four-finger Windows shortcuts retain their usual paths. Older peers,
devices with mouse-style input, and a failed Direct Manipulation initialization
use the normal Windows wheel path. Touchscreen and pen settings are independent.
Input is cancelled on focus loss, read-only, invalidated geometry and
disconnect. Wayland applications control their own scroll inertia and
interpretation of the pinch. Xwayland application gesture support depends on
Xwayland and the receiving app.

The Windows viewer and the running Linux compositor both negotiate
`touchpad_v1`. Installing a new Linux binary takes effect when the next
compositor session starts. A connection to an existing older session can still
receive Windows mouse-style scroll and zoom events.

Run the Wayland fixture with a current checkout:

```sh
cargo build --locked -p waybridge --bin waybridge
python3 tests/session_fixture.py python3 tests/touchpad_smoke.py
```

The Wayland fixture covers finger-source axis and stop, native pinch, and
cancellation on focus loss, read-only and disconnect. It also covers wheel
source and rejects late gestures and attempts to enable capability after the
handshake.

The Windows fixture uses the production Direct Manipulation adapter with its
own window procedure. Replay covers DPI, gesture transitions and cancellation.
On Windows 11 x64 build 26200, the opt-in exercise passed two-finger pan and
pinch through Windows using synthetic Precision Touchpad contacts, including
wheel fallback when native handling was disabled.

The same synthetic contacts also passed through the production x64 viewer to a
real Wayland subscriber on a disposable compositor. The subscriber observed
finger-source scrolling with an axis stop, a two-contact pinch with changing
scale and a normal pinch end. It received no duplicate wheel events. The probe's
`--production-viewer PID TITLE` mode requires a visible foreground window owned
by that process and a title beginning with `Waybridge-touchpad-e2e-`. The Wayland
fixture takes that title from `WAYBRIDGE_TOUCHPAD_FIXTURE_TITLE`.

The shipped x64 viewer also passed a synthetic Precision Touchpad pinch into
native Wayland Chrome. Chrome's visual viewport scale increased from 1 to 2.69
while its layout width and device pixel ratio stayed fixed.

The user confirmed that physical touchpad pinching in streamed Chrome worked
after reconnecting to the upgraded Linux session.
