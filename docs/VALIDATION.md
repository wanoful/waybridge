# Validation

## Launcher layout refinement — 2026-11-18

On the physical Windows x64 laptop, a private catalog with over 80 entries verified aligned toolbar controls, a reserved left navigation rail, a directory header that pinned and released in the same scroll viewport, and equal icon coordinates for one- and two-line titles. The successful settings acknowledgement updated the settings view without opening the banner. The loaded PNG replaced its placeholder. An application launched from its tile appeared only on the separate Running page. The test used UI Automation without physical input injection.

## Full Launcher — 2026-11-18

- Isolated Windows x64 UI Automation verified globally placed search, keyword-based catalog results, grid and compact list switching, the command mode, a 128-pixel icon received over TLS, the pin action and a native remote window created by launching the test application from its tile.
- Private authenticated TLS icon tests cover exact installed desktop IDs, SHA-256, response bounds and missing icons. Catalog and launch tests cover localized metadata, Desktop Actions, literal transient arguments, working directories and preview without execution. The private Linux binaries were built together because the TLS service uses the compositor's icon resolver.

## Launcher and service administration — 2026-11-18

- The Windows launcher and eight settings categories use WinUI in the first package. WinUI x64 UI Automation on the physical Windows laptop exercised offline input settings, save/reload and effective status. An isolated TLS test then invoked the actual launcher button and observed a matching native remote HWND for a GIO-launched Qt fixture. The foreground-transfer probe requires an unlocked desktop and stopped before injecting input at the lock screen.
- The TLS-only Core tests and direct integration passed with a private TLS service, compositor and worker fixture. They checked pairing, certificate mismatch, retained session, revocation and cancellation or rejection of a launch for the wrong active profile. The existing Desktop Entry override regressions also passed.
- Isolated terminal tests exercised offline recovery by correcting an invalid saved FPS and preserving its TOML comment, previewing and saving application overrides and Portal settings, and reading logs and ending a session by its verified process identity. The online terminal displayed the full fingerprint and generated a one-use code for real pinned TLS pairing, then revoked the disposable device. The Release gateway passed session admission, launch plan, config limits, live policy, retained-session and revocation checks with an actual compositor. User-unit operations were kept out of the private fixture.
- The separate Avalonia proof used Avalonia 12.1.6 and FluentAvaloniaUI 3.1.0 in .NET 10 self-contained x64 and ARM64 publishes. The x64 package started on the laptop, registered a tray icon, accepted a UI Automation invocation, wrote a silent OS notification into history and received a simulated button action through the registered COM activator. Real notification clicks through to a Linux session and ARM64 execution are pending. Probe evidence is in `build/avalonia-preview/windows-x64.log`.

## Pen and gamepad integration — 2026-11-18

Implementation `4cd03a1` is installed on both ends. Linux user-service PID102010
runs the verified service inode and launches the new Release compositor; pinned
TLS hello, listener, certificate and persisted state/configuration are unchanged.
No installed compositor was active during replacement. Windows x64 GUI PID21400
runs in interactive session1 with a fresh MainWindow activation; all 543 package
files match their inventory. Existing profiles/trust remain byte-for-byte intact:
touch stays enabled, while the new pen/gamepad preferences default to off. Enable
them under **输入与共享**, then apply to the connection. Deployment evidence:
`build/deploy-pen-gamepad.json` and `build/deploy-pen-gamepad-{linux,windows}.log`.

Both opt-in features now share `main`, the hosted native worker and the control
center's saved/live input settings. New profiles default to off. Pen calibration
and gamepad deadzone/rumble remain independent of touch, IME, files and read-only.
Actual-source WinUI provider tests on Windows x64 pass private save/reload,
calibration, independent policies and supported/missing/error/readonly states.
They do not inject physical device input or require unlocking the laptop.

- Real tablet-v2 protocol tests pass early tablet announcement, live client-seat
  binding/release, pressure/tilt/rotation validity, pressure-less tip events,
  pen/eraser/barrel, bounded identities, held-tip CSD move/resize and exact
  serial/root/operation checks, malformed/stale rejection, focus/read-only/unmap/
  capture/disconnect cancellation and reconnect. Strict x64/ARM64 NativePen probes
  pass, with controlled samples and real HWND move/resize on private Wine.
  The production viewer callback/windowProc test additionally proves its WM_SIZE
  cannot revoke an owning pen resize and tip-up clears sizing.
- Real disposable Linux uinput/evdev tests pass standard button/axis/HAT delivery,
  actual FF_RUMBLE upload/play, finite effect expiry and zero feedback, lease
  revocation and node deletion. Backend permission/type failures are isolated
  from other input. Strict x64/ARM64 native gamepad probes with simulated XInput
  pass four-slot hotplug, incarnation/focus isolation, deadzone, exact motor
  routing, expiry and default-off behavior without touching unowned motors.
  Authenticated compositor-to-evdev E2E also passes default-off, matching-focus
  leases, real button/axis/HAT events, malformed/stale/replay rejection, held-input
  revocation on focus/read-only/disconnect and fresh neutral recovery. Only its
  uniquely owned PID/name/phys/sysname nodes are opened.
- Actual hosted PE tests pass default-off, explicit-on and both older-server
  capability modes, live toggles, requested-vs-active read-only status and atomic
  rejection of twelve malformed settings including signed/unsigned integer
  overflow. Four reconnects leave the application and compositor running.
- Workspace formatting/Clippy, 47 default Rust tests and 12 .NET Core cases pass.
  The real uinput test is also explicitly run and passes; the unrelated real-FUSE
  case remains ignored. Legacy native touch/window/DND and production OLE
  lifetime tests pass. Linux Release plus full x64/ARM64 worker/GUI packages build.

Physical Windows pen/controller input and motors, representative drawing apps
and SDL/Steam games remain **pending**. Native pen data-device DND/OLE is not yet
connected. Non-XInput controllers are outside this slice. Focus changes currently
recreate virtual gamepads and may change player indices; kernel nodes are
system-visible, with session-owned lifetime rather than process isolation.
These results do not complete M3 or the earlier positive OS touch/OLE tests.
Details: [PEN.md](PEN.md), [GAMEPADS.md](GAMEPADS.md).

Evidence: `build/pen-gamepad-{clippy,rust,uinput,release,client,packages,core,protocol,native-pen,native-gamepad,lifetime,touch,touch-interaction,gamepad-protocol,gui-native}.log`,
`build/tests/professional-input-settings/`, and
`build/tests/touch-settings-laptop.log`.


## Touch window, popup and DND integration — 2026-11-18

The Linux compositor/server slice at `00345c3` and Windows final release at
`c0c41b8` are installed. The user service's running inode, Release binary hashes,
pinned TLS hello and unchanged listener/configuration/trust were verified. No
installed compositor was active during replacement. The interactive x64 GUI
restarted successfully with a fresh MainWindow activation log; all 541 package
inventory hashes match, saved touch remains enabled, and all other profile fields
are unchanged. x64 and ARM64 full portable packages pass runtime/architecture
validation. Deployment evidence: `build/deploy-touch-interactions.json` and
`build/deploy-touch-interactions-{linux,windows}.log`. The final Windows regression
also confirms physical UP before OLE readiness cancels a claimed attempt promptly.

Default input selection now checks live `wl_touch` resources: subscribed native
Wayland clients receive touch, unsubscribed/X11 windows retain mouse compatibility.
Application overrides are optional. Releasing a subscription revokes active
input and updates metadata. Native move/resize, popup routing and same-session
Wayland URI/text DND now keep the original physical gesture and owning slot.

The real Wayland interaction fixture passes live binding/release, touch xdg
move/resize and invalid serial/owner/ACK rejection, popup coordinates/outside
dismissal, cross-client text and file bytes/source finish, private-sink
claim/return and deferred external drop. It checks all three return/UP/target
settlement orders, rejects stale metadata/invalid release targets, and reads
file ranges after prompt interaction retirement. Legacy native touch, real Qt
pointer DND and lazy file export pass. A generic Smithay touch-grab bug was fixed:
owning UP now requests drop instead of unconditionally canceling.

Production PE adapter and real HWND controlled probes pass gesture/slot reuse,
second-finger isolation, popup/root routing, focus/capture changes, external
handoff and completion, size constraints and maximized restore. The actual
viewer `windowProc`/callback regression additionally verifies WM_SIZE cannot
cancel its own touch resize, and returned touch motion bypasses an unclaimed
OLE wait. Existing target-lifetime/COM reentry, hosted IPC/live settings/Unicode
upload/detach and missing-field/default/older-server tests pass. Strict workspace
formatting/Clippy and 43 Rust tests pass (one real-FUSE test remains ignored by
the default runner). x64 and ARM64 workers build.

The actual-source WinUI settings probe passes on `laptop`, including selecting
an application, saving/reloading mouse compatibility and restoring automatic
selection, alongside independent input/sharing policies and persistent status.
It uses private state and automation providers, not physical touch injection.
Profile replacement now follows its stable ID rather than collection/reference
equality after settings with dictionaries are reloaded.

The Windows system handoff helper resolves the API by name, validates one active
primary contact, and enters OLE from real synthesized mouse input with a bounded
STA wait. Its x64/ARM64 independent probes compile strictly. The API's named
export is present on Windows build26200, but **positive OS touch-to-OLE and full
production-client touch injection remain unverified**: `LockScreenBackstopFrame`
covered both private fixtures. Ownership guards stopped before any injected
contact, and the user deferred unlocking. No unlock or lock-screen input was
attempted. Repeat the opt-in laptop probes when unlocked; API presence and Wine
controller tests do not replace those runs.

Evidence: `build/tests/touch-interaction-{host,source-*,receiver-*}.log`,
`build/touch-interactions-{clippy,rust,release,client,arm64,core,ui-live,drop-lifetime,gui-native,hosted-settings}.log`,
`build/tests/touch-settings-laptop.log`, `build/tests/touch-client-laptop/` and
`build/tests/windows-touch-ole-laptop.json`. Exact scope and runnable commands:
[INPUT_DEVICES.md](INPUT_DEVICES.md#window-popup-and-drag-integration--2026-11-18).

## User-reported physical touch and Chrome window gap — 2026-11-18

The user confirms finger drawing works in their current streamed setup and
reports that dragging the streamed Chrome title bar does not move the window.
This is user-observed physical drawing coverage, without a recorded device,
application version/backend or complete gesture matrix. Source audit identifies
mouse-only checks in both native Wayland move/resize authorization and Windows
window/toplevel-drag handling. Touch window management is not complete. Related
integration gaps and the next focused checks are recorded in
[INPUT_DEVICES.md](INPUT_DEVICES.md#interaction-integration-gaps--2026-11-18-audit).
No code change, deployment or new device acceptance is claimed by this audit.

## Native touch saved settings — 2026-11-18

The control center exposes experimental native multitouch under **输入与共享**.
The boolean `settings.touch` is saved per connection (missing fields default
false), transmitted at worker startup, and applied live with a separately
negotiated server capability. Disable cancels contacts; enabling obtains a fresh
lease. The worker no longer reads the former touch environment variable. The
raw viewer uses an explicit `--touch` argument.

Code commit `7b56055` is installed on both ends. The Linux Release compositor
and service binaries match their build outputs; the restarted user service's
running executable inode and pinned TLS hello were verified. Listener, unit,
configuration and trust identity remain unchanged, and no compositor was active
at replacement. The x64 Windows installation matches all 541 files in the new
package inventory. Its limited interactive GUI restarted at 00:37 UTC+8 and the
startup log confirms MainWindow activation. The existing single profile's
previously enabled choice was migrated to `settings.touch=true`; semantic checks
preserved all other profile fields. The former user environment variable was
removed and is absent from the restarted GUI's launch environment. x64 and
ARM64 portable packages both pass architecture/runtime/inventory validation.
Deployment evidence: `build/deploy-touch-settings.json`,
`build/deploy-touch-settings-{linux,windows}.log` and
`build/touch-settings-{release,packages}.log`.

Focused real-Wayland tests pass for initial OFF, live ON/OFF, contact cancel,
fresh generations, immutable negotiation and legacy clients. Native PE/Wine
adapter tests pass for preference changes without stale contact/tap replay.
The actual hosted worker and named pipe confirm default OFF, live server/client
status, malformed boolean rejection, and a reconnect warning against an older
server without sending unsupported messages. Core tests pass ten groups,
including true/false profile round trips, missing-field defaults, independent
input policies and separate-process startup/live settings serialization.

A private actual-WinUI probe on the x64 `laptop` renders the production toggle,
uses its Toggle/Invoke automation providers to save both values and reload them,
and preserves IME/read-only/file/clipboard choices. Persistent touch status also
survives generic success/save notices and distinguishes saved preferences from
the current connection, older-server reconnect requirements and read-only pause.
It does not connect a server
or inject physical touch. The existing hosted-worker regression also passes actual file upload, live
read-only/DPI/stream settings and graceful disconnect with retained applications.
Strict workspace formatting/Clippy and workspace Rust tests pass. Evidence: `build/touch-settings-{protocol,native,hosted,clippy,rust}.log`
and `build/tests/touch-settings-laptop.log`. Repeatable commands are documented in
[INPUT_DEVICES.md](INPUT_DEVICES.md).

## Initial installed update (superseded by saved settings) — 2026-11-18

The initial Release workspace at code commit `42704d9` was installed as
`~/.local/bin/waybridge` and `waybridge-service`. The user service restarted at
00:04 UTC+8; its running executable inode matches the rebuilt service hash.
Both installed binaries match Release outputs, configuration checks pass and a
real TLS hello succeeds with the unchanged certificate pin and 49092 listener.
Persisted identity/trust, Linux configuration and user unit contents are unchanged.
No compositor was active during replacement; existing application sessions were
not ended.

The matching x64 control-center/worker/runtime bundle replaced the existing
`%LOCALAPPDATA%\Waybridge-Test\m0-m1-20261114\app\Waybridge` installation on
`laptop`; all 541 inventory hashes were verified. Windows profiles and connection
settings were unchanged. The GUI was restarted in the logged-in user's limited
interactive session and its startup log confirms MainWindow activation.
`WAYBRIDGE_TOUCH=1` was persisted for that Windows user and explicitly inherited
by the restarted GUI, whose native workers inherit the same environment. The
standard environment-change notification was broadcast for subsequent launches.
This initially opted in native multitouch for subsequent connections; the saved
GUI setting above supersedes this environment-based setup. This initial update
did not include a new physical-touchscreen acceptance run.

Both x64 and ARM64 portable packages were rebuilt and validated; the actual
installed Windows device in this update is x64. Evidence:
`build/deploy-current.json`, `build/deploy-current-windows.log`,
`build/deploy-current-release.log` and `build/deploy-current-packages.log`.

## Parallel resource, input and network delivery — 2026-11-17

Three workstreams now share the `main` development baseline. This is a first
Linux resource-admission slice, experimental native multitouch and authenticated
UDP feedback pacing; it does not complete M2/M3 or physical-device acceptance.

- Linux CPU workspaces, GL attachments, CUDA pools and active/idle codec sessions
  now hold atomic reservation/split lifetime tickets, with session/window limits
  from `[media_budget]`. Exact scope/defaults/estimates are in
  [MEDIA_BUDGET.md](MEDIA_BUDGET.md). Queue dequeue cannot release a running
  frame's ticket. Native AVHWFramesContext finalizers retain pool owners after
  source destruction and after frames/encoder caches move between threads.
- Real low-budget PNG cases verify protected-target contention and recovery of
  final static pixels without a new application commit, impossible requests
  without unrelated target/codec churn, reconnect and complete lease return.
  The real CUDA/H.264 one-slot case provides usable temporary PNG, retains the
  focused video, then restores an independent IDR/P chain after release without
  application repaint; stale pressure cannot churn the recovered encoder.
- Native Windows multitouch is opt-in (now a saved GUI setting). Real Wayland
  `wl_touch` tests cover ten contacts, 175% coordinates, atomic malformed batches,
  stale generations, read-only, focus, unmap, disconnect and source destruction.
  The PE/Wine adapter verifies first-tap buffering, exact marked handoff, no
  buffered-tap replay after policy revocation/regrant, wrong-target leases and
  touch-only promoted mouse filtering. Both x64 and ARM64 workers compile;
  physical WM_POINTER acquisition, pads/rumble/pen and X11 native touch remain
  separate work. A generic Smithay fix ensures contacts already framed still
  receive cancel. See [INPUT_DEVICES.md](INPUT_DEVICES.md).
- UDP feedback uses existing authenticated ACK/repair messages and keeps reliable
  references/order. Seven UDP/controller tests cover pacing, delay/repair backoff,
  bounded recovery, early/duplicate feedback and final-DATA ACK before trailing
  parity. Production PE/CNG through a private relay delivers 180 exact ordered
  frames after 16 dropped shards and ten ACKs delayed 200 ms; the 80 Mbit/s ceiling
  test records about 8.0 Mbit/s backoff and 24.7 Mbit/s recovery. These are fixture
  control trajectories, not general throughput/physical latency claims. Existing
  twenty-window, disconnect, corruption/replay and H.264 TCP-repair paths pass.
  See [NETWORK.md](NETWORK.md).

Final strict formatting/Clippy and 43 workspace tests pass (one real-FUSE test
is skipped by the default runner and passes when explicitly executed). The full
Linux suite passes serially; Windows/Wine native/GUI/IPC/TLS/OLE regressions,
Core management tests and configuration CLI checks pass. Focused native touch
probes and x64/ARM64 rebuilds include the final handoff fix. Real H.264/HEVC/AV1,
FP16/Main10 HDR fallback and native CUDA lifetime/fault regressions pass.

Evidence: `build/diagnostics/media-budget-{png,oversized,cuda}.*`,
`build/parallel-media-regression.log`, `build/input-touch-{protocol,native,x64,arm64}.log`,
`build/tests/udp-adaptive-report.json`, and `build/parallel-{linux,windows}-suite.log`.
The deterministic GPU probe also verifies source/frame destruction before the
encoder, cross-thread final release, failed registration cleanup and independent
HDR primary/fallback pools. GPU/codec admission is an estimate; Windows decoded
textures and encoded transport packet lifetimes are not included yet.

The scheduler visits the focused window first. Two existing DND fixtures were
updated to collect initial metadata/pixels without assuming window-ID frame
order; drag content/action checks remain unchanged. Full suites use the same
`build/tests` generated protocol sources and should run serially against a shared
checkout, because concurrent fixture regeneration can expose incomplete headers.

## Capture admission and scheduling diagnostics — 2026-11-17

Existing application/device acceptance is the development baseline; remaining
compatibility cases continue alongside implementation. Development now proceeds
on `main`, fast-forwarded from `codex/m0-m1` without rewriting its 78 commits.

Window composition and CPU/CUDA capture now live in `src/capture.rs`. Before GL
allocation/composition/readback, media capture checks presentation credits, the
window's pending raw slot and the raw-queue byte estimate. A deferred or rejected
submission retains dirty; callbacks, metadata, input and other windows continue.
The raw queue checks actual CPU/CUDA bytes on submit and requeue and no longer
evicts another window's final unencoded update to make room. A failed worker
requeue asks for a fresh capture. Cursor and DND visuals use the same capture
implementation independently of media credit, preserving their feedback paths.

`--stats` keeps average/max fields and adds fixed-memory histogram P50/P95/P99
(bucket upper bounds, at most 6.25% rounding, capped at the actual maximum),
capture-to-encode and capture-to-network-queue durations, media-send queue time,
raw-queue entry/byte gauges and distinct wait/retry counters. Wait counters count
render polls, not dropped frames. The 128 MiB budget covers queued raw images;
it does not yet cover the active encoder frame, GL targets, driver sessions or
Windows decode textures. These same-clock stages do not measure physical
input-to-display latency.

`tests/media_backpressure_smoke.py` runs real Wayland fixture windows with a real
split-media viewer. It fills both windows' two presentation credits, repeatedly
requests refresh and resize without ACKs, and requires no further composition.
It separately verifies callbacks and ping while blocked, a second window's
independent recovery, then exact final 700×450 pixels after ACK without a fresh
commit/refresh. PNG and real NVENC H.264 runs pass, including queue/percentile
logging. The PNG test is included in `scripts/test.sh`.

The unchanged previous Release binary (SHA-256
`8bde0df87c18af31eff7158e875a8f881deb59c596e0cf7e666cc68329b57a8c`)
fails the same test because composition continues while credits are exhausted:
32 composites were recorded in the blocked interval. The updated PNG and H.264
runs record zero composites in that interval. This establishes avoided capture
work in this scenario, not a general latency/FPS improvement.

Commands and evidence (repository root):

```sh
cargo test --workspace --locked
python3 tests/session_fixture.py python3 tests/media_backpressure_smoke.py
python3 tests/session_fixture.py python3 tests/media_backpressure_smoke.py --codec h264
./scripts/test.sh
python3 tests/session_fixture.py python3 tests/hardware_smoke.py
python3 tests/session_fixture.py python3 tests/codecs_smoke.py
python3 tests/session_fixture.py python3 tests/hdr_video_smoke.py
python3 tests/session_fixture.py python3 tests/gpu_window_lifecycle_smoke.py
```

Strict workspace Clippy, formatting, workspace unit tests (34 passed, one
explicit-FUSE test skipped by default), the separately executed real FUSE test
and the complete `scripts/test.sh` Linux suite pass. Real NVENC H.264/HEVC/AV1
reference continuity, exact alpha, two-credit resume, codec fallback, cached resize
and reconnect checks pass. FP16/CUDA Main10 HEVC/AV1 tests retain 277 distinct
luma levels at 960×640 and pass HDR/SDR policy/fault recovery; same-size CUDA
window detach/remap also passes. Logs: `build/media-scheduling-linux.log` and
`build/media-scheduling-hardware.log`. No Windows client/protocol change is
required; these runs are Linux/GPU evidence rather than new physical-Windows
acceptance.

Reports are `build/diagnostics/media-backpressure-{png,h264}.json`; their `.log`
and `-app.log` companions contain diagnostics and callback evidence. The
before-change capture trace is `build/diagnostics/media-backpressure-baseline.log`.

## X11 CSD movement and native fullscreen — 2026-11-17

X11 CSD ConfigureRequest movement now hands the gesture to the independent
Windows HWND's native move loop. The viewer retains the original mouse-down
anchor, so request latency does not lose the first part of a drag. Releasing the
Linux grab prevents oscillation at the finite hidden X11 root's boundary.
This covers JavaFX/Wine-style client-driven
movement without application-name rules. Standard EWMH move/resize requests
also carry the actual cursor position into the native operation. Fullscreen
survives X11 buffer hides, native minimize/taskbar restore and external
SetWindowPos attempts. Physical frame aspect fitting and a bounded fullscreen
DPI-rounding correction remove fractional-scale edge gaps.

`x11_pointer_smoke.py` verifies real GLFW CSD movement, rejects placement without
a held gesture, and retains the pointer/DPI/RandR and pointer-lock regressions.
`windows_semantics_smoke.py` verifies external move/resize and system restore in
fullscreen, plus exact restoration of the previous normal/maximized placement.
Its geometry probes cover 2559/2561-pixel sources on a 2560x1600 viewport and
retained 16:9 letterboxing on a 16:10 display.

The opt-in `windows_game_laptop.py --host laptop` test drives the production x64
viewer with real Windows mouse input. Both ConfigureRequest and `--ewmh` move a
fixture HWND by exactly the requested 120x80 pixels. `--hmcl` also passes with the
installed HMCL 3.16.4 and a disposable HOME after its initial scene has loaded.
`--hmcl --drag-dx 600 --drag-dy 0` moves both the fixture and actual HMCL by the
full 600 pixels, crossing the hidden X11 root's placement limit.
The installed REDlauncher also moves by the full 120x80 pixels through a real
streamed title-bar gesture; its game launch is separate from this acceptance.
Fullscreen bounds remain 1920x1080 at the laptop's current 125% DPI after an
external move/resize and minimize/system restore. The rightmost and center
pixels both read RGB(25,76,127), rejecting a black/blank capture. `--dpi 134`
repeats the checks with nonintegral presentation density in the disposable
worker, without changing Windows display settings. The GLFW regression also
covers real deactivation to an owned native window and foreground restoration
with fullscreen bounds and caption state retained. A new physical Minecraft/AltSnap play session
and physical ARM64 acceptance remain separate checks.

Cyberpunk's reported failure was independently reproduced as Wine
`EXCEPTION_WINE_STUB` (0x80000100): builtin `icuuc.dll` lacks
`u_setMemoryFunctions_65`, before the game window is mapped. A temporary
native-ICU launch created the game window and confirmed that diagnosis. Per
project scope, no game DLL override or persistent Proton configuration is
installed by Waybridge. This does not certify the complete Cyberpunk workflow.

Manifold Garden also stalls during shader compilation in a separate Weston
15.0.1 headless GL compositor with a virtual seat,
on the same NVIDIA 620.70.06/GE-Proton11-3 stack. Direct Weston framebuffer
captures at 100 and 145 seconds still show `Compiling shaders... (60%)`; this
control does not involve Waybridge's compositor, encoder or Windows viewer.
This establishes that the observed stall can occur without Waybridge. Wine
versus DXVK versus driver responsibility has not been narrowed further. No compatibility-layer
workaround is installed, and neither game's complete workflow is certified.

## Native X11/Wayland DND interoperability — 2026-11-17

The production compositor now routes both native backends through the selectively
backported Smithay Source/DndFocus/OfferData subsystem. The old COPY-only source
worker is test-only. Provenance and the remaining synchronous XWM boundary are
recorded in [XDND.md](XDND.md) and `vendor/smithay/PATCHES.md`.

Real Qt Wayland/XCB source/target tests cover all four text combinations,
260,000-byte Unicode data with exact SHA-256, actual filesystem MOVE between
backends and inside one X11 window, Escape/read-only/disconnect followed by a
fresh successful gesture, and target closure without delivery. Native X11
visuals are checked for empty input metadata. The incoming X11 test includes
INCR-sized text and read-only URI copies into a MOVE-preferring real file model;
external delivery remains COPY. These tests do not certify arbitrary application
workflows or every XDND version, custom action and incoming INCR source.

The physical `laptop` test uses the production x64 PE worker and a real X11 Qt
application: native cross-window capture, leaving for OLE and returning to the
original source, and Unicode Windows/Linux out/in. It verifies target content
and the source's real XdndFinished, separately from Qt's provisional exec result.
The equivalent Wine test runs for both Qt backends. Empty X SHAPE regions,
Windows input hit testing and OLE nonclient-to-image motion prevent transparent
source/OS drag visuals from forcing false returns. The physical regression
manipulates disposable fixtures only; it does not establish a QQ workflow or
physical ARM64 acceptance.

The existing eleven physical Chrome cases also pass: both directions, immediate
release, Escape, added protocol delay, rejection/timeout, two streamed targets
and one Windows source switching targets. CDP only prepares/reads test pages;
actual SendInput/OLE supplies the drag. Linux Chrome tab detach/merge remains a
separate regression; an attached tab may lose its origin window on a successful
merge and must not be cancelled as a closed ordinary data source.

Late Wayland Receive/Finish, stale revisions, MIME/action feedback and visual
regressions remain in the full suite. A payload-lease regression verifies that
a retained protocol offer cannot pin a retired bounded snapshot. All application
fixtures use a private HOME/session bus and disposable profile. Logs are under
`build/xdnd-*`, including `xdnd-final-linux.log`, `xdnd-final-windows.log`,
`xdnd-laptop.log`, `xdnd-browser-laptop.log`, `xdnd-copy-policy.log`,
`xdnd-control-tests.log`, and `xdnd-payload-lease.log`. A tray test readiness race
was corrected to wait for its actual asynchronous icon registration before its
single fixture click; no production tray behavior changed.

The Release server/compositor were installed in `~/.local/bin` with no active
user compositor session. `waybridge.service` restarted at 02:16 UTC+8; executable
path/start time, installed hashes, configuration checks, administration readiness
and a real pinned TLS handshake pass. The 49092 listener, service configuration
and pairing certificate were preserved. The x64 GUI/worker/runtime bundle at the
existing `laptop` installation was replaced and its full SHA-256 inventory
verified; user profiles were preserved. No old-version backups were created.
ARM64 is cross-built/packaged, not physically accepted. Evidence:
`build/xdnd-deployment.json`, `xdnd-linux-deployment.log`,
`xdnd-windows-deployment.log`, `xdnd-control-packages.log` and
`xdnd-release-build.log`. Linux and Windows/Wine full suites and Core tests pass.

## Chrome text drag negotiation and source lifetime — 2026-11-16

The original physical-Windows Chrome reproduction passed with a 700 ms hover
but failed Windows → streamed Chrome with immediate release. The OLE target
returned NONE before the asynchronous Wayland acceptance arrived. Previous Qt
coverage did not establish Chrome compatibility. The preserved baseline is
`build/tests/browser-dnd/baseline-results.json` and its viewer trace.

The production path now separates immutable text representations, negotiation
state, native callback leases, target revisions and completion. OLE waits with
STA message dispatch for at most 750 ms of acceptance and 2 seconds of target
completion; it does not automatically retry uncertain delivery. Native source
wrappers can change across HWNDs, so a dedicated, short-lived mouse observer
tracks press boundaries independently and preserves one source snapshot across
targets. It performs no payload work or input suppression on the hook thread.
A separate “文字拖放” setting defaults on independently of file/clipboard sharing.
The design and exact limits are in [DRAG_AND_DROP.md](DRAG_AND_DROP.md).

The accepted version was deployed to both existing installations. Linux Release
hashes match `~/.local/bin/waybridge` and `waybridge-service`; the user service
restarted at 23:28 UTC+8. Configuration checks, local administration readiness,
its configured executable path/start time and a real pinned TLS handshake pass;
the 49092 listener and certificate are unchanged. No compositor session was
active during replacement. Windows x64 GUI, worker and runtime were replaced
at the existing `%LOCALAPPDATA%\\Waybridge-Test\\m0-m1-20261114\\app\\Waybridge`;
all 538 inventory hashes match and the profiles hash is unchanged. Service
configuration and pairing identity were retained, with no old-version backups.
Deployment evidence is `build/drag-deployment.json` and the corresponding
`build/drag-{linux,windows}-deployment.log` files.

`windows_browser_drag_laptop.py --host laptop --extended` uses actual Windows
Chrome selection, physical SendInput/OLE, the production PE worker and actual
native Wayland Chrome inputs in a private Linux session. CDP only prepares and
inspects disposable test pages; it never synthesizes DOM drag events. Eleven
cases cover both directions with 700 ms and zero hover, Escape, both directions
with 120 ms added DND delay per direction, a rejecting area, bounded cancellation
at 500 ms per direction, two streamed Chrome windows, and one Windows source
switching streamed targets while held. Assertions require exact target text,
original source text, one real source dragstart and COPY/NONE at dragend. The
handoff also requires the same gesture and reuse of the immutable source.
Only test-owned windows are raised; previous cursor/foreground are restored.
This is not evidence for production foreground policy or physical ARM64.

Linux and Wine full suites pass. Targeted fixtures additionally verify stale
revision rejection, delayed old Receive/Finish, immutable retarget data, one
Drop and actual completion acknowledgment, native Query/GetData/medium-release
reentry, text enabled with file sharing off, explicit text opt-out, and a rapid
release/new press with no intervening STA timer. Private Xwayland target
Unicode/INCR and read-only URI copy regressions pass; this does not implement
X11-origin or complete mixed-backend dragging. HTML/RTF/images and pre-transfer
file hover remain open.

Logs: `build/drag-full-linux.log`, `build/drag-final-windows-suite.log`,
`build/drag-final-chrome-laptop.log`, `build/drag-revision-protocol.log`,
`build/drag-final-gesture-probe.log`, `build/drag-final-xdnd.log`,
`build/drag-core-tests.log`, `build/drag-final-clippy.log` and
`build/drag-final-control-packages.log`. Real browser event/metadata records are
under `build/tests/browser-dnd/`; those files contain only disposable fixture
page data. Core tests verify setting defaults and independent persistence;
x64/ARM64 package checks verify matching PE/resources/checksums, separately
from physical device acceptance.

## Desktop interaction follow-up — 2026-11-16

Delivery status: both installed programs were updated after acceptance. Linux
`~/.local/bin/waybridge` and `waybridge-service` match the final Release outputs;
`waybridge.service` restarted at 21:44 CST, its executable path/start time and
real TLS handshake were checked, and the certificate and 49092 listener remain
unchanged. No user compositor session was active during replacement. Windows
x64 at `%LOCALAPPDATA%\Waybridge-Test\m0-m1-20261114\app\Waybridge` was updated
with the matching GUI, worker and runtime bundle; all 537 inventory entries
matched SHA-256, and the existing profiles file hash was preserved. No old-version
backups were created. Service configuration, pairing identity and notification
provider selection were preserved.

Final `scripts/test.sh` and `scripts/test-windows.sh` both passed, as did Core
unit tests, Release workspace build, x64/ARM64 publishing and packaging checks.
Logs: `build/final-linux-suite.log`, `build/final-windows-suite.log`,
`build/current-core-tests.log`, `build/final-control-packages.log`,
`build/final-gui-focus-run.log` and `build/final-deployment.json`.

The tray callback/activation, runtime window-icon, foreground/modal-owner,
generic Unicode DND/capture/return and DND-icon fixes are integrated. Direction,
format and remaining interoperability limits are recorded in
[DESKTOP_INTERACTIONS.md](DESKTOP_INTERACTIONS.md); these results do not establish
complete X11-origin or mixed Wayland/X11 drag support.

- Physical Windows x64 `laptop`: the shared production WinUI UI performs actual
  Connect and Launch clicks, GIO launches two different Linux Qt processes,
  and exact HWND/worker PID readback proves each new process receives foreground.
  A standard owned modal closes back to its exact parent. See the complete GUI
  acceptance below and `build/tests/gui-focus-laptop.log`.
- Physical Windows x64 DND: `windows_data_drag_laptop.py --host laptop` runs the
  actual production worker against a private Linux compositor and real Qt
  widgets. Cross-window text and OLE departure/return finish as native MOVE;
  successful outbound Unicode OLE finishes as COPY at both the Windows target
  and original Qt source; inbound Windows Chinese text reaches the Qt editor.
  The transcript is `build/tests/data-drag-laptop.log`. Only disposable probe
  windows are temporarily raised above existing topmost user windows to isolate
  mouse input; their order, cursor and previous foreground are restored. This
  DND test is not used as evidence for production foreground activation.
- Real protocol fixtures cover runtime Wayland/X11 icons, immutable delayed
  server offers, inactive target rejection, standard source MIME acceptance and
  exact finish/cancel behavior. Native COM fixtures cover reentrant target
  closure/replacement, text-target handoff and strict Unicode size/encoding.
- The private Xwayland integration accepts Unicode and INCR text, and receives
  read-only URI files into a writable independent QFileSystemModel copy. Eleven
  lower-level X11 cases cover proxy/version/selection/chunk/identity/cancel
  behavior. No application name appears in the transfer dispatch.
- Source acceptance feedback was previously missing in vendored Smithay:
  Windows could receive bytes while Qt reported Ignore. The standard
  `wl_data_source.target` feedback is now present; real Qt/Wine and physical
  source/target completion assertions agree on MOVE/MOVE/COPY.
- The complete GUI test also found and fixed Windows SSH management bootstrap
  command-line overflow. A bounded stdin bootstrap replaces the long argv;
  Core and real private OpenSSH management regressions pass. Direct TLS profiles
  retain their normal transport.

HMCL/QQ installed desktop associations are covered by generic StartupWMClass,
class/instance and runtime-icon fixtures; their actual icon appearance in every
application version still needs the application matrix. ARM64 compilation is
separate from physical ARM64 acceptance. Cross-device file hover, richer drag
formats and full X11 source/target bridging remain open work.

## Linux server deployment — 2026-11-16

The M2 Release compositor and TLS entry point were atomically installed as
`~/.local/bin/waybridge` and `~/.local/bin/waybridge-service`, without old-version
backups. `waybridge.service` was restarted and is executing the new entry-point
inode. Installed SHA-256 hashes match the build outputs. Both installed binaries
pass configuration validation; the local administration endpoint and a real TLS
handshake succeed, with the existing certificate and `0.0.0.0:49092` listener
preserved. No Waybridge compositor session was running during this update.

Existing service drop-ins and user configuration were preserved. Notification
provider selection was not changed: this machine has installed `mako.service`
and `dms.service` units declaring the same notification BusName. No Waybridge
notification unit was installed or enabled as part of this binary update.
Windows deployment was not changed. New sessions use the updated compositor;
new desktop capabilities require a matching Windows client.

## M2 integrated desktop delivery — 2026-11-16

The first M2 batch is integrated: shared branding and connection badges,
read-only file inputs, SNI/DBusMenu trays and background actions, and explicitly
selected native desktop notifications. At this verification stage installed
programs were not replaced; device probes used their own temporary objects and
registrations. The subsequent Linux deployment is recorded above.

- Workspace formatting, strict Clippy, workspace unit tests and the explicit real
  FUSE acceptance pass. After the final descriptor-budget fix, tree transfer,
  stalled-worker/reconnect and actual Dolphin/KIO writable-copy tests pass again.
  Debug and Release workspace builds pass.
- The M0/M1 Linux phase passes, including real systemd startup/error shutdown,
  native desktop services, admission/launch/configuration, Chrome/Qt/Dolphin/X11,
  pinned TLS/OpenSSH, actual NVENC codecs and HDR protocol regressions. These
  are software/host checks, not physical Windows HDR validation.
- Windows x64/ARM64 workers and self-contained WinUI packages build. Packaging
  checks the matching notification resource DLL's PE architecture, required brand
  assets, manifests, runtime resources and SHA-256 inventory.
- Wine native icons, tray/menu/input gating, notification IPC, drop-target
  lifetime, ordered sending, TLS/Schannel/direct control/media/audio, hosted
  startup/live settings/Unicode upload, and file/directory virtual-stream drags
  pass. Windows cases were completed in focused reruns after correcting Wine's
  unsupported taskbar property assertion and awaiting compositor/FUSE shutdown
  before a GUI fixture removes its temporary directory.
- Icon self-tests cover default/custom/reset badges, unmodified master pixels,
  generic fallback, 100/125/175/400% DPI and 300 HICON replacements. `laptop`
  passed native WM_GETICON and strict window AppUserModelID readback: same-app
  grouping labels, distinct profile/app labels and separate unknown windows.
  Wine's window property store is explicitly reported as a limitation, not a
  Windows identity pass. Properties are removed during window destruction.
- Native Explorer tray/menu acceptance is recorded in [DESKTOP-TRAY.md](DESKTOP-TRAY.md).
  Notification registration/center/activation-interface checks and the repaired
  runtime packaging are recorded in the M2 notification section below. Actual
  Shell banner clicking/cold start, DND visuals, full taskbar interaction, mixed
  monitor DPI and ARM64 device operation remain separate acceptance work.

Reproduction entrypoints are `scripts/test.sh`, `scripts/test-windows.sh`, and
`scripts/test-m0-m1.sh --with-wine`; Windows-only regressions still use a private
Linux session fixture plus Wine/Xvfb. Latest integration logs are
`build/m2-full-regression.log` (Linux phase), `build/m2-windows-regression.log`,
`build/m2-gui-native-regression.log`, `build/m2-windows-drag-regression.log`,
`build/m2-final-files.log`, `build/m2-release-build.log` and
`build/m2-control-packages.log`. Native icon/tray reports were retained under
`build/tests/m2-*-laptop.log`. Source feature branches are retained for history;
temporary parallel worktrees can be removed after integration.

## Error shutdown and Windows drop-target lifetime — 2026-11-15

Desktop-service status reporting is now best-effort. A failed status write or
observation no longer skips service startup, shutdown or activation-environment
cleanup. Failure to allocate the lifecycle worker rolls back the environment
using the existing ownership checks. `session_service_status_smoke.py` blocks the
initial status write; the real-manager `session_lifecycle_smoke.py` also blocks
ready and stopping reports. Sessions remain usable, components stop successfully
with a live display, and unset/empty values and unrelated services are preserved.

Application startup, session descriptor publication and event-loop errors now
share the normal display-aware shutdown path. `session_error_shutdown_smoke.py`
injects one accept error on its disposable Wayland socket, and separately checks
an unavailable application and failed descriptor publication. The original error
is returned after service cleanup. Against the prior installed Release binary,
the event-loop case left the component unable to finish its Wayland roundtrip
and systemd killed it after its stop timeout; the fixed binary stops it normally.
The injection is a test-only preload library, not a production runtime option.

Windows drop callbacks now identify their target by window ID and a weak lifetime
token, retain their own COM reference during reentrant callbacks, and revalidate
after external data/medium operations. Closing a target also invalidates queued
virtual-file and filesystem uploads, including replacement with the same ID.
`windows_drop_lifetime_smoke.py` passes under Wine: normal accept/leave plus
twelve closure/replacement combinations during query, data retrieval, medium
release and asynchronous setup cancel without scheduling an upload.

Workspace formatting, strict Clippy, 29 Rust tests, Debug/Release builds,
`GVFS_DISABLE_FUSE=1 ./scripts/test.sh`, the real-manager lifecycle/error tests,
session admission, native fixture KWallet/GTK Portal and Windows x64/ARM64 client
builds pass. Existing Wine virtual-file stream/HGLOBAL/pending cases and
bidirectional file/directory clipboard and OLE drag regressions also pass. New
regressions are included in the regular or M0/M1 test entrypoints. Logs are under `build/tests/lifecycle-fixes/`,
with the drop callback trace at `build/tests/windows-drop-lifetime.log`.
The tested Release compositor was subsequently deployed to
`~/.local/bin/waybridge`, and the x64 worker replaced the existing executable in
`laptop`'s `%LOCALAPPDATA%\Waybridge-Test\m0-m1-20261114\app\Waybridge`.
Installed hashes match the build outputs; the Windows checksum manifest was
updated and all 517 package entries verify. No old-version backup was created,
as requested. The deployed Linux binary passes the isolated error-shutdown
regression, and the existing TLS entry point remains healthy. The Windows worker
version and capability commands exit normally over SSH; codecs are unavailable
in that SSH context, so this does not establish interactive hardware decoding or
physical drag-and-drop acceptance. Existing services, credentials and real wallet
data were not reconfigured.

## KWallet protocol disconnect and Portal activation — 2026-11-15

The user's Chrome Paused report coincided with `kwalletd6` exiting on
`Invalid binding of wl_output version 4`, followed immediately by Chrome's token
decryption error. Window destruction removed a per-window output global before
another client's already queued bind reached the compositor. Outputs are now
disabled immediately and reclaimed after a ten-second grace period, following
the wayland-server global-lifetime contract. New clients do not see retired
outputs, and existing bound objects remain valid through global reclamation.

`output_bind_race_smoke.py` deterministically queues a real wl_output v4 bind,
closes another client's window, then flushes the bind. The old server rejects it
with the same protocol error; the fix passes. The test also checks hidden retired
globals and releasing an already bound object after deferred reclamation. It is
included in `scripts/test.sh`.

The machine's earlier renamed Portal wrapper units also conflicted with the
distribution's D-Bus `SystemdService=` mapping. They were backed up and removed;
the canonical `xdg-desktop-portal.service` and GTK unit now use Waybridge lifecycle
drop-ins. A real isolated systemd user manager checks the deployed configuration
with native activation names, provider startup and shutdown. Component setup must
reuse existing unit names rather than allocate the same BusName twice.

Strict workspace Clippy, 29 Rust tests, Debug/Release builds,
`GVFS_DISABLE_FUSE=1 ./scripts/test.sh`, and the native KWallet/GTK Portal UI tests
pass. Logs are under `build/tests/wallet-reliability/`. The fixed Release compositor
and corrected user configuration are deployed. After reconnecting and unlocking
the real wallet, the user confirmed Chrome returned to normal. No Cookie/password
database, wallet secret or OAuth token contents were inspected, migrated or reset.

## X11 absolute-pointer clipping — HMCL and Minecraft laptop retest

The reported inaccessible HMCL buttons and Minecraft menu offsets were traced to
Xwayland's root-screen clipping. The native viewer displayed the entire HMCL
window while its X11 geometry was `1236x571-283+591` on a `1920x1080` screen.
Positions outside the root were clipped before reaching JavaFX/GLFW. A controlled
regression against the old server requests local X=25 and observes X=283.

Managed X11 top-levels are now fitted inside the shared screen on mapping,
configure requests/notifications, viewer resize and reconnect. The shared RandR
topology remains stable across individual-window operations. The viewer also
reports full monitor dimensions separately from work-area dimensions, so a
taskbar does not shorten the fullscreen coordinate space. Old v3 clients retain
the work-area fallback; update both binaries for full-monitor reporting.

Physical `laptop` verification used the existing TLS/Tailscale connection, its
actual x64 worker, 125% DPI and hardware presentation:

- HMCL 3.16.4: Settings navigation, custom minimize/restore and close buttons
  respond; reopening uses valid X11 geometry `1236x571+0+509`. Clicking Launch
  Game starts the user's installed Minecraft 26.1.2 Fabric instance.
- Actual Minecraft main-menu pointer samples match Windows client coordinates
  in windowed `854x480`, maximized `1920x1051`, and true F11 fullscreen
  `1920x1080`. Both corner samples were checked; fullscreen `(1880,1040)` and
  `(40,40)` reach exactly those X11 local positions. The fullscreen Options
  button opens the real settings screen; Escape returns to the main menu.
  No world was opened. Interactive automation restores the game if its normal
  fullscreen focus-loss policy iconifies it between scheduled test actions.
- The installed Linux Release compositor and laptop worker were updated; the
  previous laptop executable is retained as a backup. Pairing identity and the
  TLS entry remain unchanged.

Automated coverage: `x11_pointer_smoke.py --bounded-screen` reproduces negative
placement with a finite 1920x1200 monitor/1920x1080 work area, maximized/fullscreen
edge clicks and pointer-lock teardown. The original 175%/125% GLFW case, real
GLX and bidirectional X11 clipboard tests pass. `GVFS_DISABLE_FUSE=1
./scripts/test.sh`, strict workspace Clippy, 29 Rust tests, Debug/Release and
x64/ARM64 packaging pass. The FUSE variable prevents an unrelated GVFS mount
from outliving a short-lived test directory. Generated measurements/screenshots
and transcripts are under `build/tests/pointer-laptop/`.

Coverage is the tested single-monitor configuration and selected game/launcher,
not every modpack, display-hotplug or mixed-monitor configuration.

## Configuration, desktop overrides and standard session lifecycle — 2026-11-14

Project documentation is now under `docs/`, with a root navigation README. Usage,
configuration syntax and component installation are documented in
[configuration](CONFIGURATION.md) and [desktop integration](INTEGRATION.md).

The Linux/Wine M0/M1 regression passed, including workspace formatting, strict
Clippy and unit tests, real Linux graphics/input/files, TLS/OpenSSH/.NET, actual
NVENC/HDR, native Schannel/GUI IPC, audio and OLE file/directory flows. The suite
was run as `python3 tests/session_fixture.py ./scripts/test-m0-m1.sh --with-wine`;
the real systemd test was run separately outside the simulated user manager.
The regular script now performs that real-manager test before entering its
fixture. Logs are `build/tests/config-session-regression.log`,
`config-final-clippy.log`, `config-final-unit.log`, and `config-real-lifecycle.log`.

- `configuration_smoke.py` checks both binaries' defaults, XDG lookup/fallback,
  explicit file replacement, CLI precedence, unknown/type/range errors,
  provenance and absence of state/network/session side effects. Management and
  explicitly named existing-session attachment remain usable with a malformed
  new config. Configuration errors do not echo file contents.
- `desktop_overrides_smoke.py` uses actual GIO and a disposable bus. It covers
  complete folded desktop IDs, original `%k`, literal arguments/percent/quotes,
  extra arguments before `--`, independent actions, Path/HOME/cwd, environment
  unset, standard Activate/Open/ActivateAction, process-override conflicts and
  explicit Exec mode. Source entries and ordinary local GIO launches remain
  unchanged. Derived entries stay private and survive until session cleanup.
  Additional regressions check a legal NBSP in an executable filename and an
  application-only `PYTHONHOME`: overrides use GIO launch context rather than
  altering the Python dispatcher environment.
- `session_launch_policy_smoke.py --actual-chrome` passes with temporary Chrome
  data, including an explicit scoped overlay, actual remote windows and reuse of
  the same original-profile instance. It does not read the user's real wallet,
  cookies or saved passwords, or certify restoration of their account sign-in.
- `direct_service_smoke.py` checks the actual TLS service/compositor with an
  explicitly selected TOML: omitted media fields use file defaults, over-limit
  startup and live requests fail, allowed updates return effective policy, and
  requests cannot select server executables/configuration or rewrite the file.
  Reconnect retains the same session despite a newly invalid config.
  `launch-plan` does not execute the application or expose environment values.
  The embedded TLS helper also launches a real shell desktop entry with scoped
  cwd and `PYTHONHOME`, verifying dispatch and the target's effective environment.
- `session_lifecycle_smoke.py` runs a real nested systemd user manager and real
  user bus inside a delegated test scope, with temporary HOME/runtime and a
  restricted unit search path. A `Type=notify` service connects and round-trips
  Wayland before READY and again during shutdown. Declared `PartOf` services stop
  before display destruction; an unrelated service survives. It also checks
  true unset versus explicit empty values, D-Bus/systemd activation forwarding,
  mixed valid/stale previous displays, later-desktop environment preservation,
  and refusal to adopt an already active target or pending startup transaction.
  The host manager only allocates the test scope; host desktop targets and
  activation environment are untouched.
- Native wallet/GTK Portal tests pass with components selected explicitly by
  fixture D-Bus aliases and Portal configuration. The production core no longer
  activates or polls named wallet providers, generates Portal configuration, or
  forces Qt/GDK backends. Installer tests use temporary paths and verify layout,
  idempotence and refusal of modified-unit/Portal conflicts before any writes.

Debug and Release workspace builds pass. Windows x64 and ARM64 portable packages
are rebuilt with the matching embedded SSH helper and the `docs/` directory.
The native worker is unchanged. Python 3.11+ and system GIO/GLib are required for
the management helper; no Python package installation is needed.

This revision has not been retested on physical Windows. The existing x64 device
results below keep their original scope. Existing saved-credential access,
Flatpak document grants, physical WoA/HDR and later-roadmap features remain
separate acceptance work. The D-Bus daemon's missing unset API is explicitly
reported; systemd applies genuine unset after daemon activation forwarding.

## Shared user data and one-way session admission — 2026-11-14

This supersedes the earlier private-bus/per-session wallet and browser-profile policy; the historical device results below retain their original scope. See [SESSION_POLICY.md](SESSION_POLICY.md).

`./scripts/test-m0-m1.sh` passed in full, including formatting, workspace Clippy/unit tests, real Linux graphics/input/file regressions, native wallet/Portal tests, authenticated TLS and real OpenSSH management, .NET control tests, live NVENC codec changes and HDR policy/numerical regressions. The separate actual-Chrome default-profile test also passed. Debug and Release workspace builds passed. Existing vendored Smithay warnings remain unchanged. Windows x64 and ARM64 self-contained control-center packages were rebuilt successfully, including the updated embedded SSH helper.

- `session_admission_smoke.py` uses a real compositor with disposable D-Bus/logind/systemd fixtures. It checks active/background Wayland and X11 rejection, ignored greeter/TTY/user-manager/other-UID/closing sessions, fail-closed logind errors, duplicate compositor exclusion, attachment and auto-reconnect despite later desktop login, lock release, shared application paths/bus, canonical user-bus fallback with an absolute Wayland socket, and restoration that preserves a later desktop's environment.
- `desktop_services_smoke.py` exercises installed KWallet 6.31.0 and GTK Portal with temporary user data, including a runtime path with spaces, percent signs and quotes. Actual remote pixels/input select a Unicode file and cancel wallet prompts; explicit user-service reactivation after terminating the fixture wallet uses the current display. Service data paths match application paths, user activation variables restore, and an unrelated background process survives session end.
- `session_launch_policy_smoke.py --actual-chrome` opens installed Chrome in its ordinary user profile under temporary XDG directories; no profile or password backend is injected. Command and GIO/desktop-entry launches reuse that browser instance. Explicit flags, Unicode and literal argv survive. This does not test access to the user's real saved passwords or wallet.
- The Release compositor's read-only `--check-session` on the real desktop reports the existing graphical session and asks the user to log out; no real wallet, DM/PAM configuration or live service activation environment was changed.

The test fixture provides private user/system buses and a simulated login manager rather than bypassing the production check. Shared document-portal mounts stay inside the fixture lifetime and are detached during fixture cleanup. The OpenSSH fixture explicitly passes these test bus addresses because SSH does not inherit them automatically.

Windows hardware has not been retested for this policy change. TLS uses the updated server helper; SSH control-center distributions must carry the updated embedded Python helper. Chromium password-backend selection remains application policy; KDE users may need to retain `--password-store=kwallet6` explicitly when reusing their original encrypted data.

## Windows 11 x64 laptop — 2026-11-14

Completed a first physical Windows x64 pass over the existing Tailscale VPN. The `laptop` host runs Windows 11 Pro 10.0.26200.9974, with Intel Iris Xe and NVIDIA RTX 4060 Laptop graphics. The portable x64 package was copied to `%LOCALAPPDATA%\Waybridge-Test\m0-m1-20261114`; private interactive scheduled tasks exercised the existing logged-in desktop using UI Automation and Win32 APIs. No VM, RDP desktop or Wine rendering was used for these device results.

The pass found and fixed two issues that Linux/Wine had missed:

- The native [HostControl pipe](../client/host_control.h) used a synchronous handle for its concurrent reader and writer. On Windows, the pending read prevented ready/status output even while TLS streaming already displayed working windows. It now uses `FILE_FLAG_OVERLAPPED`, separate read/write events, cancellation with completion draining, and bounded bootstrap/write deadlines. See Microsoft's [overlapped pipe requirements](https://learn.microsoft.com/en-us/windows/win32/ipc/synchronous-and-overlapped-input-and-output). The standalone [Windows pipe probe](../tests/windows_host_control_probe.cpp) passes on this physical host: pending read plus ordered ready/status writes, coalesced/fragmented commands, cancellation of pending read/write, invalid bootstrap cleanup and a 15-second incomplete-bootstrap deadline. The test-side [pipe fixture](../tests/windows_pipe_fixture.h) is also duplex/overlapped.
- [EngineChannel startup](../gui/Waybridge.Control.Core/EngineChannel.cs) treated a cancelled process-exit wait as an actual exit, then awaited live stderr EOF before killing the worker. Startup could therefore remain pending indefinitely. It now propagates cancellation before reading exit diagnostics and reports its internal startup deadline as a timeout. The Core regression uses a separate process that accepts bootstrap but never reports ready or closes stderr. It fails against the old branch and passes after the fix, including bounded cancellation and child termination.

Physical results after applying both fixes:

| Area | Observed result |
|---|---|
| WinUI startup | Real controls, navigation, resources and application window load successfully; the previous bootstrap fix is confirmed on x64 |
| Pairing and credentials | Real GUI pairing with a locally obtained pin/code; client credential exists in Windows Credential Manager and is absent from profile fields; GUI revocation removes the credential and Linux trust |
| VPN connection | C# TLS management and independent Schannel control/media streams connect across Tailscale; GUI reaches connected state and receives live diagnostics |
| Pixels and input | Two deterministic native windows rendered actual H.264 pixels; a 1200×800 capture has the expected red upper and blue lower regions; click and `abc` scan codes arrive in the Linux fixture log |
| Hardware path | Actual worker logs report D3D11 hardware H.264 decoding and DirectComposition presentation; HEVC/AV1/Main10 capability probes also complete, but capability alone is not playback or HDR-display acceptance |
| Applications | GUI catalog launches Blender 5.2.2 LTS to its initial viewport/setup screen; GUI terminal launch opens Ghostty. This is launch/display coverage, not a complete Blender project or game acceptance |
| File picker/upload | Real owned Windows picker selects a Chinese-named test file; all 151,552 bytes arrive intact over TLS and match the expected UTF-8 contents |
| Disconnect/reconnect | Normal GUI disconnect terminates the local worker while compositor, Blender and Ghostty retain exactly the same PIDs/start identities. Reconnect starts a new Windows worker and resumes those same instances |
| Explicit end | GUI confirmation ends the test-owned compositor and both applications; the session descriptor is removed. A separate controlled check confirms ending a session leaves the detached TLS entry and its trust/identity running |
| Tray/single instance | Closing hides the window and another launch reactivates the same GUI process. Tray-menu Exit was not reliably driven by the automation harness, so its result is not accepted; the test-owned GUI was stopped during cleanup |

The existing native GUI IPC and full direct TLS/WASAPI Wine regressions pass after the fix, as do Core, .NET direct and SSH regressions. `windows_tls_smoke.py` now includes the standalone pipe probe. ARM64/x64 workers and both portable bundles have been rebuilt and each package's PE/resources/checksums verified. Test transcripts, UI results and test-window captures are under `build/tests/laptop/results/`, including `pipe-probe.log`, `core-after-fix.log`, `upload-verified.json` and `blender-windows11.png`; these generated artifacts are ignored by Git.

Test pairing credentials, temporary scheduled tasks, Linux sessions and the isolated TLS entry were cleaned up. The portable package and diagnostics remain in the dedicated Windows test directory.

Remaining device coverage includes tray-menu Exit, the updated ARM64/WoA package, physical Main10/HDR display behavior, real audio-device playback/recovery, IME/device variations, games and complete professional-software workflows. This pass covers the selected Tailscale path, not all LAN/VPN loss and performance conditions. Later milestone devices/features and Flatpak document authorization retain their previous pending status.


## M0/M1 — 2026-11-14

Implemented core I/O cleanup and the independent-session/TLS connection baseline. At the initial delivery Windows hardware verification was deferred; the later physical x64 pass is recorded above. Linux, cross-build and Wine results below do not substitute for remaining hardware coverage. Older entries describe their historical implementation: the host keyring bridge has now been replaced by native private-session services.

The complete `scripts/test-m0-m1.sh --with-wine` run passed on this NVIDIA Linux development host, including strict workspace Clippy, 26 Rust unit tests, real Linux application regressions, TLS/.NET integration, HDR NVENC and native Wine checks. It uses temporary application data for test sessions. `scripts/build.sh` (Linux Release workspace plus x64 native worker) and `scripts/build-control-center.sh all` also passed after source freeze. ARM64/x64 portable packages are `build/Waybridge-Control-win-{arm64,x64}.zip`; transcripts are `build/tests/m0-m1-validation.log`, `m0-m1-build.log` and `m0-m1-packaging.log`. Focused coverage:

- Linux receive-side disk I/O runs on an ordered bounded worker, with connection/clipboard generations and asynchronous cancellation. `transfer_worker_smoke.py` deliberately blocks an actual write with a narrowly scoped preload fixture, verifies responsive input/ping and a new connection, rejects a queued stale commit, reuses the transfer ID on the new connection and preserves committed files.
- Windows input/control writes use a bounded FIFO sender; receive-side filesystem work uses an ordered worker. Portable sender stress and the real PE tree probe cover ordering, cancellation, slow I/O, stale/disabled sharing, completion overflow and 10,000 coalesced cancellation requests. Queue overload explicitly fails the connection. This does not claim every filesystem call is asynchronous: legacy non-file_tree upload metadata preflight still has a synchronous path.
- Shared CLI/live codec resolution and `stream_config_hdr_smoke.py` exercise real NVENC auto SDR H.264 / HDR HEVC switching. `gui_codec_smoke.py` verifies H.264 → PNG → HEVC → AV1 → H.264 and independently decodable new epochs. `hdr_video_smoke.py` passes actual HEVC/AV1 Main10 color/alpha, resize/cache credits, SDR monitor fallback, missing explicit HDR capability, HDR-only policy changes and GPU encode failure recovery. Physical Windows Main10/HDR display behavior remains unverified.
- `desktop_services_smoke.py` runs two actual same-UID Waybridge sessions with private D-Bus activation. It selects a Unicode path through the real GTK Portal picker using viewer input, renders/cancels a real Secret Service prompt, restarts the wallet provider, verifies service-only persistent paths and unchanged application XDG paths, and checks teardown including an activated SIGTERM-ignoring descendant. The host-runtime variant leaves the host document-portal mount unchanged; `--special-runtime` additionally verifies spaces, percent and quotes in the runtime path with actual Qt/GTK providers. Unit checks cover D-Bus address encoding/decoding and exact owned FUSE mount matching. This is not full Flatpak sandbox/document authorization or ScreenCast coverage.
- `session_launch_policy_smoke.py` checks browser-family persistent profiles, scoped Secret Service flags, explicit options, GIO desktop expansion and Steam host conflict detection. Real installed Chrome argv/catalog launches also pass with spaces, percent, apostrophe and dollar signs in temporary paths; they do not read or modify the user's existing browser profile or credentials. Steam is covered by controlled process metadata fixtures, not an actual Steam/game launch. Known browser/Steam catalog wrappers or sandbox launchers fail explicitly; arbitrary wrappers and nested Portal/xdg-open requests are not intercepted. Other browsers and every installed game remain separate compatibility work.
- `service/tests/direct_service_smoke.py --compositor target/debug/waybridge` exercises the real rustls daemon and compositor: pairing code replay/expiry, wrong credentials, pinned identity, resource/state ownership limits, authenticated session creation/launch, raw v3 stream forwarding, reconnect, daemon restart with the same certificate/trust, revocation and explicit session termination.
- `gui_direct_smoke.py` runs the actual .NET `DirectAgent` and `SessionController` against that TLS entry and compositor: pin rejection, pairing/replay/authorization, literal Unicode argv, application catalog, retained-session reconnection, explicit end/revoke, private worker bootstrap and profile/log credential omission. Its IPC worker is a controlled fixture; the real native TLS worker is independently exercised below. Core console tests and the actual OpenSSH regression also pass.
- `windows_tls_smoke.py` executes Schannel in a real x64 PE under private Wine/Xvfb: three authenticated streams, 256 KiB fragmented/coalesced TLS round trips, wrong pin rejected before credentials and no credential logging. `windows_direct_smoke.py` connects the actual C++ worker to the actual rustls entry/compositor, checking real pixels, input/settings, Unicode upload, audio on a private sink, reconnection, wrong pin/token and active-client revocation. Private WASAPI/null endpoints validate the audio transport without changing host defaults; physical audio timing/device behavior is separate.
- Existing Linux application/window/input/clipboard/tree/IME regressions, native GUI IPC and OLE file/tree flows pass. ARM64 and x64 native workers and self-contained WinUI packages cross-build; package checks verify PE architecture, manifest, resources and checksums.

The implementation does not activate a user systemd service or change the user's running desktop. `service/install-user-service.py` provides an optional deployment step. User logout persistence depends on deployment/logind settings. Revoked clients lose future RPC and active streaming access; already completed application launches are not undone. Paired clients are authorized as this Linux user across its Waybridge sessions, without a separate per-application permission model.

Remaining M0/M1 acceptance: actual WinUI startup/layout, Credential Manager/pickers/tray and native x64/WoA hardware paths; physical Media Foundation/Main10/HDR; real LAN/VPN failure/performance baselines. Also unverified are migration/concurrent access to existing wallet data, Flatpak document grants and arbitrary application wrappers. Portal ScreenCast, rich clipboard, microphone, pads/touch/pen, camera and printing remain later milestones. Monolithic entry points and GPU synchronization can be improved incrementally; the I/O changes do not claim a full architectural rewrite.

## Earlier validation — 2026-11-10

## C# / WinUI control center

The second physical report `/tmp/20261110-194814-fba4bdb9.zip` confirms the default PRI is now present and the theme file loads. The next failure is `AcrylicBackgroundFillColorDefaultBrush` while constructing `XamlControlsResources`. The programmatic Application had omitted control-library preactivation and the `IXamlMetadataProvider` contract normally emitted by the XAML toolchain. The bootstrap now calls `XamlControlsXamlMetaDataProvider.Initialize()` before loading styles and implements the metadata interface by forwarding all three methods to the SDK provider. It logs actual native type-resolution callbacks and successful resource merging. Build/package checks pass; physical startup after this bootstrap correction is still pending.

The physical WoA report `/tmp/20261110-191550-48f7a65a.zip` identifies the startup failure: .NET/WinRT/Application.Start succeed, then `XamlControlsResources` throws HRESULT `0x80004005` because it cannot locate `ms-appx:///Microsoft.UI.Xaml/Themes/themeresources.xaml`. All 512 original package checksums match. The self-contained package omitted its default `resources.pri` after PRI tooling was disabled for programmatic UI. A publish target now copies the complete Microsoft-generated index from the exact same version/RID framework MSIX, verifying the published WinUI/AppRuntime/MRT binaries and component PRI are byte-for-byte identical first. Native MRT Core under Wine successfully resolves both theme XBFs (645609 and 193991 bytes); their payloads exactly match the component's original embedded resources. Package verification now requires the default PRI. Physical WoA startup after applying this resource fix remains to be confirmed.

For the initial silent exit, added managed startup stage/exception/HRESULT logging and an ordinary Win32 failure dialog, plus `Diagnose-Startup.cmd`/`.ps1` to capture package checksums, OS/PE architecture, actual process exit code, .NET host tracing and matching Windows Application events. The ARM64 diagnostic bundle builds and passes ZIP/PE/manifest checks; BCL logger rotation/failure fallback and PowerShell syntax parsing pass. The cross-host manifest was also compared against the real Microsoft SDK `mt.exe` output under Wine and has the same XML element/attribute tree.

Added a .NET 10 / WinUI 3 control center with separate C++ streaming workers, saved profiles, OpenSSH session startup/attachment and forwarding, remote application launching, live stream/input settings, file transfer activity, tray lifecycle and actual worker diagnostics. Both ARM64 and x64 self-contained publishing succeeds on Linux. Packaging checks the GUI/worker/CoreCLR/WinUI/AppRuntime PE architectures, embedded WinRT/DPI manifest, runtime configuration and WinUI resources; ZIPs include the matching signed Microsoft VC++ redistributable and third-party notices.

Validation passed:

- `dotnet build` and the Core console tests: profile validation and atomic persistence, bounded UTF-8 IPC, strict SSH arguments/quoting, separate-process pipe bootstrap without token argv, graceful disconnect and automatic reconnect after worker death.
- `gui_ssh_smoke.py`: a private actual OpenSSH daemon, rejecting then accepting SHA256 host fingerprints, strict known-host reuse (including a Unicode/spaced known-host path), embedded helper RPC, authenticated TCP forwarding, literal Unicode application argv, session survival after detaching SSH and explicit shutdown.
- `gui_session_smoke.py`: actual compositor session creation/attachment/catalog/launch, descriptor permissions, credential omission from list/info, read-only enforcement, live FPS/bitrate validation, detach persistence, stale-PID rejection and cleanup.
- `gui_native_smoke.py`: real x64 C++ PE worker through a Win32 named pipe under isolated Wine/Xvfb, sanitized capabilities/status, actual settings acknowledgements, DPI/read-only changes, completed Unicode file upload, worker disconnect and retained remote session.
- `gui_codec_smoke.py`: real NVIDIA NVENC live FPS/bitrate changes and H.264 → PNG → HEVC → AV1 → H.264 switches, independently decodable new epochs/keyframes, correct color and exact alpha.
- Full existing Linux `scripts/test.sh`, strict Clippy, 19 Rust tests, Linux Release and native x64/ARM64 worker builds. Real PE Chrome tab split/merge/cancel, relative-pointer lock/release and OLE/IME regressions pass under private Wine desktops.

The WinUI shell itself has **not** been visually or interactively verified on physical Windows/WoA. The earlier Wine .NET loader failure was caused by disabling `mscoree` in the test harness; with that override removed, the minimal .NET 10 console probe succeeds. Actual WinUI now reaches `Application.Start` but Wine aborts in its unimplemented `ntdll.NtCreateWaitCompletionPacket` before dispatcher initialization, so it still cannot provide valid UI startup/layout evidence. Native tray/Credential Manager/pickers/audio-device enumeration and full SSH-to-GUI interaction still need the user's Windows device. The GUI uses real WinUI controls and compiled service implementations; no simulated telemetry or web preview is substituted for those device checks. Existing native ARM64 device results below refer to earlier streaming workers.

First-version limits: SSH authentication uses keys/agent (no interactive password UI); first-use host scanning needs direct connectivity or a preverified known-host entry for jump hosts. Transfers expose activity/completion, without progress/resume/cancel management. UDP/output-device changes require reconnect, and enabling server HDR/UDP/CUDA may require a newly created session. Tests use private sockets, keys, profiles and Wine prefixes; they do not restart the user's live service or modify wallet/Chrome data.

New test transcripts are `build/tests/gui-*.log`; portable bundles are `build/Waybridge-Control-win-{arm64,x64}.zip`. See [GUI usage](GUI.md) for first connection and build commands.

## X11 game input, Chrome tabs and host keyrings

X11 windows now share one stable RandR output; native Wayland per-window outputs are hidden from Xwayland. Individual stream resize/DPI changes no longer alter the shared monitor topology or application density. `x11_pointer_smoke.py` uses real X11 GLFW windows, checks actual cursor/framebuffer pixels at multiple points after resize and 175%/125% presentation changes, and asserts an identical single-monitor RandR layout while creating/closing the game and a native Wayland window. It negotiates and acknowledges a real pointer lock, closes the GLFW_CURSOR_DISABLED game, observes viewer unlock, and clicks the launcher again. This is a controlled MC-style regression; actual Minecraft menu/backpack and HMCL recovery still need device retesting.

The installed unforced `google-chrome-stable` (156.0.8078.47) selects native Wayland and explicitly requests ClientSide decorations. That request is now honored. `chrome_drag_smoke.py` uses a temporary two-tab profile and real viewer input to detach a tab, release outside all targets, then reattach it to the other tab strip. The complete x64 PE viewer under private Wine/Xvfb passes `windows_chrome_smoke.py`: no native caption on CSD, real SendInput drag across HWNDs, outside drop preserving the new window, merging through the overlapping dragged window, and Escape reverting detachment. Immediately after Escape it navigates through Ctrl+L/Unicode/Return without clicking or refocusing, verifying keyboard-focus recovery. Explicit X11 Chrome/XDND remains unsupported.

The private application D-Bus now relays only available host KWallet/Secret Service names. Each private caller gets a separate persistent host connection. The mock two-bus test verifies Unicode/raw bytes, FD round trips, 100 NoReply calls without consuming pending-reply slots, unique-owner calls, errors, sender-filtered unicast signals, and exclusion of an unrelated host GUI service. A separate test covers KDE4/5/6 backend hints for SSH shells missing KDE_SESSION_VERSION. No actual host wallet content, Chrome cookies or password records are read by these tests. Access to an existing unlocked keyring is implemented; restoration of the user's existing Chrome sign-in is not yet verified, and a locked wallet can still prompt on the Linux desktop.

Validation for this change passed:

- Full `scripts/test.sh`, including strict workspace Clippy, **19 Rust tests** (18 host + 1 codec), the new GLFW/Chrome tests, Qt/Dolphin/Ghostty focus/IME, desktop semantics, files and clipboard regressions.
- `x11_smoke.py` and `x11_clipboard_smoke.py`: GLX, independent native Wayland windows, XCB input/focus, Unicode/PNG/INCR clipboard and cleanup.
- `windows_chrome_smoke.py`, `windows_relative_smoke.py`, `windows_ime_keys_smoke.py --ole`: complete PE drag/cancel/focus, real native clipping/relative input and OLE/IME handoff regressions under private Wine desktops.
- Linux Release, x64 Windows and ARM64 Windows builds. The ARM64 artifact is confirmed as ARM64 PE; physical WoA execution of this revision remains pending.
- An isolated Release startup against the actual host bus exposes KWallet6/Secret Service owners on the private bus, restores KDE6 hints in the launch descriptor, excludes host GUI service owners, and exits/cleans up within five seconds. Only bus metadata was queried; no keyring API was called.

Transcripts and fixture screenshots are `build/tests/compat-*.log`, `x11-pointer-*.log`, `chrome-drag-*` and `windows-chrome-*`. Tests use temporary profiles/listeners and do not restart the user's service or modify the default Chrome profile. The earlier integration and physical checks below describe their own source checkpoints.

## Application fixes after physical WoA testing

The previous ordinary startup inherited the host's `QT_IM_MODULE=fcitx`/`GTK_IM_MODULE=fcitx`, omitted Xwayland/audio unless explicit flags were supplied, and routed only PulseAudio applications with `PULSE_SINK`. This prevented Dolphin's native text-input bridge, legacy GUI startup and mpv's default native PipeWire output. Startup now detects available Xwayland/private audio, isolates the application D-Bus session, removes host IM overrides and routes both PulseAudio and native PipeWire. `--launch -- <application>` restores that same environment for another SSH shell using a 0600 descriptor with process-identity checks and owned cleanup.

Executed Linux checks:

- `scripts/test.sh`: formatting, strict Clippy, 17 Rust tests and the full protocol/Qt/Dolphin/Ghostty/files/desktop suite passed, including the new inherited-fcitx environment regression.
- `launch_smoke.py`: real Dolphin enables text-input-v3, commits a Chinese directory path and immediately handles raw Backspace/Return. An external `--launch` process receives the identical display, D-Bus, IM and audio environment; stale descriptors cannot launch a command.
- `launch_smoke.py --apps`: installed Google Chrome, HMCL (Java/JavaFX) and VLC each create and paint remote windows. Chrome uses an isolated `--user-data-dir` and local test page; this does not promise reuse of the host browser's active profile. All test state is private, and the preexisting host Dolphin daemon/VLC process were retained.
- `launch_smoke.py --audio`: actual mpv chooses native PipeWire without an AO override, its paused stream targets the private sink with fallback forbidden, and streamed PCM reaches peak 12000. `audio_smoke.py` and `session_integration_smoke.py` pass isolation, authentication, flow control, shutdown and multi-backend checks. The host default output remains unchanged.
- A stronger `ghostty_smoke.py --ime` exposed a second input bug even when raw keys reached GTK: redundant non-null empty preedit updates left Ghostty composing and swallowed Backspace/Return. Active cancellations now use null preedit/cursor -1; redundant clears are skipped. The old Release fails the exact PTY test, while the fix receives committed Chinese followed immediately by DEL/CR with no refocus. Logs: `/tmp/waybridge-ghostty-key-{before,after}.log`.

Windows native MSVC ARM64 testing in the collaborating Windows session verified the real Microsoft Pinyin candidate UI, `ni` + Space committing “你”, immediate Backspace and raw Return without changing focus. It also found and fixed composition-end messages that do not arrive, zero-scan key events, translated control characters and Ctrl+H/I/M duplication. The full Wine IME/OLE regression, including the new cases, passes (`/tmp/waybridge-ime-keys-final.log`). `native_icon.h` no longer uses the RPC-macro-colliding member name `small`, and the native ARM64 build/shader checks pass.

The same Windows session confirmed actual mpv default PipeWire → private sink → PCM → native WoA WASAPI → default physical output. A quiet 997 Hz fixture produced loopback magnitude 0.01343 at 997 Hz and 0.0000565 at adjacent 2003 Hz; only frequency magnitudes were measured, no recording was saved. This is an audio/IME subset of physical validation, not certification of every integrated feature or the Linux cross-compiled artifact. The completed 49120 test service was stopped after the Windows operator finished; no user service or application was restarted.

Linux Release and cross-compiled Windows ARM64 builds passed (`/tmp/waybridge-fixes-{release,arm64}-final.log`); Linux application logs are under `build/tests/launch-*.log`. The earlier integration checkpoints remain below for traceability.

The source integrates all 14 feature implementations. Results below distinguish the feature/media checkpoints, the desktop+media combined session, the final files-inclusive rerun, and earlier physical-device work. Building an ARM64 executable or running a PE under Wine is not physical WoA validation.

## Current integration status

| Checkpoint | Result |
|---|---|
| Media integration (`2ca36dd`) | Passed Rust fmt, workspace strict Clippy, 7 tests, Linux build, x64/ARM64 clients and UDP probes; media/UDP/audio checks below |
| Desktop + media integration | Passed strict Clippy, 12 Rust tests (11 host + 1 codec), x64/ARM64 builds and combined-session regression |
| Full 14-feature merge (`f74d34a`) | All 14 feature refs are ancestors; 17 Rust tests (16 host + 1 codec), strict Clippy, full Linux suite, Windows desktop and seven files/IME/OLE suites, all 21 virtual-source cases and media matrix passed |
| Ordered video shutdown fix (`e0b0c6d`) | Strict Clippy/17 tests; live cached-NVENC SIGTERM regression, codec/GPU/HDR and combined-session checks passed; Linux Release rebuilt |
| OLE pointer-lock boundary (`91ef2d9`) | Actual PE test verifies preexisting 1×1 clip/hidden cursor, release before OLE and rejection of a late lock during OLE; the pre-fix client fails that negative control |
| Physical Windows/WoA execution of the complete integrated client | Pending |

The combined `python tests/session_integration_smoke.py` passed after the full file merge and again after the ordered video-shutdown fix, with **X11 + audio + UDP + CUDA + HDR policy enabled together**. It checks the unified desktop/file/IME capabilities, authenticates the independent PCM socket, keeps control ping responsive, displays native Wayland and GLX/X11 windows to a PNG-only peer, and verifies ordinary-TCP fallback when UDP was offered but never authenticated. SIGTERM exits successfully, removes the owned sink and private X socket, and leaves the host default sink unchanged. This test does not claim actual HDR output, UDP pixel delivery, file transfer execution, or every feature in one physical Windows session; separate tests below cover those implemented paths.

Cross-system integration preserves a single shutdown request path for Xwayland/audio cleanup, separate listener options/capabilities, per-window presentation credits, global UDP credits, independent audio credits, and PNG-to-video fresh epochs. `gpu_window_lifecycle_smoke.py` passes real CUDA/NVENC repeated same-size detach/remap with other-window rendering, new registrations, fresh decoded pixels/reference chains and no CPU fallback. `resize_smoke.py` now sends SIGTERM while the viewer, encoded window and cached sizes remain alive, and requires exit 0 within five seconds. This exposed a driver-finalization deadlock: the detached encoder worker was closing NVENC contexts concurrently with process exit. The fix clears/stops/wakes/joins the worker before App destroys graphics resources; the strengthened regression passes.

The final native files and media matrices used SHA-256-verified copies of the merged host/viewer in separate worktrees. They passed ordinary and directory clipboard/drop, full Qt/IME, synchronous/asynchronous native OLE and 21 virtual sources, plus PCM/WASAPI, all UDP media kinds, global flow/cancellation and codec fallback. Desktop cursor/icons/semantics/relative and `windows_smoke.py --x11 --udp --qt` also passed. Their source checkpoint is `f74d34a`; subsequent shutdown and OLE-boundary corrections receive targeted regression below, rather than being represented as physical-device runs.

The final root GPU run after `e0b0c6d` passed H.264/HEVC/AV1 reference/alpha/epoch/backpressure checks, CUDA pixel and fault-recovery oracles, FP16 mixed-SDR composition, and both Main10 codecs with 15 frames at 960×640 and 277 distinct decoded luma values. Actual Windows PE software-D3D11 shader math produced 1006 distinct red values and exact FP16 alpha; WIC preserved 1024 raw-half alpha values. These precision checks remain separate from physical HDR/MFT support. Root transcripts are `/tmp/waybridge-merge-all-linux-*.log`, `/tmp/waybridge-merge-all-linux-final.log` and `/tmp/waybridge-merge-all-final-gpu-*.log`; native matrices are in the validation worktrees' `build/tests/final-*.log` and `/tmp/waybridge-final-media-*.log`.

`windows_ime_keys_smoke.py` includes an OLE-only mode (`--ole`). Its persistent native observer verifies that a preexisting pointer lock is genuinely active, that starting OLE restores the full clip and visible cursor, and that a later lock is rejected even after source focus is restored while the OLE loop pumps messages. The pre-fix frozen client accepts the late generation and fails the test. The guard is combined with IME cancellation/key release, server-side outbound-drag lock exclusion and the deterministic stale-owner-MOUSELEAVE popup regression. Linux Release and both Windows architectures have been rebuilt after these code changes; only Windows system DLL imports are present.

Final rebuilt root artifacts after `91ef2d9` passed the full native `windows_ime_keys_smoke.py`, `windows_relative_smoke.py`, `windows_smoke.py --x11 --udp --qt` and `windows_drag_out_smoke.py --tree` sequentially. These cover both the shutdown and OLE corrections in the delivered host/client combination. Logs: `/tmp/waybridge-merge-all-final-native-00.log` through `03.log`. PE headers confirm x64 `0x8664` and ARM64 `0xaa64`; ARM64 runtime testing remains a physical-device task.

## Media integration: executed results

- `cargo fmt --all --check`, `cargo clippy --workspace --all-targets --locked -- -D warnings`, `cargo test --workspace --locked` and Linux debug build passed. The vendored Smithay dependency emits four existing warnings; the project strict checks pass. Both Windows client architectures and the standalone UDP probes build; PE inspection confirms x86-64 and ARM64 respectively.
- Extended `tests/windows_udp_smoke.py` uses the production Windows CNG/UDP receiver under a private Wine/Xvfb desktop. PNG kind1, H.264 kind2, HEVC kind4, AV1 kind5, HEVC Main10 kind7 and AV1 Main10 kind8 each delivered 24 strictly ordered frames under the applicable loss/corruption/replay/reordering scenarios. A blackholed H.264 stream recovered through TCP repair. Actual FFmpeg decoding verified SDR color and exact independent alpha; Main10 verified ten-bit PQ/BT.2020 and raw-half alpha16. The HDR fixture's inherited-stdin EOF was corrected before the HDR rerun; it was a harness issue, not a production media fix.
- `tests/udp_flow_smoke.py` passed actual CNG head repair with all 16 future slots occupied; 20 real Wayland windows/40 ordered PNG frames with a 600 ms first-NACK delay enforced the global eight-message gate and resumed after ACK. A noisy 1 Mbit/s transfer stopped on TCP disconnect, with zero datagrams after a 100 ms grace over two seconds. The 64 MiB credit accounting, repair priority and per-window presentation-credit separation remain in the implementation.
- `tests/gpu_smoke.py --binary target/debug/waybridge` passed real GL renderbuffer/CUDA registration, every-pixel unpremultiply/flip/alpha oracle, opaque alpha-readback omission, actual native H.264/HEVC/AV1 GPU inputs, and an injected fourth-frame GPU encode rejection. The current PNG remains exact, a fresh CPU capture occurs without application repaint, and CPU NVENC resumes with a new independent epoch.
- `tests/hdr_video_smoke.py` passed the real FP16/CUDA P010 luma/chroma/alpha oracle, both Main10 codecs with 15-frame IDR/P chains, two-credit pause/resume, odd-size/cache epochs, wrong-codec rejection, SDR monitor fallback and PNG-to-HDR independent-key recovery. The integration rerun used 256×256/257×257 fixtures, producing 149/150 decoded distinct luma codes; those widths cannot prove more than 256 levels. The earlier HDR checkpoint's separate 960×640 runs retained 277 levels. Non-negotiated HDR and injected HDR encode failure/accurate tone-map recovery passed.
- `tests/audio_smoke.py` passed private-session capture, unrelated-sink isolation, authentication/duplicate/legacy gating, four outstanding block credits, future-ACK rejection, independent controls, audio-only disconnection and cleanup. `tests/windows_audio_smoke.py` ran the actual client/WASAPI into an explicitly selected private null endpoint and recovered stereo peaks L=12000/R=6000. No default output device was changed and no generated sound was routed to the user's normal endpoint.
- `tests/windows_codecs_smoke.py` passed actual production-client rejection of unadvertised SDR/HDR kinds 4/5/7/8, codec-bound fallback and subsequent correct PNG painting. Wine advertises no HEVC/AV1 hardware path; the test deliberately does not claim one.
- `WAYBRIDGE_TEST_CODEC=av1-nvenc python tests/windows_smoke.py --udp` passed the actual native viewer's encrypted-UDP receive/paint path, native resize, Unicode clipboard, bidirectional file clipboard, OLE incoming drop and single decoration ownership. Wine capability-gated video to PNG, so this is not real Windows AV1 decode coverage.

Media integration logs were written to `/tmp/waybridge-merge-media-*.log`; detailed artifacts are under that integration worktree's `waybridge/build/tests/` and `build/diagnostics/`. Artifacts/logs are not guaranteed to be versioned or copied to the root build directory. Tests used private ports/sockets/prefixes/Xvfb/null sinks and did not restart the existing 49090 service, change display settings or terminate unrelated GPU processes.

## Desktop and files: established feature checks

These are results established by the feature/integration checkpoints. The final aggregate rerun is tracked above; the table is not a claim that each test was just rerun against the final root tree.

| Scope | Tested behavior and entry points |
|---|---|
| Base windows/input/DPI/popups | `smoke.py`, `decorations_smoke.py`, `qt_smoke.py`, `dolphin_smoke.py`, `ghostty_smoke.py`, `windows_smoke.py`: real Wayland/Qt/GTK, 175%→125% scaling, exact alpha, surface-local hit testing, independent overflowing/nested popups, native ownership, clipboard/files, disconnect/reconnect |
| Windows IME | `ime_smoke.py`, `windows_ime_keys_smoke.py`, Ghostty `--ime`: text-input-v3 preedit/commit/cancel, Chinese/emoji, focus generation, native Unicode injection, shortcuts/Ctrl+Space and modifier recovery. This is not execution of Microsoft Pinyin's real candidate engine |
| Cursor | `cursor_smoke.py`, `windows_cursor_smoke.py`: named/hidden/custom/animated cursors, DPI/hotspots, malformed/oversize/stale rejection, animation without main-window damage, non-client sizing cursors and replacement resource bounds |
| Application icons | `app_icon_smoke.py`, `windows_app_icon_smoke.py`: XDG PNG/SVG resolution, identity/revision safety, fallback, per-DPI native HICONs and cleanup |
| Window semantics | `window_semantics_smoke.py`, `windows_semantics_smoke.py`: genuine modal hint versus parent, owner lifetime, min/max, fullscreen epoch/restore, unmap/remap, suspended hidden windows and input-region operations. HTTRANSPARENT covers only the same UI thread |
| Relative pointer | `relative_smoke.py`, `windows_relative_smoke.py`: real locked-pointer/Raw Input handshake, generation guards, clipping, focus/menu/native failure/emergency cleanup and no immediate retrapping |
| X11 | `x11_smoke.py`, `x11_clipboard_smoke.py`: GLX animation, 175% Qt/XCB, alpha, transient/override-redirect/nested menus and Tooltip, parent move/reconnect, native Wayland coexistence, UTF-8/PNG/URI/INCR clipboard and owned socket/lock cleanup |
| Linux native drag-out | `drag_out_smoke.py`, `windows_drag_out_smoke.py`: real Wayland grab→native DoDragDrop, real-button handoff, COPY, lazy FILECONTENTS/IStream reads/Seek/Clone, synchronous/asynchronous completion, cancellation/disconnect; source files and clipboard preserved |
| Directory transfer | `tree_smoke.py`, `windows_drag_out_smoke.py --tree`: four clipboard/drop directions, manifests, nested/empty directories, Unicode/zero-length bytes, source identity/path limits, collision/link/reparse rejection and incomplete staging cleanup |
| Windows virtual-source import | `windows_virtual_smoke.py`: cross-process STA providers without CF_HDROP, FILEDESCRIPTORW/A and FILECONTENTS streams/HGLOBAL, descriptor indices, synthesized parents, sequential/Clone/Seek/Stat, sync/async completion, errors/E_PENDING, cancellation and disconnect. Successful Linux paths are published only after full commit |

Smithay patches remain local under `vendor/smithay`, with sources/licenses in `vendor/smithay/PATCHES.md`; no system Cargo cache or host compositor is modified. New window semantics/cursors/relative input/IME/OLE combinations need continued physical-device testing even when the relevant Linux/PE API fixture passes.

## Earlier physical Windows ARM64 baseline

An earlier native MSVC ARM64 build ran on Snapdragon 8cx Gen 3 / Adreno 690 (driver 30.0.4035.6900). Removing H.264 filler NAL12 eliminated the final-picture hold: all 120 native 1280×720 access units produced sequenced GPU textures, including the last frame. The decoder probe's mean transform time was 1.139 ms, with a 22.910 ms cold maximum. WIC tests verified exact alpha/premultiplied pixels and faster image/alpha decoding than the earlier GDI+ path.

That baseline full client retained hardware H.264 on static translucent and animated fixtures. Bounded decoder/encoder caches were checked over repeated resize epochs; the native GPU hash probe matched fresh decoders on all 33 frames. Earlier warm maximize/restore `resize_to_present` samples averaged about 98.72 ms with the cached server versus 284.35 ms previously, while a first cold large size still took about 305 ms. These are fixture/stage/presentation-submission timings, not input-to-photon or Moonlight comparisons. Historical artifacts include `build/diagnostics/decode-native-120-fixed.log`, `image-bench.log`, `resize-ui-results.json` and adjacent client logs when retained.

This physical evidence applies to that earlier H.264/PNG/resize baseline. It does not establish physical execution of the final integrated binary, the Linux cross-built ARM64 executable, HEVC/AV1/Main10 decoders, HDR displays or Microsoft Pinyin. Linux LLVM-MinGW cross-builds confirm architecture and Windows system imports, not runtime hardware compatibility.

## Window activation and modal focus (2026-11-16)

- `tests/activation_smoke.py` passed against a real compositor and Wayland
  fixture on a disposable user bus: standard xdg-activation, recent serial,
  consumed/invalid-token rejection, stale window focus rejection, explicit launch
  binding, and connection invalidation.
- `tests/desktop_overrides_smoke.py` passed real GIO raw argv/desktop launches
  and D-Bus Activate/Open/ActivateAction, including token environment/platform
  data and application PID reporting through the standard launched signal.
- `tests/windows_semantics_smoke.py` passed the real Windows viewer and Wayland
  modal fixture under private Wine/Xvfb, including new assertions that modal
  unmap/remap and final destruction leave the correct parent in the foreground
  with another independent application window present.
- `tests/windows_activation_probe.cpp` ran on laptop as a limited interactive
  user and exited 0: explicit first-frame activation, consumed intent rejection,
  newer user input invalidation, background parent refusal, owner restoration
  before modal hide/destroy and connection reset. It creates three private test
  windows, verifies that any test click hits its own HWND, then restores the
  previous cursor/foreground and removes its temporary task/files. Run with
  `python3 tests/run_windows_focus_probe.py --host laptop` after compiling it.
- Rust Clippy with warnings denied, the x64 native viewer, WinUI GUI and Core
  controller tests passed. The native hosted IPC regression also exercises
  activation negotiation, prepare/token response and launch/PID forwarding.
- `tests/windows_gui_focus_laptop.py --host laptop` passed as a limited
  interactive user with `Waybridge.GuiFocus.Probe`. This separately named
  executable compiles the production `MainWindow`, application bootstrap and
  controller sources, injecting only a temporary state directory. Actual mouse
  clicks on Connect and Launch drive the real SSH management/GIO and native
  worker path. `GetForegroundWindow`, the exact worker PID and distinct Linux
  application PIDs verify two new launches (including an already running app of
  the same desktop ID), the standard Qt owned modal and return to its exact
  parent after Escape while another application window remains alive.
  Only the test GUI is temporarily topmost around its own verified click to
  avoid an unrelated topmost terminal; remote application HWNDs use the normal
  product activation policy. The private SSH daemon, disposable bus/HOME,
  profile/key/known-hosts, scheduled task and notification registration are
  isolated from the installed GUI and removed afterwards. Cursor/foreground are
  restored. Build/run:

  ```sh
  dotnet publish gui/Waybridge.GuiFocus.Probe -c Release -r win-x64 -p:Platform=x64 --self-contained true -p:WindowsAppSDKSelfContained=true -p:MicrosoftWindowsAppSDKPackageDir= -p:ManifestTool="$PWD/scripts/merge-windows-manifest.py" -o build/gui-focus-probe-x64
  python3 tests/windows_gui_focus_laptop.py --host laptop
  ```

  This test found that embedding the growing management script in `ssh.exe`
  argv exceeded Windows' 32,767-character process command-line limit. The
  production agent now uses a constant-size Python loader and a bounded base64
  stdin bootstrap before the existing JSON RPC frames. Core and real private
  OpenSSH regressions passed, then the complete physical GUI test passed.

These fixture checks do not certify every application's activation
implementation. Legacy toolkit startup metadata, delayed/daemonized application
launches and additional Windows foreground-lock policies remain compatibility
checks. Background requests intentionally may flash instead of taking focus.

## Remaining device and compatibility validation

- Real Windows/WoA HEVC/AV1 and Main10 MFT availability, correct GPU planes/textures, DirectComposition FP16 HDR presentation, active HDR↔SDR transitions, per-pixel alpha and multi-monitor DPI. The production HDR shader/WIC numerical checks under private software D3D11 verify arithmetic only; WARP is not advertised as HDR hardware.
- Further Microsoft Pinyin candidate positioning, cancellation and Win+Space/shortcut switching across Qt/GTK on the physical device, beyond the basic Qt candidate/commit/Backspace/Return test above. XIM and surrounding-text reconversion are not implemented.
- Windows Explorer/desktop and third-party OLE targets; Outlook/archive/cloud providers, real COM cancellation and provider hangs. No FUSE, IStorage, unknown-size HGLOBAL, move semantics or general third-party compatibility claim. An unresponsive provider that ignores OS cancellation may require source-process recovery.
- Cross-process transparent click-through and KDE blur are not implemented. Relative-only/unlocked motion, confined-pointer and popup locks remain unsupported. X11 lacks PRIMARY, XDND, XIM, full EWMH and automatic crash restart.
- General multi-GPU/driver/application coverage, congestion adaptation, endpoint roaming, Opus, microphone, strict A/V synchronization and device hot-switching remain outside this implementation. CUDA retains GPU copies/synchronous waits and independent alpha readback; it is not an entirely zero-copy pipeline.
- Actual input-to-display latency, high-refresh-rate/bandwidth behavior and comparative latency against Moonlight remain unmeasured. Sunshine/Moonlight source trees are unchanged references; their wire protocol is not used.

## M2 read-only file delivery — 2026-11-16

Linux now publishes completed legacy uploads, directory uploads and virtual-file
imports through a session-owned RO FUSE view over their already-complete private
cache. The view advertises ordinary 0600/0700 modes; kernel mount protection
rejects file writes, creation, unlink and replacement without making saved copies
read-only. Mount setup is lazy: unavailable FUSE reports a transfer error without
stopping window streaming or exposing a writable fallback. Completed backing paths survive viewer disconnect; reads open and release
file descriptors for each request, rather than retaining one per cached file. Reused protocol IDs after reconnect receive
fresh cache directories rather than overwrite an old successful delivery.

Verified on this Linux host:

- `cargo clippy --all-targets --locked -- -D warnings`, workspace tests and normal
  build pass (pre-existing warnings in vendored Smithay remain dependency warnings).
- Explicit `cargo test --locked readonly_delivery -- --ignored` uses a real FUSE
  mount. Actual overwrite/create/unlink return EROFS; rename replacement fails;
  `cp -a` produces writable independent files/directories, preserves empty
  directories and leaves delivered content unchanged. Failed publication rolls
  back staged nodes and open descriptors, including multi-directory commits;
  duplicate published names cannot overwrite an old view.
- `tests/tree_smoke.py` and `tests/transfer_worker_smoke.py` exercise real transfer
  commits, cancellations, disk stalls, viewer disconnect/reconnect and reused
  protocol IDs. Old URIs remain readable, saved copies remain editable, and only
  the canceled task's own staging is removed.
- `tests/dolphin_smoke.py` pastes an actual received directory through Dolphin/KIO
  into the chosen target directory; its files and empty-directory descendants
  accept writes and the delivered original stays unchanged.

Windows materialized clipboard caches deny write/delete and directory child
mutation through DACLs, with no DOS READONLY attribute. Filesystems that synthesize
READONLY from these ACLs are explicitly rejected and cleaned before CF_HDROP
publication. Windows OLE virtual streams remain read-only and reject both Write
and SetSize; saved stream content is an ordinary destination file.

Real Windows 11 on `laptop` passed both the standalone
`waybridge-readonly-probe.exe` and the complete `windows_tree_probe.exe`:

- Direct file write, `MoveFileEx(MOVEFILE_REPLACE_EXISTING)`, deletion and directory
  mutation are refused; CopyFile and SHFileOperation directory copies are writable,
  retain empty directories and leave original content unchanged.
- Full native tree validation, synchronous/worker staging, clipboard publication,
  cancellation, supersession, late-completion rejection and committed-cache
  lifetime pass.
- OpenSSH initially supplied the standalone probe an administrative token with
  backup/restore privileges enabled. Such a token can bypass normal filesystem
  ACL checks. The probe disables privileges **only in its own process token** to
  validate normal desktop application permissions. This is not an administrator
  or same-UID sandbox and does not resist deliberately changing ACLs or bypassing
  the public delivery path.
- Only uniquely named test files/directories in the remote user's Temp and the
  uploaded test executables/scripts were created; those objects were removed.
  No deployed binary, desktop service, session or wallet was changed.

On Wine, DACLs map to Unix permissions and READONLY attributes, which would make
ordinary copies read-only. Tests explicitly require the unsupported error,
absence of CF_HDROP and cleanup of partially protected staging; they do not
silently accept writable delivery or report NTFS behavior as Wine coverage.
`tests/windows_smoke.py` and `tests/windows_drag_out_smoke.py --tree` pass this
refusal path, Windows→Linux uploads, and actual OLE virtual stream reads including
Write/SetSize rejection. Both x64 and ARM64 clients compile successfully.

Focused repeatable commands (run from repository root):

```bash
cargo test --workspace --locked
cargo test --locked readonly_delivery -- --ignored
cargo build --locked
python3 tests/session_fixture.py python3 tests/tree_smoke.py
python3 tests/session_fixture.py python3 tests/transfer_worker_smoke.py
GVFS_DISABLE_FUSE=1 python3 tests/session_fixture.py python3 tests/dolphin_smoke.py
GVFS_DISABLE_FUSE=1 python3 tests/session_fixture.py python3 tests/windows_smoke.py
GVFS_DISABLE_FUSE=1 python3 tests/session_fixture.py python3 tests/windows_drag_out_smoke.py --tree
cmake --build build/windows --target waybridge-readonly-probe
scripts/build-windows-arm64.sh
```

The standalone Windows probe uses its own Temp directory and may be run directly
on Windows. On Wine, pass `--expect-unsupported` to require the error/cleanup path.
`windows_tree_probe.exe --expect-unsupported` additionally asserts that neither
synchronous nor worker receive publication leaks CF_HDROP or staging.

Read-only delivery descriptor regression: four completed batches totaling 1,024
files retain bounded process FD usage (within eight descriptors of baseline).
The real FUSE test randomly reopens/range-reads files across all batches 128 times
and repeatedly reads an old URI, while publication failure still rolls back nodes
and descriptors. Backing files remain complete local caches; this is not network
lazy reading or a new filesystem protocol.

## M2 standard notifications (2026-11-16)

The notification branch was checked with isolated buses/runtimes and a separately
registered Windows probe; no real wallet, application profile, notification
provider or installed Waybridge process was replaced for these tests.

- `cargo check/build --bin waybridge` and Windows x64 native worker / WinUI GUI
  builds passed. The standalone Windows notification probe cross-published for
  x64 and ARM64; ARM64 publication is compilation evidence only.
- `/usr/bin/python3 tests/notifications_smoke.py` passed standard D-Bus methods,
  introspectable signals, Chinese text, raw icon data, text/action limits,
  replacement, stale-revision rejection, originating owner isolation, expiry,
  application close, nonexistent-ID error, disconnect reset/offline refusal,
  non-replacing provider conflict and absence of notification text in logs.
- `/usr/bin/python3 tests/notifications_bridge_smoke.py` passed a real compositor
  with a disposable user bus: notifications_v1 transport/action, explicit
  provider status, live sharing switch, server-side read-only action rejection,
  disconnect reset, provider heartbeat loss and shared-bus owner conflict without
  takeover. No host desktop activation environment was modified.
- `/usr/bin/python3 tests/native_notifications_smoke.py` exercised the real PE
  worker under a private Wine/Xvfb prefix: GUI-readiness handshake, host-pipe
  notification/action round trip, Chinese metadata, placeholder and unbadged
  application PNG, replacement revision rejection, native read-only gating, live opt-out,
  no body logging and clean worker teardown. This is IPC/protocol evidence,
  not Wine execution of Windows AppNotifications.
- `tests/windows_host_control_probe.cpp` includes an actual unread pipe and
  blocked native writer: filling the 128-event queue must post WM_CLOSE to its
  owner, and stop must cancel the outstanding read/write within two seconds.
- The isolated `Waybridge.Notifications.Probe` ran on **laptop, Windows x64**, in
  the logged-in user's limited interactive scheduled task and exited 0. Actual
  AppNotificationManager Show/GetAll confirmed native notification-center items;
  the same Tag/Group replacement left exactly one item. It exercised the native
  INotificationActivationCallback interface → NotificationInvoked → production
  DesktopNotifications helper → bound IPC feedback, rejected a replaced token,
  and verified application close, notification-center removal feedback and
  disconnect cleanup. The callback test directly queries the registered native
  manager interface; it does **not** claim an automated Windows Shell banner
  click/COM cold-start test. The probe uses a random AppID, its own executable
  identity, no profile store, and removes its own registration/toasts.

Real registration testing exposed the SDK's omitted
`Microsoft.WindowsAppRuntime.Insights.Resource.dll`, matching upstream
[WindowsAppSDK #6071](https://github.com/microsoft/WindowsAppSDK/issues/6071).
The shared publish task now copies that signed DLL from the exact 1.8.260921001
framework MSIX after byte-validating the existing runtime components, rejects
an inconsistent pre-existing DLL and leaves the NuGet cache untouched. The
probe passed with registration required after that packaging repair; successful
Show/GetAll with failed registration is not accepted as notification availability.

Reproduce the Windows probe after publishing it (choose x64/ARM64 for the target):

```sh
dotnet publish gui/Waybridge.Notifications.Probe/Waybridge.Notifications.Probe.csproj \
  -c Release -r win-x64 --self-contained true \
  -p:MicrosoftWindowsAppSDKPackageDir= \
  -p:ManifestTool="$PWD/scripts/merge-windows-manifest.py" \
  -o build/notifications-probe-x64
python3 scripts/test-notifications-laptop.py --host laptop
```

The runner uploads to a unique temporary directory, creates a unique limited
interactive task and removes its own task/files/processes afterwards. It must
run with a logged-in Windows desktop; elevated SSH sessions themselves cannot
exercise AppNotifications. Current Windows policy/DND settings are not changed.
Physical banner click/foreground routing, Do Not Disturb visual behavior,
multi-monitor/DPI icon appearance, broader application compatibility and real
Windows ARM64 runtime coverage remain device checks. Windows center-dismissal
feedback is intentionally delayed by the two-second polling/activation grace.


## Runtime window icon sources — 2026-11-16

Window icon lookup previously treated Wayland app_id and X11 WM_CLASS as an exact
desktop-file ID and ignored runtime window icons. Read-only inspection of the
installed desktop entries showed `hmcl-bin.desktop` uses
`StartupWMClass=org.jackhuang.hmcl.Launcher`, and `qq.desktop` uses
`StartupWMClass=QQ`. These association fields were unused; a correct independent
SNI tray icon did not repair the window icon. The implementation now uses generic
StartupWMClass/class/instance matching and runtime image sources, with no HMCL/QQ
name rules.

`tests/window_icon_smoke.py` runs real Wayland and X11 fixture clients against an
isolated compositor. It verifies:

- Wayland pixel/name priority, straight PNG output from premultiplied SHM alpha,
  pending versus committed state, immediate icon/buffer destruction, reset/empty
  and oversized fallback, app_id class/case compatibility, delayed stale desktop lookup rejection and reconnect.
- X11 `_NET_WM_ICON` initial values, updates and deletion, malformed/oversized
  fallback, runtime icons without WM_CLASS, and class/instance/case matching.
  A grabbed private X server cannot stall compositor ping; subsequent icon updates
  still work. A socket-pair X11 server that never answers setup separately proves
  the worker deadline fires. StartupWMClass wins over a
  coincidental generic desktop ID; user desktop entries
  retain priority and Hidden masks the same system entry.
- Invalid buffer destruction while its icon remains alive disconnects that client
  while the compositor survives. Legal destruction after icon destruction remains
  valid. This caught a vendored Smithay lifetime bug: a cached icon kept userdata
  alive and its buffer destruction hook unconditionally posted `no_buffer` after
  the protocol icon destructor. The hook now checks resource liveness and the
  destructor removes its hooks immediately.

Runtime SHM is copied once into an owned, bounded pending snapshot before client
objects disappear; protocol resources/SHM pools are not retained by the snapshot.
X11 property requests and all image scaling/PNG encoding run on the bounded icon
worker. Old source revisions and closed windows reject asynchronous results.

The new protocol test, existing `app_icon_smoke.py`, and accelerated GLX
`x11_smoke.py` pass. Rust formatting, clippy and build pass. These are protocol and
association results; no user application profile, wallet or real desktop service
was modified, and HMCL/QQ visual acceptance on Windows remains an integration
check after installing the fix.

## DND source acceptance and completion (2026-11-16)

Source completion is also verified separately. The enhanced
`tests/windows_data_drag_smoke.py` checks the source's `QDrag::exec` result,
in addition to the Win32 target receiving the text: outbound delivery returns
Copy (1), while the existing native/returned Qt drags retain Move (2). Before
the fix, the same real OLE delivery and `dnd_finished` returned Ignore (0).
The missing standard `wl_data_source.target` MIME feedback kept the source
toolkit's acceptance state false despite a Copy action and finished event.

`tests/dnd_source_feedback_smoke.py` passed with two independent native Wayland
clients on a disposable bus. It checks valid/nil/unsupported MIME feedback,
hover leave reset, late Accept/SetActions on departed and completed offers,
successful Copy completion and rejection of a fresh drag. The completed source
is deliberately kept alive so bad late events remain observable. Outbound
range-read/cancellation/disconnect and Qt file MOVE/return regressions, plus
strict Rust Clippy, passed. These checks use the protocol for all applications;
there is no Qt-specific production rule.

## Generic DND icon feedback (2026-11-16)

- `tests/drag_visual_smoke.py` passed a real Wayland `dnd_icon` surface and SHM
  animation against an isolated compositor/user bus. It checks exact premultiplied
  RGBA/transparent pixels, attach offsets, increasing commit revisions, animated
  frame callbacks while the application stream is limited to 1 FPS, drop/icon
  destruction cleanup, oversized-source rejection without starving callbacks,
  and absence of visual messages when data_drag_v1 is not negotiated.
- The production `client/drag_visual.h` passed the isolated
  `tests/windows_drag_visual_probe.cpp` under Wine and on laptop Windows x64.
  A separate child-process test window receives actual clicks through the
  topmost ghost; foreground remains on that underlay, the ghost follows pointer
  movement, ended/old generations are rejected and reset destroys it. On laptop,
  screen pixel readback also confirms 50% premultiplied red correctly composited
  over the probe's blue underlay. No user application/data is involved.
- The Windows probe runner creates a unique temporary executable/task, restores
  the previous cursor/foreground and removes its own files/processes. Compile
  the probe with `-municode -Iclient -lgdi32`, then run
  `python3 tests/run_windows_focus_probe.py --host laptop --visual`.
- Native x64 compilation and strict Rust Clippy passed. DPI/bounds unit coverage
  includes asymmetric edge/area limits and their rounding margin. This does not
  claim every toolkit supplies a DND icon, nor physical ARM64/mixed-DPI monitor
  testing of the new ghost; clients without an icon simply show the normal cursor.

## DND offer identity and text-target reentry — 2026-11-16

`tests/data_drag_offer_smoke.py` uses two real Wayland targets which deliberately
retain offers. An inactive offer from target A cannot change target B's current
acceptance through late Accept or SetActions. A separate source is dropped but
not read/finished until another source starts: the old Receive returns the old
Unicode payload, and its Finish leaves the newer source able to Drop, Receive
and Finish its own payload. This verifies source cookies through the wire and
checks active-offer status independently of the per-source cookie.

Native text targets now acquire a generation before calling external OLE
providers. Window lifetime, policy and generation are rechecked after GetData
and ReleaseStgMedium. Older DragEnter completion cannot overwrite state created
by a nested enter; an old leave cannot cancel the new target. Sharing/read-only
reset invalidates pending calls even when the window stays alive or the setting
is later reenabled. Drop revokes the target lease.

`tests/windows_text_drag_lifetime_smoke.py` passes a standalone PE probe through
actual IDataObject QueryGetData/GetData and STGMEDIUM release callbacks which
reenter the adapter. It verifies replacement survives all three callback points,
leave/policy/drop rejection, valid Unicode delivery, malformed UTF-16,
unterminated HGLOBAL and UTF-8 expansion limits. The probe uses disposable Wine
state, creates no windows and touches no clipboard or user data.
