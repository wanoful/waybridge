# Native pen — experimental

Windows `PT_PEN` input can reach native Wayland applications through tablet-v2.
The implemented slice includes hover/proximity, tip down/up, pressure, tilt,
rotation, barrel button and distinct pen/eraser tools, plus pen-initiated window
move/resize. It does not complete the professional-input milestone.

## Enable and calibrate

In the control center's input settings, enable **压感手写笔（实验）**. Settings
are saved per connection:

| Field | Default | Meaning |
| --- | --- | --- |
| `pen` | `false` | Enable native pen forwarding. |
| `pen_pressure_gamma` | `1` | Pressure curve; finite values from `0.25` to `4`. |

**保存设置** persists the choice; **应用到当前连接** also updates the active
worker/session. Gamma `1` is linear; smaller values strengthen light pressure,
larger values require more force. The curve changes valid pressure samples only.
The low-level viewer also accepts `--pen`; no environment switch is required.
Live enable/disable requires negotiated `pen_settings_v1`; unsupported servers
produce a reconnect/upgrade warning.

## Automatic routing and axes

Native eligibility requires a live tablet-v2 **tablet-seat resource owned by the
application**, plus the focused, mapped, input-allowed Wayland window lease. The
virtual tablet is advertised before first proximity, so applications can discover
it without an initial pen event. Binding/releasing the resource updates window
metadata and eligibility. Unsubscribed applications and X11 windows retain the
existing Windows pen-to-mouse compatibility path. Background-window hover does
not steal focus. Subscription identifies protocol availability, not whether each
individual widget handles pen input.

Only axes marked valid by Windows `PEN_MASK_*`, with values in range, are
forwarded. Pressure is normalized before applying gamma; absent pressure is never
invented. Tablet tool capabilities come from valid observed axes. A pressure-less
tool advertises no Pressure capability and still receives tip events. Newly
observed capabilities are applied at the next proximity, without replacing a
tool during a stroke. Tilt requires valid observations for both coordinates.

## Ownership, windows and cancellation

Generation, gesture, sequence and tool identity bind samples to their original
owner. A held tip retains its surface through release. CSD move/resize requests
must carry the current tip serial and match its seat, client and root window.
The Windows controller validates that the same physical tip is still held, then
moves/resizes the HWND from pen coordinates while respecting window constraints.
Late requests, another pointer and stale completion messages cannot take over or
finish a later operation. This path does not synthesize a global mouse drag.

A marked handoff releases ordinary tablet tip/button/proximity ownership while
preserving the authorized physical window operation. Focus loss, read-only,
hide/unmap, capture loss, explicit cancellation and disconnect release ownership;
an unmarked policy revocation also stops window manipulation. Held or queued old
strokes cannot resume after regrant. Initial focus buffering is bounded to 64
samples and 500 ms. Tool caches are bounded to 16 native identities / 32 server
pen-and-eraser tools.

## Pen drag and drop

On a `pen_drag_v1` connection, a Wayland application can start
`wl_data_device.start_drag` with the current tablet tip serial on its origin
surface. The compositor verifies the seat, origin, mapped window, lease and
held tip. The viewer independently checks the same pen or eraser, its gesture
and the original HWND before accepting. A remote target receives a normal
Wayland data-device drag without pointer buttons or touchscreen contacts.

For a supported outbound text or file drag, Windows can promote the same
primary pen contact on a build that provides
[`ConvertPrimaryPointerToMouseDrag`](https://learn.microsoft.com/en-us/windows/win32/winmsg/winuser/nf-winuser-convertprimarypointertomousedrag).
The OLE loop starts from its matching system mouse message after the external
destination is ready. If Windows declines promotion, dragging between
streamed windows remains available. Return from OLE resumes the original
Wayland source, and a superseded operation cannot accept another tip's release.

## Validation

`tests/pen_smoke.py` exercises real tablet-v2 resources, axes, CSD operations,
remote-window drag, external file-sink handoff, and stale generation and tool
rejection. `tests/windows_pen_smoke.py` uses the production native adapter with
controlled samples on a private Wine desktop. It checks ownership, cancellation,
real HWND movement and preservation of the OLE completion token after pen-up.
Physical Windows pen promotion and drops into Windows apps remain to be checked.
