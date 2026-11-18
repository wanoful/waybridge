# Professional input and game devices

Implemented experimental slices include native multitouch for `wl_touch`
applications, [native pen/tablet-v2](PEN.md), and [SDL gamepads with rumble](GAMEPADS.md).
Each is enabled and saved in the control center's input settings. Physical-device
and application coverage, application-specific gesture mapping and X11 native
touch remain follow-up work. This does not mark M3 complete.

## Control-center setting

In the control center's input/sharing settings, enable **原生多点触控（实验）**.
The choice is saved per connection as the boolean `touch`, defaulting to false
for a new profile. **保存设置** persists it for subsequent connections;
**应用到当前连接** also changes the active native worker and Linux session.
Touch remains subject to the existing read-only input setting. When enabled,
mode selection is automatic: a native Wayland application with a live `wl_touch`
resource receives touch, while an unsubscribed or X11 window retains mouse
compatibility. Binding reports protocol availability, not whether an individual
widget handled an event. Binding/releasing resources updates window eligibility.
The optional application selector offers automatic, prefer native, or mouse
compatibility overrides in `touch_modes`; no manual setup is required normally.
Mouse compatibility uses the system's long-press right-click and drag-selection
path. Native long press, selection and drawing remain the application's choice.

Turning it off immediately cancels local pending/held contacts, stops native
WM_POINTER collection and touch-mouse suppression, and asks the server to cancel
its contacts. Turning it on obtains a fresh server lease; earlier contacts and
queued taps cannot resume. Application overrides are saved with the connection and applied through the
same buttons. Changing settings cancels current input before changing modes.

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
  test. X11 windows retain the existing pointer compatibility path. Popup touch is
  routed through its root lease to the concrete popup surface. A validated
  window move/resize or native DND request transfers the physical contact to an
  explicitly identified operation; relative-pointer locks still exclude touch.
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
  Physical mouse messages retain their existing behavior. Pen pressure, tilt and
  eraser fields use the separate optional tablet-v2 adapter, never native touch.

An application which does not bind `wl_touch` automatically retains Windows mouse
compatibility. Optional per-application overrides can address exceptions. Touchpad gestures are
not claimed as touchscreen contacts. Contact area/orientation, pressure and
hardware history/coalescing are not included in this slice.

## Window, popup and drag integration — 2026-11-18

The user reported working finger drawing and a streamed Chrome title bar which
could not move. The implementation now accepts a live touch serial for native
Wayland move/resize, binds the request to its original gesture and owning slot,
and drives the Windows HWND from that contact's physical coordinates. Resize
honors window constraints, including opposite-edge anchoring, and moving a
maximized window restores it. When a self-decorated window confirms a move,
the viewer may ask Windows to promote the held primary contact and take over
its caption move. The viewer enters the system move loop on the genuine
promoted mouse message for that same contact. The loop owns placement and edge
Snap once entered. The viewer keeps the original touch point over the source
HWND until Windows delivers that first mouse message. Contact-based placement remains available when conversion
or loop entry is unavailable. Resize retains its contact-based placement, and
native Windows frames retain their system handling.

Popup HWNDs map to their root and concrete popup surface. Native contact hit
coordinates reach menus and nested popup surfaces; an outside press dismisses
the current popup grab. Menu content, long-press recognition, text ranges and
selection handles remain owned by the remote application. There is no generic
handled-event acknowledgement with which to safely replace an application's
already delivered long press with a mouse right-click.

Native Wayland text and URI/file DND between streamed windows retains the original
source, MIME and COPY/MOVE negotiation. Contact motion, up and cancel drive the
original touch grab across HWNDs without acquiring target keyboard focus. Only
the initiating contact can finish the operation; a second finger, stale request
or old slot reused by a later gesture cannot release it. Changing ownership does
not trigger the ordinary focus policy's blanket touch cancellation.

When an application attaches a toplevel to the drag, Windows can take over its
movement and edge Snap using the original primary contact. The Wayland drag
continues delivering motion and drops so the application can reattach the
toplevel or merge a tab. Touching a different streamed window can focus it and
start a new gesture on the same press.

Cross-platform DND reuses existing text providers, delayed virtual-file streams,
read-only file delivery, target revisions and completion acknowledgements.
Exports, claim, drop and return now accept the bound touch owner rather than
requiring a physical mouse button. Returning retains the original source/grab;
physical release and target completion have separate lifetimes, so asynchronous
file reads do not keep all touch input locked after drop.

For Windows OLE, `client/touch_ole.h` resolves
[ConvertPrimaryPointerToMouseDrag](https://learn.microsoft.com/en-us/windows/win32/winmsg/winuser/nf-winuser-convertprimarypointertomousedrag)
by name, validates one active primary contact and waits for a genuine synthesized
mouse message. OLE entry stays on that mouse-handler stack while a bounded
750 ms STA pump waits for remote readiness. This follows the
[DoDragDrop input contract](https://learn.microsoft.com/en-us/windows/win32/api/ole2/nf-ole2-dodragdrop).
Unsupported/rejected promotion and readiness timeout stop or reject the attempt
with a visible settings warning; mouse compatibility remains available.
The named API is present on the tested Windows build26200, but actual positive
system promotion/OLE tests are **pending**: its lock screen covered the private
fixtures, and ownership guards stopped all touch injection. Presence alone is
not evidence of successful drag-and-drop. The standalone probe includes direct,
delayed-readiness and multiple-contact cases for the next unlocked run.

The implemented default is automatic protocol detection; optional application
settings address exceptional compatibility, not normal setup. Native mode leaves
long press/selection to the app, compatibility mode keeps system right-click and
mouse-style selection. Applications without selection handles are not promised
mobile-style controls. General gesture mapping, X11 native touch and richer
contact samples remain follow-up. Native pen and gamepads/rumble now have separate
experimental implementations described in [PEN.md](PEN.md) and [GAMEPADS.md](GAMEPADS.md).
Mixed-monitor DPI and lock/sleep/device recovery still require physical checks.

### Interaction protocol

A viewer advertises `touch_interaction_v1:true` alongside `touch_v1`. Ordinary
contact packets carry a monotonic client `gesture` plus a concrete `target`
(root or popup), while `id` and `generation` still identify the root lease.
The server sends `touch_interaction` with `operation`, `kind` (move/resize/dnd),
`id`, `generation`, `gesture`, `slot` and optional resize `edge`. The client ACKs
only if that exact physical owner is still held; delayed requests cannot claim
a newer tap. Operation motion/up/cancel use `touch_interaction_input`, validated
by operation/gesture/slot, with current target coordinates for DND. Server
`kind:"end"` retires only the matching operation. Release before ACK, policy
revocation and invalid release targets cancel without dropping into stale focus.
Touch DND offers and toplevel metadata include the original touch-owner identity;
OLE attempts and returns preserve it. Legacy viewers keep their existing paths.

### Focused verification

Real Wayland regressions exercise live subscription/release, xdg move/resize,
owner ACK races and invalid serials, popup coordinates/dismissal, cross-client
URI and UTF-8 DND with actual data/source finish, private sink claim/return,
external drop and late reads after operation retirement. Controlled production
PE adapter tests exercise gesture/slot reuse, popup targets, focus changes,
second-finger isolation, external capture/up and exact completion. Actual HWND
regressions check contact-based movement, edge limits and maximized restoration.
The guarded production-client probe used injected PT_TOUCH on an unlocked x64
Windows desktop. It verified a normal touch, two-contact isolation during local
move, anchored resize, fresh touch after release, and a single-contact CSD move
that snapped to half the work area. Windows reported the window as arranged.
Physical touchscreen input across mixed-DPI monitors and ARM64 remain separate
checks.

```
python3 tests/touch_interaction_smoke.py
python3 tests/windows_touch_smoke.py
# Opt-in unlocked, logged-in Windows test device; exact fixture ownership guards:
python3 tests/windows_touch_client_laptop.py --host laptop
# Requires an interactive desktop with Windows Snap enabled:
python3 tests/windows_touch_client_laptop.py --host laptop --expect-snap
python3 tests/windows_touch_ole_laptop.py --host laptop
```

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
