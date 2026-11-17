# Professional input and game devices

The first implemented slice is experimental native multitouch for Wayland
applications which consume `wl_touch`. Gamepads/rumble, pen/tablet input,
application-specific gesture mapping and X11 native touch remain follow-up work.
This does not mark M3 complete.

## Control-center setting

In the control center's input/sharing settings, enable **原生多点触控（实验）**.
The choice is saved per connection as the boolean `touch`, defaulting to false
for a new profile. **保存设置** persists it for subsequent connections;
**应用到当前连接** also changes the active native worker and Linux session.
Touch remains subject to the existing read-only input setting.

Turning it off immediately cancels local pending/held contacts, stops native
WM_POINTER collection and touch-mouse suppression, and asks the server to cancel
its contacts. Turning it on obtains a fresh server lease; earlier contacts and
queued taps cannot resume. A per-application mouse/gesture compatibility UI is
still follow-up work.

For a directly launched native viewer, add the explicit final option:

```powershell
.\waybridge-client.exe <host> <port> <token> --touch
```

Omitting `--touch` leaves native touch disabled. The normal authenticated TLS/SSH
transport requirements still apply. With an older server which has native-touch
capability but lacks live-preference support, a changed live setting is saved
and produces a reconnect/upgrade warning; the active setting stays unchanged.
A server without native-touch capability requires an upgrade before use.

## Current behavior

- The Windows controller consumes `PT_TOUCH` WM_POINTER down/update/up and capture
  loss. Physical client coordinates are transformed through the actual image
  rectangle into Linux logical coordinates, with the same crop-origin correction
  as ordinary pointer input. This preserves fractional display scaling and
  letterboxing without multiplying logical positions by DPI twice.
- Up to ten contacts have independent bounded slots. Each wire batch ends in one
  Wayland touch frame. Smithay retains the surface selected by the first contact
  for the gesture; motion outside its initial input region does not become a
  different target. Native gesture recognition is performed by the Linux app.
- A server-issued generation binds touch to the focused, mapped, visible,
  input-allowed native Wayland root. New contacts use its surface-tree input hit
  test. X11 windows retain the existing pointer compatibility path. Menus, native
  OLE/window drags and relative-pointer locks do not start a native touch gesture.
- Focus loss, window hide/unmap/destruction, explicit input cancel, read-only,
  pointer-lock/drag ownership and disconnect cancel the whole gesture. Old
  generations cannot update a later focus or a remapped window. Updates/up after
  local cancellation cannot recreate contacts; a fresh down is required.
- The initial down may race focus negotiation. The native adapter buffers at
  most 64 events for 500 ms, including a complete quick tap, and flushes only for
  the matching current window/focus lease. A disabled transition preserves that
  first gesture only when the server explicitly names its exact `pending_target`;
  unmarked revocation, another target, read-only and explicit cancellation purge
  pending contacts/events, so later regrant cannot replay an old tap.
  Expired/overloaded pending input is discarded. A long or unavailable connection is not guaranteed to deliver that
  first tap.
- Only mouse messages carrying Windows' documented *touch* promotion signature
  are suppressed after a consumed touch, avoiding duplicate primary clicks.
  Physical mouse and pen messages retain their existing behavior. Pen pressure,
  tilt and eraser fields are not converted into native touch.

An application which does not bind `wl_touch` is not automatically adapted to
mouse input in native-touch mode. Use the default compatibility path for those
applications until per-application mapping is implemented. Touchpad gestures are
not claimed as touchscreen contacts. Contact area/orientation, pressure and
hardware history/coalescing are not included in this slice.

## Protocol extension

The v3 hello negotiates immutable `touch_v1` capability independently of the
initial boolean `touch_enabled` preference. New native workers always advertise
the capability and send their saved preference explicitly. For compatibility,
a legacy viewer which requested `touch_v1` but omitted `touch_enabled` retains
its previously enabled behavior; viewers without capability cannot enable touch
through a later hello or live toggle. The server advertises `touch_settings_v1`
for live preference support. A negotiated viewer can send
`{"type":"touch_enabled","enabled":true}` (or false) without reconnecting.
Viewers without `touch_v1` do not receive active touch leases. Native Wayland
window metadata includes `touch_input:true`;
X11 metadata reports false.

The server sends:

```json
{"type":"touch_state","id":1,"generation":7,"enabled":true,"requested":true,"max_contacts":10}
```

The client sends bounded, atomically validated contact batches:

```json
{"type":"touch","id":1,"generation":7,"origin_x":0,"origin_y":0,
 "points":[{"slot":0,"phase":"down","x":100.25,"y":80.75},
           {"slot":1,"phase":"down","x":200.5,"y":160.25}]}
```

`phase` is `down`, `motion` or `up`; slots are 0–9 and may occur only once in a
batch. A malformed batch is rejected before any of its events are dispatched.
The entire gesture can be canceled with
`{"type":"touch","id":1,"generation":7,"action":"cancel"}`.
Cancellation and lease changes invalidate the old generation. During a normal
focus acquisition, an intermediate disabled state may contain `pending_target`
with the intended new window ID; this single-use handoff preserves only that
window's as-yet-unsent first gesture. A disabled state without this marker (or
with zero/another ID) is a policy revocation and clears pending input.

## Verification

```sh
cargo build --locked
python3 tests/session_fixture.py python3 tests/touch_smoke.py
python3 tests/windows_touch_smoke.py
python3 tests/session_fixture.py python3 tests/touch_settings_smoke.py
cmake --build build/windows --target waybridge-client
scripts/build-windows-arm64.sh
dotnet run --project gui/Waybridge.Control.Tests -c Release
# Publish the production-source settings-only probe before running on a test PC:
dotnet publish gui/Waybridge.GuiFocus.Probe/Waybridge.GuiFocus.Probe.csproj -c Release \
  -r win-x64 --self-contained true -p:WindowsAppSDKSelfContained=true \
  -p:MicrosoftWindowsAppSDKPackageDir= \
  "-p:ManifestTool=$PWD/scripts/merge-windows-manifest.py" -o build/gui-settings-probe-x64
python3 tests/windows_touch_settings_laptop.py --host laptop
```

The live-preference tests exercise an initially disabled negotiated viewer,
actual contact cancellation on disable, fresh generations on reenable, immutable
capability gating and the legacy missing-preference default. The hosted-settings
regression runs the production PE worker and private host-control pipe against
real Linux sessions, checking startup OFF, live ON/OFF, server preference/status
feedback and boolean validation. A local relay removes only the new server's
live-setting capability to verify the old-server warning and absence of an
unsupported toggle; it does not change system networking.

The protocol fixture exposed and fixes a vendored Smithay cancellation bug:
contacts whose last frame was already delivered were skipped by cancel. The
generic touch implementation now cancels active focus regardless of pending
frame state and clears retired slots; the protocol regression covers this case.

The protocol fixture uses actual independent Wayland surfaces and real
`wl_touch` resources, not mocked event callbacks. The standalone PE probe runs the
production native adapter under a disposable Wine/Xvfb desktop and provides
controlled pointer samples. It verifies the adapter's contact, geometry,
queueing and lease policy; it does not exercise real touchscreen WM_POINTER
acquisition or certify physical x64/ARM64 hardware. Both Windows worker
architectures compile. Physical touchscreen, multi-monitor/display changes and
broader Qt/GTK application behavior remain device/application checks.

The native adapter follows Microsoft's
[WM_POINTER capture-loss rules](https://learn.microsoft.com/en-us/windows/win32/inputmsg/wm-pointercapturechanged),
[pointer cancellation flags](https://learn.microsoft.com/en-us/windows/win32/inputmsg/pointer-flags-contants)
and [touch-promoted mouse signature](https://learn.microsoft.com/en-us/windows/win32/tablet/system-events-and-mouse-messages).
