# Waybridge 0.3.0 — Usage, Architecture Summary, and Build Guide

For a step-by-step Linux host installation tutorial (distro packages, `systemd --user`, `linger`, firewall, Portals, and troubleshooting), see [Linux Host Setup Guide](LINUX_SETUP.md). For Linux TOML configuration, validation, Waybridge-specific `.desktop` appends/overrides, and working-directory rules, see [Configuration and Application Launching](CONFIGURATION.md). For wallet, Portal, and systemd session lifecycle setup, see [Desktop Components](INTEGRATION.md). For design rationale, see [Configuration Policy](CONFIGURATION_POLICY.md).

The first M2 wave adds read-only cross-machine file delivery, SNI application trays/background entries, native notifications, and source icon badges. Notifications require explicitly opting into the provider per [Desktop Integration](INTEGRATION.md#explicitly-opting-into-the-notification-provider); tray scope is documented in [DESKTOP-TRAY.md](DESKTOP-TRAY.md). Update both ends together; active deployments and full hardware verification are tracked separately from source progress.

Waybridge is a Linux → Windows standalone remote application windowing system. The server is a private headless Smithay Wayland compositor with optional rootless Xwayland—it neither attaches to KWin nor migrates existing host desktop windows. The Windows client is native Win32 with x64 and native ARM64 builds.

This document lives in `docs/`, and all build and run commands below execute from the Git repository root. Layout: `src/` (Linux compositor), `native/` (codecs & GPU interop), `config/` (shared Linux config crate), `service/` (TLS user service & TUI), `client/` (Windows native C++ engine), `gui/` (C# / WinUI 3 Control Center), `integration/` (systemd targets & Portal preset), `scripts/` (build/packaging helpers), and `tests/` (verification suite). Build outputs write into Git-ignored `target/`, `build/`, and .NET `bin/`/`obj/`.

See [FEATURES.md](FEATURES.md) for capabilities and target scope, [ARCHITECTURE.md](ARCHITECTURE.md) for technical boundaries, and [ROADMAP.md](ROADMAP.md) for milestones. Sessions follow [Session Policy](SESSION_POLICY.md) by default: shared user data with a one-way check requiring logout from any same-UID local desktop before creating a new session (reconnecting to an existing session is allowed).

See [DESKTOP_INTERACTIONS.md](DESKTOP_INTERACTIONS.md) for the window, tray, focus, and drag-and-drop support matrix, and [DRAG_AND_DROP.md](DRAG_AND_DROP.md) for plain-text drag state, protocol adaptation, timeout bounds, and real Chrome testing.

Waybridge uses an independent v3 wire protocol—standard Moonlight clients cannot connect.

Every remote toplevel maps to an independent native Windows window with title bars owned by exactly one side; overflowing or nested menus map to independent owned popups without resizing their parent. Rendering follows Windows DPI, handling Qt fractional scaling, transparent alpha, parent/child dialogs, and native input separately. Disconnects keep remote apps running and reconnects restore live windows; keys, IME compositions, and pointer locks from prior focus never leak onto new targets.

The default `--codec auto` selects NVENC H.264 for SDR and HEVC when HDR preference is enabled, with PNG for menus (actual paths depend on capability negotiation). Optional HEVC/AV1 preserves independent lossless alpha. Windows probes real hardware support before using Media Foundation / D3D11 decoding and DirectComposition, falling back per window to PNG if unavailable or on runtime error. `--codec png` forces lossless frames. Default is 60 FPS with raw-update coalescing, ordered encoded reference frames, and at most 2 in-flight presentation-ACK frames per window. Pre-capture checks downstream credits and raw queue capacity, keeping windows marked dirty under congestion and resending once capacity recovers; `--stats` reports stage percentiles, frame age, and queue diagnostics.

Control/files, media, and optional audio use independently authenticated connections carried over the same TLS entry point or SSH forwarded port. Optional UDP uses an additional directly reachable port while preserving TCP control and frame repair. Encoding, decoding, and disk writes on both sides run on worker threads, and Windows input sends use a bounded ordered queue. Negotiated peers offload file bytes to the dedicated file data channel, and UDP uses delivery-feedback adaptive pacing plus NVENC bitrate adaptation (see [Network Experience](NETWORK.md) and [File Transfers](FILE_TRANSFERS.md)). Common window sizes use bounded encoder/decoder caches while still enforcing fresh epochs and IDR keyframes on reuse; brand-new sizes, resource limits, and synchronous readbacks can still introduce occasional stalls.

**Update and restart both ends together before launching remote applications.** The wire protocol remains independent v3 (incompatible with standard Moonlight). Windows 10/11 is required; earlier WoA H.264/alpha tests do not substitute for full on-device verification of newer features. Waybridge remains an active development prototype, and HDR is an explicitly scoped scRGB subset.

## Windows GUI Control Center

The **C# / WinUI 3 Control Center** ships as portable native ARM64 and x64 packages. It handles Waybridge TLS pairing, connections, session management, app launching, display/input/sharing settings, file transfer activity, trays, and diagnostics while delegating video and remote windows to the standalone C++ worker.

Extract the full `build/Waybridge-Control-win-{arm64,x64}.zip` archive and launch `Waybridge.Control.exe`. On Linux, build and start the user-level service, then generate a one-time pairing code from a second local terminal:

```bash
cargo build --release --workspace --locked
./target/release/waybridge-service serve --compositor "$PWD/target/release/waybridge"
# In another Linux terminal:
./target/release/waybridge-service pair --expires 120
```

In the GUI, add a workstation with the Linux LAN/VPN address and port `49092`, verify and enter the full `certificate_sha256` fingerprint from the trusted Linux terminal together with the one-time pairing code, then pair and connect. Client tokens are stored in Windows Credential Manager, and connections are rejected if the server certificate fingerprint changes. The service defaults to listening on `0.0.0.0:49092`; pass `--listen` to bind a specific LAN/VPN interface. See [Service Documentation](SERVICE.md) for systemd installation, trust revocation, and protocol details, and [Control Center Guide](GUI.md) for client usage.

Receiving cross-machine files on Linux requires `fusermount3` and user access to `/dev/fuse` (typically from the `fuse3` package). A read-only export view is mounted upon the first completed transfer; if unavailable, that transfer reports an error while window streaming continues. Windows temp directories require normal NTFS DACL support. Copies created through Dolphin or Explorer into a user-chosen folder are normally writable; modifying delivered caches in place fails and never writes back automatically. Because Wine currently maps this DACL into a propagating `READONLY` attribute, materialized clipboard file downloads are rejected under Wine, while read-only virtual-file drag-out and Windows → Linux uploads remain testable.

## Running Directly (Low-Level CLI)

Build artifacts:

- Linux: `target/release/waybridge`, `target/release/waybridge-service`
- Windows x64: `build/windows/waybridge-client.exe`
- Windows on ARM native ARM64: `build/windows-arm64/waybridge-client.exe`

Both Windows builds depend only on system DLLs and accept identical CLI arguments. Use the native ARM64 build on WoA devices to avoid x64 emulation overhead.

Launch a test application on Linux:

```bash
cd /path/to/waybridge
./target/release/waybridge -- weston-terminal
```

By default, the compositor creates socket `waybridge-0` and listens on `127.0.0.1:49090` (distinct from Sunshine's 47990 port). First run writes a `0600` auth token to `$XDG_RUNTIME_DIR/waybridge.token`. Read it from another Linux terminal:

```bash
cat "$XDG_RUNTIME_DIR/waybridge.token"
```

Copy `waybridge-client.exe` to Windows. Open a PowerShell tunnel and leave it running:

```powershell
ssh -N -L 49090:127.0.0.1:49090 <user>@<linux-host>
```

In a second PowerShell window, launch the client:

```powershell
.\waybridge-client.exe 127.0.0.1 49090 "<token>"
```

This is the low-level SSH-compatible invocation. Raw v3 TCP is not intrinsically encrypted and must ride over the TLS entry point, SSH, or an equivalent protected transport; optional UDP media has its own AES-GCM authenticated encryption. Authentication and version checks complete before any window data is sent; each session allows one active viewer at a time.

After upgrading, terminate older Waybridge sessions before creating new ones. Log out of any same-UID local desktop first (screen lock is not sufficient). Run `./target/release/waybridge --check-session` to inspect admission state; GUI/TLS, SSH, and direct launches run the same check. Reattaching to an existing Waybridge session takes priority and a second concurrent session is rejected; logins via other display managers after startup are left to those DMs.

Apps launched from a terminal running inside Waybridge inherit the session environment automatically. To launch additional apps into a running session from another terminal:

```bash
./target/release/waybridge --launch -- dolphin --new-window
./target/release/waybridge --launch -- hmcl-bin
./target/release/waybridge --launch -- vlc
./target/release/waybridge --launch -- mpv "/path/to/video.mkv"
# Uses your existing Chrome profile by default; explicit --user-data-dir still works:
./target/release/waybridge --launch -- google-chrome-stable
```

Pass `--socket <name>` for custom session sockets. `--launch` reads the `0600` session descriptor and injects Waybridge's display, user D-Bus, IME, and audio environment. Stale descriptors are rejected and removed on clean exit. Already-open local host windows are never migrated into Waybridge.

Sessions use the standard user D-Bus bus, existing wallets, and existing app configs/data. Waybridge exports the display environment and manages user-declared components through standalone systemd session targets; shutdown stops bound services first before revoking the environment and closing the display. Wallets and Portals use user-configured native implementations without hardcoded provider launching in core. Integration files must be installed explicitly (see [Desktop Components](INTEGRATION.md)); inspect status via `WAYBRIDGE_DESKTOP_SERVICES`. Neither KWin nor any other DE is required; Flatpak document-portal grants require per-app testing, and window sharing is not yet implemented.

The Control Center, management API, and `--launch` never auto-inject Chrome `--user-data-dir` or password-store flags—your existing data is used by default and explicit flags are preserved verbatim. Legacy isolated `~/.local/share/waybridge/sessions/` directories are kept untouched without auto-migration or overwrite. Data-directory locking follows each app's native mechanism without killing competing processes by name. Catalog launches apply explicit [Waybridge desktop overrides](CONFIGURATION.md#waybridge-specific-desktop-overrides).

Chromium picks its password backend based on desktop identity. Users coming from KDE / KWallet 6 can launch `waybridge --launch -- google-chrome-stable --password-store=kwallet6` or save that flag in a Waybridge desktop override to keep using their existing profile; Waybridge never converts password backends automatically (see [Chromium Linux Password Storage](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/linux/password_storage.md)).

Normal startup (`./target/release/waybridge -- ghostty`) automatically enables Xwayland (if installed) and a private audio sink (if available). Use `--no-xwayland` / `--no-audio` to disable, or `--xwayland` / `--audio` to require initialization and fail fast on error. When optional subsystems are unavailable, native Wayland windows still work.

Additional flags include `--render-node /dev/dri/renderD129` (select GPU), `--fps 60` (frame callback ceiling, default 60), `--codec auto|png|h264-nvenc|hevc-nvenc|av1-nvenc`, and `--bitrate 30000` (kbps per video stream). Legacy `--quality` is accepted for compatibility but ignored by lossless PNG; run `--help` for the full list. Normal operation requires no root privileges, DRM master, or attached physical monitor.

## Cursors, Icons, and Window Semantics

Cursors, XDG app icons, and window semantics negotiate automatically over v3. Supported cursor features include system shapes, hidden cursors, custom/animated cursors (up to 30 FPS independently of window frames), and DPI-scaled hotspots. Windows prefer runtime icons via Wayland `xdg-toplevel-icon-v1` or X11 `_NET_WM_ICON`, falling back to `.desktop` lookup by `app_id`, desktop ID / `StartupWMClass`, and case-insensitive X11 `WM_CLASS` matching. PNG/SVG lookups, X11 property reads, and scaling run off the compositor thread; clearing an icon reverts to desktop/default without ever executing `Exec` from `.desktop` files.

Supported window semantics include genuine `xdg-dialog` modal hints, dynamic owners, min/max sizes, fullscreen restoration, unmap/remap, and suspended/low-rate callbacks when hidden or minimized (a parent relationship alone does not imply modality). Input regions and visual alpha map independently; excluded client pixels use `HTTRANSPARENT`, supporting click-through across Waybridge windows on the same UI thread (not through to unrelated OS processes or the desktop). KDE blur-behind is not implemented.

`xdg-decoration` client/server decoration requests are honored so self-decorated apps like Chrome do not get doubled Windows title bars. Native Wayland Chrome supports `xdg-toplevel-drag-v1`—tabs detach into independent Windows windows, reattach into another Chrome window, and cancel with `Esc` while preserving the Wayland data-device grab and hit-testing real local window z-order (excluding the dragged window itself). Both ends must be updated so apps discover the Wayland global at startup. Generic X11 XDND source/target paths are wired; X11 Chrome tab tearing does not use this Wayland toplevel protocol and is not separately certified (see [XDND.md](XDND.md)).

## Relative Mouse and Pointer Lock

Implements `zwp_relative_pointer_manager_v1` and a `locked-pointer` subset backed by Windows Raw Input, hidden cursors, and cursor clipping. Lock requests engage only when the target is mapped, focused, contains the pointer inside a valid region, and has no active popup grab, reporting `locked` to the app only after native confirmation; messages carry target/generation IDs to reject stale events.

Press **`Ctrl+Alt+Shift+F12`** for emergency unlock. Focus loss, popup menus, move/resize, constraint removal, disconnects, and exits all release native lock and restore the active cursor. `confined-pointer`, unlocked standalone relative motion, popup locking, and cursor-position hints are not enabled.

## Windows Native Input Method (IME)

Bridges Windows native IME to Wayland `text-input-v3`. Update and restart both server and client so apps discover the global; CLI usage and wire version remain v3.

When a text input inside a remote app has focus, switch Windows IME normally (e.g., `Win+Space` to Microsoft Pinyin). The candidate UI stays native on Windows while preedit text renders inside the remote app and commits on confirmation. Candidate positioning tracks the app's cursor rectangle across Windows DPI, scaling, and crop origin. Switching windows cancels stale preedit, and late commits are rejected by focus generation. Plain English text flows through the same channel while `Ctrl`/`Alt`/`Win` shortcuts and navigation keys stay on the physical key channel.

Shortcuts no longer emit spurious translated characters (e.g., `Ctrl+Space` will not insert an extra space). After composition finishes or cancels, held modifiers such as `Ctrl`/`Shift` resynchronize while released keys stay up; `AltGr` continues working for local keyboard layouts.

Real Qt and Ghostty/GTK are verified at 175% DPI for Chinese, emoji, preedit/cancel, and focus protection. Physical WoA testing (2026-11-10) verified Microsoft Pinyin's native candidate UI, selecting and committing CJK text, and immediate Backspace/Return without refocusing; Ghostty's real Linux PTY regression checks immediate `DEL`/`CR` after commit. Other IMEs, physical GTK candidate behavior, and broader switching scenarios remain to be expanded; Wine Unicode injection and physical device runs are tracked separately.

Only Wayland apps implementing `text-input-v3` are supported; X11/XIM, reconversion, and surrounding-text queries are not implemented (surrounding app text is never sent to Windows). Startup commands and `--launch` automatically strip host `GTK_IM_MODULE` / `QT_IM_MODULE` overrides and prefer native Wayland for Qt/GTK so remote input is not hijacked by a host fcitx instance.

## Session Audio (Optional)

When `pactl`, `parec`, and a PulseAudio / PipeWire-Pulse daemon are available on Linux, audio enables automatically. The server creates a private sink, routing PulseAudio apps via `PULSE_SINK` and native PipeWire apps such as `mpv` via `PIPEWIRE_NODE` / stream properties without altering the host default output. Apps started via `--launch` join the same sink automatically. Native PipeWire streams forbid falling back to host speakers if the sink vanishes; direct hardware-ALSA apps must choose a compatible backend.

Windows clients auto-negotiate a dedicated audio connection over the TLS entry point or SSH tunnel and play via WASAPI to the default output device (`WASAPI 48 kHz stereo audio playback active` confirms startup). Set `WAYBRIDGE_NO_AUDIO=1` to disable client playback or `WAYBRIDGE_AUDIO_DEVICE` to select a WASAPI endpoint ID. Missing sound cards or audio failures never interrupt video.

Playback streams 48 kHz, 16-bit stereo PCM (~1.536 Mbit/s before framing) in 10 ms packets, capping server pending queue at 40 ms, in-flight at 40 ms, and client playback queue at 100 ms (plus OS buffers), dropping old PCM under congestion to recover real-time latency. Opus, cross-device clock sync, strict A/V sync, and output hot-switching are not yet implemented (see [MICROPHONE.md](MICROPHONE.md) for optional mono PCM microphone uplink). Clean exits (`SIGINT`/`SIGTERM`) unload the private sink; forced `SIGKILL` cannot clean up automatically, leaving modules that can be unloaded manually using logged IDs.

## Optional X11 Applications

Enabled automatically when `Xwayland` is installed so Java/JavaFX apps, X11-mode Chrome, etc. launch seamlessly from session terminals or `--launch`. Example requiring X11 on an explicit port and socket:

```bash
./target/release/waybridge --xwayland \
  --listen 127.0.0.1:49101 --socket waybridge-x11 \
  --token-file "$XDG_RUNTIME_DIR/waybridge-x11.token" -- glxgears
```

Connect Windows to port 49101 with the corresponding token; no extra Windows runtime dependencies are needed.

Look for `private rootless Xwayland ready display=":1000"` in server logs for the allocated `DISPLAY` (selected from free displays `1000–4095` avoiding existing filesystem and abstract sockets, bound solely to a `0700` same-user Unix socket). Startup commands inherit `DISPLAY` automatically, and native Wayland apps coexist in the same session:

```bash
DISPLAY=:1000 QT_QPA_PLATFORM=xcb ./build/tests/qt-app
WAYLAND_DISPLAY=waybridge-x11 QT_QPA_PLATFORM=wayland ./build/tests/qt-app
```

Supported X11 features include independent toplevels, transient owners, borderless override-redirect menus/submenus, post-move click routing, resize/close, keyboard focus, text/PNG/URI clipboard, and large-text `INCR`. X11 menus can extend beyond their parent window under mouse capture, while tooltips avoid input grabs. Clipboard reads drop stale generations with bounded cache workers (max 16 workers, 10 s timeout).

`Xft/DPI` and X11 client density are shared across the X server (initialized from the connecting Windows DPI) while each Windows window still scales presentation independently. For instance, Qt/XCB at 175% renders 1680×1120 and presents as 1000×625 if moved to 125% while keeping shared 175% Xft density; legacy apps ignoring `Xft/DPI` need their own font settings.

All X11 windows share one stable RandR output while per-window native Wayland outputs are hidden from Xwayland—resizing, changing DPI, or closing a game never mutates Java/GLFW's monitor topology. X11 toplevels are placed inside that virtual screen so negative or out-of-bounds coordinates (e.g., HMCL) never clip clicks; full monitor dimensions and work area are reported separately so fullscreen covers the taskbar properly.

Current X11 limits exclude `PRIMARY` selection, XIM/Windows IME, full EWMH fullscreen/size hints, and automatic Xwayland crash restart (generic XDND is now wired; see [XDND.md](XDND.md)). Crashing or closing an X11 app leaves Wayland windows unaffected, and clean shutdown removes private X sockets and locks.

## Data Exchange

Cross-machine files are always delivered read-only: apps can read and import them, while in-place writes and atomic replacements fail. Copy delivered items via Dolphin or Explorer into a personal folder to edit and save normally. Linux exposes completed transfers via a read-only FUSE view, and Windows protects materialized caches with NTFS DACLs (Wine's attribute mapping fails the copy-permission invariant, so materialized clipboard downloads are rejected and cleaned up on Wine while virtual-file drag-out and Windows → Linux uploads remain usable). See [Read-Only Delivery](FEATURES.md#read-only-delivery-of-cross-machine-files).

- Text and PNG images sync automatically across clipboards (12 MiB cap per format); image clipboard data and image files are distinct.
- Copying files or directories in Windows Explorer uploads and makes them pasteable in Linux apps; Linux `text/uri-list` directory trees download for pasting in Windows Explorer. Negotiated `file_tree` preserves directory hierarchies, empty directories, zero-byte files, and names; older peers fall back to flat files.
- Dragging files or directories from Windows Explorer onto a remote window uploads the batch first, then negotiates a `Copy` drop with the target app (no pre-upload hover preview yet).
- Windows apps exposing only `FILEDESCRIPTORW/A` + `FILECONTENTS` (without `CF_HDROP`) also copy or drop into remote windows via a dedicated COM worker reading 64 KiB chunks (supporting `Clone`/`Seek`, sequential streams, unknown-size `Stat`, and known-size `HGLOBAL` up to 12 MiB). Async sources receive completion notifications after Linux commit; synchronous drops wait for completion (cancellable via `Esc`). Linux apps read completed bytes via read-only FUSE (`IStorage` and unknown-size `HGLOBAL` are not supported).
- Dragging files or directories out of Linux apps to Windows targets that accept virtual files streams bytes on demand as the target reads, supporting async extraction, cancellation, and disconnect cleanup (always `Copy`, preserving Linux originals and clipboard). `CF_HDROP`-only Windows consumers are not yet supported on this drag-out path.
- Batch caps: 128 top-level items, 512 total entries, 16 directory levels, 8 GiB total payload, 1 GiB per file, 240 UTF-16 units per relative path, 240 UTF-8 bytes / UTF-16 units per name; Windows temp dir + relative path must fit within `MAX_PATH`. `Copy` only—permissions, timestamps, and hard links are not preserved.
- Symlinks, NTFS reparse points (including junctions and cloud placeholders), special files, non-local URIs, Windows reserved names, ADS, trailing dots/spaces, and case collisions in the same parent directory reject the whole batch—never silently overwriting or renaming. Linux drag-out requires Wayland data-device v3 supporting `Copy` (one outbound drag at a time, 120 s idle timeout).
- Incomplete staging trees clean up on cancel, error, clipboard replacement, or disconnect. Completed Linux batches publish under `$XDG_RUNTIME_DIR/waybridge-inbox/readonly/tree-<batch>/` or `readonly/<transfer-id>/` (reused IDs across reconnects allocate suffixed dirs to keep old URIs intact). Completed Windows caches live under `%TEMP%\Waybridge-tree-*` or `%TEMP%\Waybridge-*`. Because apps may read lazily, completed caches persist past viewer disconnect (automatic cache GC/quotas are not yet implemented; avoid deleting while apps still read). Files created by native Windows drop targets belong to the user and are never deleted by Waybridge.

Individual apps impose their own paste/drop format restrictions; proprietary app-specific formats are not translated cross-platform.

## Optional UDP/FEC Media Channel

TCP over TLS or SSH is used by default. To enable UDP explicitly, add `--udp-listen <reachable-bind-addr>:49091` on Linux (`--udp-mbps 200` caps pacing including parity from `1–1000` Mbit/s). Configure UDP directly in the Control Center, or for the low-level SSH client set:

```powershell
$env:WAYBRIDGE_UDP_HOST = "<Linux IP reachable directly from Windows>"
# Optional if NAT/port-forwarding exposes a different external port:
$env:WAYBRIDGE_UDP_PORT = "49091"
.\waybridge-client.exe 127.0.0.1 49090 "<token>"
```

SSH `-L` forwards TCP only and cannot carry UDP; use a reachable LAN or VPN address with the UDP port allowed. Handshakes and media keys still travel over the protected TLS or SSH control connection. UDP uses fresh per-session, per-direction random AES-256-GCM keys, unique counter nonces, authenticated chunks, and replay checks—never transmitting frames until authenticated.

Frames split into 1024-byte data chunks with 1 XOR parity chunk per 8 data chunks (recovering single-chunk loss per group), reassembling out-of-order packets in original order before decoding. Unrecovered losses repair over the TCP media connection to preserve H.264/HEVC/AV1/Main10 reference chains. Per-window 2-frame presentation credits remain active. Unreachable UDP falls back seamlessly to TCP, and mid-session UDP drops repair via TCP (endpoint roaming requires reconnecting). Wire version stays v3.

Dynamic FPS, Reed–Solomon, multi-loss low-latency IDR recovery, and UDP audio are not yet implemented. Global in-flight cap is 8 unacknowledged media messages / 64 MiB raw frame bytes (allowing a single oversized TCP frame exclusively) across UDP and TCP fallback; repairs have priority. Receiver reassembly caps at 16 messages / 96 MiB, letting a missing head frame delivered via TCP repair drain queued successors immediately. UDP handles frames up to 8 MiB with a 16-message / 32 MiB server repair cache; larger frames go over TCP. Disconnects interrupt slow sends per packet. Because delivery is globally ordered across media messages, one lost frame can temporarily stall other windows.

## Optional CUDA GL Color Input

On a single NVIDIA GPU, pass `--gpu-input cuda` to feed GL renderbuffer → CUDA → NVENC directly (default `cpu` keeps the standard path). Public GL framebuffer attachment queries fetch the resource, a CUDA kernel un-premultiplies and handles orientation/odd sizes on GPU, and FFmpeg consumes the CUDA RGBA frame—avoiding CPU color readback and upload on the fast path while still involving internal GPU copies, sync, and NVENC color conversion (not end-to-end zero-copy).

Transparent windows still read back 1 byte/pixel lossless alpha to CPU for PNG compression; fully opaque windows read back only a 4-byte opacity flag. GL/CUDA handoff and alpha readback are currently synchronous. PNG frames, popups, and unsupported codecs/sizes stay on CPU. Init/registration errors stay on CPU; GPU encode failures preserve the current frame as PNG, trigger a fresh CPU capture, and recover with a new CPU NVENC keyframe epoch without retrying GPU on that window/session.

Builds compile optional interop only when CUDA and GL development headers exist under `WAYBRIDGE_CUDA_ROOT` (default `/opt/cuda`), loading `libcuda.so.1` dynamically at runtime with no `libcudart`/`libnvrtc` dependency. Builds without CUDA headers work normally and log a fallback reason if `cuda` is requested. PTX is built from repo kernel sources via `bash scripts/build-gpu-kernel.sh` and requires a compatible NVIDIA driver. Regular CUDA input handles RGBA8 SDR; HDR below adds an FP16 → P010 path. Same-GPU affinity is verified via CUDA GL queries.

`python tests/gpu_smoke.py --benchmark` tests real GL/CUDA/NVENC, pixel/alpha accuracy, and error recovery, writing Release CPU vs. CUDA stage timings to `build/diagnostics/gpu-input-benchmark.json`. On a local RTX 5070 Ti, color-input + encode dropped from ~6.65 / 9.20 ms (opaque / transparent) to 1.67 / 2.40 ms at 1080p, and from ~10.80 / 15.35 ms to 2.40 / 3.37 ms at 1440p (synthetic stage timings only—not physical input-to-photon latency or universal hardware numbers).

## Experimental scRGB HDR Subset (Partial)

Default remains 8-bit BT.709 SDR. Enable explicitly on Linux with `--hdr experimental-scrgb --codec hevc-nvenc` (or `av1-nvenc`) and on Windows PowerShell with `$env:WAYBRIDGE_HDR="1"`. Requires a single NVIDIA GPU for GL/CUDA/NVENC and auto-enables CUDA input for HDR (`--codec auto` also selects HEVC when HDR is enabled).

Only Wayland surfaces explicitly tagged via `wp_color_manager_v1.create_windows_scrgb` enter HDR (linear Rec.709/scRGB where 1.0 = 80 nits). Surface trees composite into a real RGBA16F target, converting untagged SDR subsurfaces from sRGB to linear before blending. CUDA un-premultiplies and converts to BT.2020 / PQ / P010 for NVENC HEVC/AV1 Main10. Alpha stores **raw FP16 alpha bits** in a 16-bit PNG without 8-bit quantization; transparent frames read back 2 bytes/pixel alpha and keep a GPU SDR fallback image (not end-to-end zero-copy).

Clients advertise HDR only after actually decoding a Main10 probe into a P010 GPU texture and verifying P010 plane views, precision, and an FP16/scRGB composition swapchain. Each window requests HDR only while its monitor is already in Windows HDR mode (re-checked on window move and display changes), converting P010/PQ back to premultiplied linear FP16 scRGB for DirectComposition without toggling OS HDR settings. SDR displays, missing decoders, GPU/encode errors, or out-of-range/non-finite source values fall back to SDR via Reinhard tone mapping `rgb / (1 + luminance)` → sRGB; decode/present errors fall back per window to PNG.

**Overall HDR remains partial.** ICC profiles, arbitrary parametric BT.2020/PQ descriptions, general gamut mapping, HDR X11 apps, and physical Windows/WoA HDR display/MFT certification remain incomplete. Values outside positive BT.2020 `0–10000` nits are not silently clipped to masquerade as valid HDR; untagged apps stay SDR.

`waybridge --check-hdr` runs offscreen RGB10/FP16 and real Main10 codec checks without starting a session. `waybridge-client.exe --check-hdr` queries display modes and FP16 support read-only; `--check-hdr-shaders` runs offscreen P010/PQ/scRGB numerical shader checks. Linux tests: `python tests/hdr_smoke.py`, `python tests/hdr_video_smoke.py`, and Wine/Xvfb numerical + raw-alpha check `python tests/hdr_windows_smoke.py` (does not substitute for physical HDR display testing). Wire protocol uses frame kinds `7`/`8` and `hevc_main10_pq_alpha16` / `av1_main10_pq_alpha16` capabilities (keeping the 60-byte video header; kind `6` is reserved for UDP repair). Tests require NumPy, Pillow, CUDA dev headers, and real NVENC. Vendored Smithay includes both the Xwayland patches and a same-size SHM texture-cache format-transition fix.

## Building

Rust dependencies are locked via `Cargo.lock`. Linux build requirements: EGL/GLES, `libdrm`, Wayland, `xkbcommon`, FFmpeg (`libavcodec`/`libavutil`) dev packages, and `pkg-config` (plus FFmpeg `h264_nvenc`/`hevc_nvenc`/`av1_nvenc` and NVIDIA drivers for NVENC). Building the native Windows x64 client from Linux requires MinGW-w64, CMake, and Ninja:

```bash
./scripts/build.sh
```

Cross-compile the native Windows ARM64 client using LLVM-MinGW's `aarch64-w64-mingw32-clang++` into `build/windows-arm64/` without rebuilding Linux:

```bash
./scripts/build-windows-arm64.sh
# If LLVM-MinGW is not on PATH or the default location:
LLVM_MINGW_ROOT=/path/to/llvm-mingw ./scripts/build-windows-arm64.sh
```

`client/` can also be built natively on Windows with MSVC + CMake. The client relies solely on Win32, D3D11, DXGI, DirectComposition, GDI+, Media Foundation, and Windows clipboard APIs—no WSL or Qt runtime dependencies.

Linux runtime requirements: `systemd-logind` and a working user D-Bus bus (`dbus-daemon` or `dbus-broker`, e.g., `systemctl --user start dbus.service`), Python 3.11+ and GIO for session management/app catalog, and a systemd user manager plus installed Waybridge targets for declarative session component lifecycles. Wallets and file pickers require the user's chosen Secret Service/KWallet, `xdg-desktop-portal`, and GTK backend.

Optional feature dependencies: `Xwayland` for X11 apps; `pactl`/`parec` + PulseAudio or PipeWire-Pulse for audio; `rsvg-convert` for SVG icons; `fuse3`/`fusermount3` + `/dev/fuse` for read-only Linux file delivery; compatible NVIDIA GPU/driver + CUDA/GL dev headers for CUDA/HDR (CPU/PNG builds cleanly without CUDA headers). Tests additionally use Pillow, `wl-clipboard`, `wayland-scanner`, `Qt6Widgets`, Wine/Xvfb/MinGW, and NumPy (for HDR numerical checks).

## Running Tests

Run all commands from the repository root. Combined scripts automatically spin up temporary user buses, simulated `logind`, and temporary user directories so they never touch your active desktop wallet or activation environment. When running older graphical tests standalone, wrap them with `python tests/session_fixture.py python tests/<script>.py`; newer admission and desktop-service tests manage their own fixtures.

```bash
./scripts/test.sh
./scripts/test-m0-m1.sh --with-wine  # full M0/M1 Linux/Wine suite; requires NVIDIA/CUDA/NVENC
python tests/session_integration_smoke.py
```

Configuration and desktop integration tests: `tests/configuration_smoke.py`, `tests/desktop_overrides_smoke.py`, and `tests/session_lifecycle_smoke.py`. The first two run inside the combined suite; `session_lifecycle_smoke.py` launches an isolated temporary real systemd user manager inside a delegated scope before the mocked bus wrapper to verify component start/stop while the display is live.

The combined suite exercises X11, audio, UDP, GPU, and HDR simultaneously—checking capabilities, audio auth, PNG-only fallback, Wayland+X11 coexistence, control responsiveness, and clean shared shutdown—without claiming all features are jointly certified in a single physical Windows session.

Targeted regressions (each requires its corresponding dependencies):

| Area | Test Entry (`python tests/<name>.py`) |
|---|---|
| Native Windows windows / clipboard / menus | `windows_smoke` (optional `--csd` / `--terminal` / `--qt` / `--udp`) |
| Cursors & icons | `cursor_smoke`, `windows_cursor_smoke`, `app_icon_smoke`, `window_icon_smoke`, `windows_app_icon_smoke` |
| Modality / fullscreen / input regions | `window_semantics_smoke`, `windows_semantics_smoke` |
| IME & shortcuts | `ime_smoke`, `windows_ime_keys_smoke`, `ghostty_smoke --ime` |
| Relative mouse | `relative_smoke`, `windows_relative_smoke` |
| Native drag-out / trees / virtual files | `drag_out_smoke`, `tree_smoke`, `windows_drag_out_smoke` (`--tree`), `windows_virtual_smoke` |
| X11 / GLX / clipboard | `x11_smoke`, `x11_clipboard_smoke` |
| Gaming X11 scale / click & stable RandR | `x11_pointer_smoke` (requires GLFW ≥ 3.4, GL, `xrandr`) |
| Chrome tab tear-off / merge / cancel | `chrome_drag_smoke`, `windows_chrome_smoke` (isolated test profile) |
| PCM / WASAPI audio | `audio_smoke`, `windows_audio_smoke` |
| Real NVENC / caches / fallback | `hardware_smoke`, `codecs_smoke`, `resize_smoke`, `windows_codecs_smoke` |
| CUDA & fault recovery | `gpu_window_lifecycle_smoke`, `gpu_smoke --binary target/debug/waybridge` (`--benchmark` for Release comparison) |
| UDP faults / all codecs / global flow control | `windows_udp_smoke`, `udp_flow_smoke` |
| HDR numeric / video / Windows shaders | `hdr_smoke`, `hdr_video_smoke`, `hdr_windows_smoke`, `hdr_preflight` |

Wine GUI tests use the GDI fallback path and do not certify physical Windows GPU/alpha rendering; offscreen software D3D11 HDR tests verify shaders/WIC only. Test logs write under `build/tests/` and `build/diagnostics/` and are Git-ignored.

## On-Device Windows Checks and Latency Logs

Run `waybridge-client.exe --check-codecs` first. Normal GPU presentation logs `D3D11/DirectComposition presentation active` along with the active video decoder (`GDI fallback` lacks per-pixel alpha). For HDR, run `--check-hdr` to inspect current display capabilities and verify Main10, alpha, and SDR fallback on an actual HDR display (Waybridge never toggles OS HDR).

Key physical verification targets: 175% DPI, Ghostty overflowing menus, real Microsoft Pinyin candidates/selection/focus switches, bidirectional Explorer directories and drag-out, Outlook/archiver virtual file sources, pointer lock + emergency unlock, new Windows codecs, and multi-monitor moves. Linux → Windows drag-out is always `Copy` and requires virtual-file-capable targets; validate small files, empty directories, and Unicode names with hash checks first.

Linux:

```bash
./target/release/waybridge --stats --codec auto -- ghostty
```

Windows PowerShell:

```powershell
$env:WAYBRIDGE_STATS = "1"
.\waybridge-client.exe 127.0.0.1 49090 "<token>"
```

Linux logs composite, readback, encode queue, convert, encode, send, and presentation-ACK round-trip timings; Windows logs decode queue/work, UI queue, present submit, and control RTT. Each side uses its own monotonic clock (`present` excludes display scanout, `send` is socket write only, `display_roundtrip` includes round-trip—none equal physical input-to-photon latency). Resizes additionally log `encoder_init`, `encoder_reuse`, `resize_first_encoded`, `resize_first_sent`, `decoder_create`, `decoder_reuse`, and `resize_to_present`.

Diagnostic environment overrides: `WAYBRIDGE_DPI=168` (force 175%), `WAYBRIDGE_GDI=1` (force GDI fallback), `WAYBRIDGE_PNG=1` (disable video negotiation); unset to restore defaults. Keep builds, window sizes, and network paths identical across A/B comparisons; no input-to-photon benchmarks or Moonlight performance claims are made here.

## Current Limitations

- Each video toplevel consumes an NVENC session subject to GPU/driver concurrency and size caps (typically `48×48–4096×2304`, with larger or rejected windows falling back to PNG). SDR video is 8-bit 4:2:0 (no 4:4:4), so colored text may be less crisp than PNG.
- CUDA/HDR requires a single compatible NVIDIA GPU and still involves alpha readback, sync, and GPU-internal copies. HDR covers explicit Wayland scRGB only; ICC, generic parametric color management, HDR X11, and physical Main10/HDR certification remain incomplete.
- IME supports `text-input-v3` only (no XIM, surrounding text, or reconversion). X11 lacks `PRIMARY`, full EWMH, and automatic crash restart; apps ignoring `Xft/DPI` need their own scaling settings.
- Pointer lock and input regions are scoped subsets (no cross-process click-through, KDE blur, or `confined-pointer`).
- Cross-machine files are `Copy`-only without preserving permissions, timestamps, or hard links, and without resume, on-demand network FUSE, or `IStorage`; Explorer, Outlook, archivers, and COM sources ignoring cancellation still need broader physical testing.
- Audio lacks Opus, strict A/V sync, and output hot-switching; UDP uses global media ordering without endpoint roaming, so packet loss on poor links can briefly stall other windows.
- Raw v3 TCP requires the built-in TLS entry point, SSH, or an equivalent protected transport and is incompatible with Sunshine/Moonlight wire protocols. TLS entry restarts preserve live sessions, whereas surviving a compositor crash is not implemented and `SIGKILL` skips cleanup.

SHM, DMA-BUF, and optional Xwayland are wired, but complex apps, drivers, multi-GPU setups, and physical Windows environments still require per-combination testing rather than promising universal compatibility.

For current window activation, tray, and drag-and-drop boundaries, see [DESKTOP_INTERACTIONS.md](DESKTOP_INTERACTIONS.md).
