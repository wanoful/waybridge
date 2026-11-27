# Waybridge Plan and Roadmap

Updated: 2026-11-18. Existing M0/M1 verification serves as the baseline for ongoing development on `main`. M2 has landed desktop entry points, read-only file delivery, and drag-and-drop while advancing Linux media resource budgets and delivery-feedback-driven UDP pacing; M3 is progressing in parallel starting with experimental native multitouch. Parallel progress does not imply milestone completion. Windows resource budgets, per-app visual policies, encoder/network coordination, gamepads/pen, and remaining device capabilities remain under active development; hardware coverage and compatibility matrices expand as each validation record lands.

Waybridge targets professional desktop workloads and gaming on Windows, running Linux applications in a standalone session and presenting them as independent native Windows windows over LAN or VPN. Its goals are host-DE independence, SSH-free daily connections, and preserving running Linux sessions across disconnects.

## Documentation Roles

- [ARCHITECTURE.md](ARCHITECTURE.md): technical boundaries, state ownership, and process/channel design.
- [FEATURES.md](FEATURES.md): feature scope, application samples, recommended baselines, and acceptance criteria.
- This document: execution order, delivery milestones, dependencies, and exit criteria.
- [README.md](README.md), [PROTOCOL.md](PROTOCOL.md): current usage, testing, and implemented wire protocol.

## Confirmed Direction

| Area | Baseline |
|---|---|
| Product form | Standalone Linux remote application session with independent native Windows windows; one active viewer per session |
| App entry | Applications and launchers start inside the Waybridge session |
| Desktop environment | Does not require KWin, Plasma, or GNOME Shell; the session manages its own services using standard interfaces and existing components |
| Core stack | Linux Rust/Smithay compositor + native codec layer; Windows C++ engine + C# / WinUI 3 Control Center |
| Process layout | One Rust process per session with bounded media and I/O workers plus managed desktop services; a user-level service provides independent pairing and session access |
| Connectivity | Windows clients connect uniformly via Waybridge TLS to the Linux user service; system SSH handles Linux deployment, admin, and optional TLS forwarding |
| Productivity & gaming | Shared session, window, input, and wire protocol with per-window clarity, FPS, bitrate, and queuing policies |
| Session recovery | Reattach to surviving sessions after network or viewer drops; explicit session termination is separate from disconnecting |
| Desktop entry points | Remote app trays/menus, notifications, background apps, and taskbar identity are tracked as first-class deliverables distinct from Waybridge's own tray |
| File delivery | Cross-machine files are always read-only; users copy via Dolphin/Explorer into a chosen directory to edit and save normally, without per-app Save As hooks or automatic write-back |

The first pass keeps Linux-side per-window surface-tree compositing while Windows composites independent native windows. Direct encoding, partial updates, and selective surface layering are evaluated by measurement.

## Current Progress

| Milestone | Implemented & Verified | Remaining Acceptance & Follow-Up |
|---|---|---|
| M0 | Both sides' file receive moved to bounded workers; Windows network sends moved off the UI thread; stale connection/selection results invalidated; auto/HDR and PNG fallbacks unified; output global, OLE window lifetime, and X11 input-coordinate fixes landed; capture module extracted with pre-capture credit/queue checks, latest-update preservation, and P50/P95/P99 telemetry; existing Linux/Wine regressions pass | Large entry-point modularization, GPU sync stalls, and full performance baselines remain open; WoA, mixed monitors, and broader networks need physical testing; Windows x64 mixed drag-and-drop and OLE return fixtures passed, while full GUI/device sign-off is tracked separately |
| M1 | Standalone TLS user entry point, pairing/revocation, session & app management; shared user D-Bus/data and standard component integration; TOML config and desktop overrides; one-way admission and reconnect after disconnect or entry restart; bound services cleaned up on normal and error exits without diagnostic writes blocking teardown; real Chrome/KWallet login restored | x64/Tailscale and selected apps passed; WoA, full app workflows, additional networks, and Flatpak document-portal grants need verification; ScreenCast belongs to M4. Legacy isolated-data migration is optional recovery, not a prerequisite for shared data |
| M2 | Trays, notifications, window integration, per-app visual presets, bidirectional HTML/RTF clipboard, dedicated file transfer channel, and microphone forwarding landed. Linux and Windows media resource budgets are wired, and UDP feedback drives NVENC bitrate | Broader app matrix for file drag-and-drop, X11 drag visuals, file hover, physical microphones, and real conferencing apps still need expansion; see [Desktop Interactions](DESKTOP_INTERACTIONS.md) |
| M3 | Windows native multitouch, Precision Touchpad, tablet-v2 pen, and 4-slot SDL3/`uinput` gamepads landed, including native Wayland touch/pen drags, window operations, rumble, and short same-device reconnect | Physical Windows pen OLE drag-out and Steam player ordering need on-device verification. Broader app coverage and X11 native touch remain open. |
| M4–M5 | Architecture and scope documented | Full feature delivery not yet started |

Automated M0/M1 verification runs via `scripts/test-m0-m1.sh --with-wine`. It tests the real Linux compositor, TLS service, .NET management layer, and Wine native worker, and does not replace physical WinUI verification on Windows.

See [DESKTOP_INTERACTIONS.md](DESKTOP_INTERACTIONS.md) for current interaction mechanics, the drag-and-drop direction/format matrix, and remaining gaps.

## Next Work Items

With existing verification as the baseline and `main` as the unified trunk, work proceeds in the following order, interleaving compatibility expansion with feature delivery:

1. **Converge media scheduling**: Linux checks presentation credits, raw queue capacity, and resource budgets before compositing/readback; Windows enforces global and per-window budgets for decode buffers, GPU presentation textures, and hardware decoders while prioritizing foreground windows. Continue calibrating resource usage, frame age, and recovery under real multi-window Windows loads. See [Media Budget](MEDIA_BUDGET.md).
2. **Everyday workflows**: Per-app visual presets, dedicated file channel (progress/cancel/rate limit), bidirectional HTML/RTF clipboard, and single-channel microphone forwarding are in place. Expand testing across real office, clipboard, and conferencing app suites. On the networking track, continue integrating FPS adaptation, per-stream delivery, and real LAN/VPN capacity measurements. See [Network Experience](NETWORK.md).
3. **Complete desktop integration**: Expand real-app testing for tray menus, notification actions, background apps, and taskbar behavior alongside Flatpak document-portal grants, open/save flows, URI/login callbacks, polkit, and SSH/GPG agents.
4. **Professional input**: Native pen drags and short same-device gamepad reconnects are in place. Continue physical-device verification for Windows pen OLE and Steam player ordering (see [Professional Input](INPUT_DEVICES.md)). M4 adds webcam, window sharing, printing, A/V sync, and large-project resume.
5. **Compatibility and performance matrix**: Continuously add Blender editing/saving, Minecraft gameplay, native Linux and large Windows games, LAN/VPN links, and mixed DPI. Keep existing launch/menu results scoped accurately and separate compatibility-layer bugs from Waybridge bugs; these matrix expansions do not block ongoing core work.

Direct surface encoding, partial updates, and finer-grained Windows-side surface compositing remain M5 measurement experiments rather than blockers for the tracks above.

### Client Launcher and Server Management TUI

The client converges on a single TLS transport, retiring embedded SSH and raw tunnel modes. During the prototype phase both ends update together, and users can re-add and pair workstations on upgrade. The Linux TUI supports both local TTYs and system SSH terminals for session management and configuration editing. See [Launcher and TUI Design](LAUNCHER_AND_TUI_DESIGN.md) for sequencing and acceptance criteria.

### Reusing Upstream Libraries from Sunshine and Moonlight

- **SDL3 gamepad support** handles discovery, mapping, hotplug, and rumble in the Windows client. Discovery, input, rumble, and reconnect were verified with a physical Xbox Bluetooth controller. Waybridge manages focus leases, unfocus zeroing, slot assignment, and Linux session ownership. See [Gamepads](GAMEPADS.md).
- **`libopus` audio** is preferred for Linux-to-Windows playback at 128 kb/s with 10 ms stereo blocks. PCM remains available through codec negotiation. Verify latency, dropouts, A/V sync, and CPU cost across LAN and VPN links. See [Network Experience](NETWORK.md).
- **`libvirtualhid`** will be evaluated when advanced controller features such as DualSense touchpads and gyroscopes are needed (assessing Linux `uhid` permissions, Rust FFI boundaries, game detection, and feedback); `uinput` continues serving standard Xbox controllers.
- **Forward error correction** uses existing XOR parity + TCP repair as the baseline. Compare Reed–Solomon under equal transmit budgets for burst-loss frame age, repair success rate, and CPU overhead before introducing a separate implementation (`moonlight-common-c` serves as a GameStream design reference). See [Network Experience](NETWORK.md).

## Independent Desktop Integration Deliverables

M2 delivers both media/data workflows and desktop entry points as separately verified tracks so capabilities like trays are never hidden inside a vague "background apps" bucket. See [Desktop Integration Delivery Checklist](FEATURES.md#desktop-integration-delivery-checklist).

| Area | M2 Deliverable | Follow-Up / Deferred Scope |
|---|---|---|
| Remote app trays | SNI icons, status, activation, and menus rendered natively on Windows; background reopen/exit and reconnect recovery | Probe XEmbed separately and pull forward if required by target apps; unexportable menus fall back to verified Linux menu windows |
| Desktop notifications | Content, replacement/close, action callbacks, toggles, and Do Not Disturb; no replaying stale actions on reconnect | Offline notification archive and platform-specific extensions confirmed separately |
| Background apps & taskbar | Background apps discoverable with restore and clean exit entries; consistent app identity, icons, and taskbar grouping | Start Menu / pinned shortcuts and saved launch bundles tracked separately |
| Remote icon badging | Preserve original app icons across windows, app catalog, and trays with upper-right Waybridge badge and per-connection custom badges | Primary icon/badge, runtime window icons, and desktop/`StartupWMClass` fallback are wired; client-area icons painted by apps remain untouched |
| App invocation & auth | Native open/save, uniform read-only cross-machine delivery + file-manager copy, URI/login callbacks, MPRIS2 ↔ Windows SMTC media controls, and selected Flatpak/native auth flows | No transparent cross-machine file picker, forced Save As, or auto write-back; global menus, GlobalShortcuts backend, and extra system tools confirmed per app |
| Session state transitions | Release input cleanly across Windows lock, sleep/resume, and disconnects while preserving remote sessions without stuck inputs | Extend stop/resume semantics per peripheral for microphone, webcam, gamepads, and printing |

App verification must record the exact tray and menu protocol used. Passing requires the full loop: launch app → close main window leaving it in the background → reopen/operate menu from the Windows tray → receive and click a notification → disconnect and reconnect → exit cleanly. Having a visible window, receiving one notification, or seeing Waybridge's own tray icon does not substitute for this sign-off.

## Delivery Milestones

| Milestone | User Outcome | Exit Criteria |
|---|---|---|
| M0 Core Consolidation | Existing capabilities are stable, responsive, and diagnosable | Remove interactive-thread blocking; align state and config semantics; verify native client and GUI on physical Windows |
| M1 Standalone Sessions & Connectivity | Pair, connect, and launch apps without a Linux host DE or Windows SSH setup | User service, authenticated encrypted direct connections, native desktop services, and disconnect recovery pass end-to-end |
| M2 Daily Workstation Preview | Blender, office suites, and everyday gaming work reliably with full Windows desktop entry points | Remote trays/menus, notifications, background apps, clarity/interactive/low-latency presets, basic network adaptation, rich text, microphone, and transfer management meet baseline |
| M3 Professional Input & Gaming Preview | Pen creation, multitouch, and multi-controller gaming work seamlessly | Target hardware, apps, and input cancellation/recovery pass on physical devices |
| M4 First Complete Stable Release | Conferencing, printing, large project transfers, and core productivity/gaming workflows work end-to-end | Webcam, window sharing, A/V sync, printing, and project transfers pass; all seven new capabilities meet baseline and soak tests |
| M5 Future Enhancements | Higher image efficiency, fidelity, and broader hardware coverage | Each optimization proves measurable value and compatibility before adoption |

## M0 Core Consolidation

- Extract session, window/input, media, transport, and data-task responsibilities out of large entry points while keeping runnable builds at each step.
- Move file disk I/O, network send waits, and bulk conversions off the compositor and Windows UI threads.
- Unify session, connection, window, and media IDs/generations along with reconnect snapshots and input-release semantics.
- Unify config and capability negotiation, fixing startup vs. live-update mismatches for `auto`/HDR.
- Complete physical x64/ARM64 WinUI startup verification and establish representative app and network baselines.

Exit criteria: the UI stays responsive during file transfers, resizes, and reconnects; desired, effective, and fallback config values agree; existing regressions pass.

## M1 Standalone Sessions and Connectivity

- Provide a Linux user-level service supporting pairing, trust management and revocation, session listing/creation/reattach, app launching, and session shutdown.
- Standardize on `rustls` / `SslStream` / Schannel TLS carrying v3 with per-channel session authentication; defer QUIC, datagrams, and loss-scheduling comparisons to M2.
- Connect Windows clients directly over Waybridge TLS to configured LAN/VPN endpoints; converge the Control Center onto the single TLS path while keeping system SSH or local TTYs for deployment and administration.
- Unify Waybridge display, user D-Bus, service activation, and config environment, verifying startup, failure, recovery, and teardown cleanup.
- Align session integration with [Configuration Policy](CONFIGURATION_POLICY.md): move component selection and dependencies to standard configs, remove hardcoded wallet activation from core, and validate real graphical session lifecycles.
- Standardize working-directory rules and status reporting for desktop/command launches, implement explicit Waybridge-specific desktop overrides, and verify arguments, actions, D-Bus activation boundaries, and immutability of original files.
- Ship config validation, effective-value/source queries, and verified component combinations for file pickers, links, and native auth dialogs while keeping app-specific flags in user configs and examples.

Exit criteria: pair, connect, launch, disconnect, and reattach without a host DE or Windows OpenSSH; alternate cleanly between local desktop and Waybridge with shared user data via one-way creation checks without intercepting later DM logins (see [Session Policy](SESSION_POLICY.md)).

## M2 Daily Workstation Preview

The initial wave delivered filesystem-layer read-only delivery, SNI/DBusMenu trays and background entries, the opt-in standard notification provider and Windows native notifications, branding/per-connection icon badges, and window `AppUserModelID` grouping (see [FEATURES.md](FEATURES.md)). Both ends must be updated together, and Linux notifications require explicitly opting into the provider without displacing an existing daemon.

Plain-text drag-and-drop separately fixed real bidirectional Chrome drags and immediate-release timing, separating immutable offers, negotiation/completion state, native adapters, and input/visuals while adding an independent toggle and target revisions. On 2026-11-17, generic X11 source/target and mixed-backend paths were added, preserving intra-session COPY/MOVE and cross-machine read-only COPY along with completion receipts, cancellation recovery, and large-text tests. Next come broader real-app tests, X11 drag visuals, file hover/progress/cancel, and richer MIME formats; passing fixtures does not imply universal app compatibility (see [Drag-and-Drop Design](DRAG_AND_DROP.md)).

M2 also includes per-app visual presets, file transfer management, bidirectional rich-text clipboard, and microphone forwarding. Real-app compatibility, on-device media-budget tuning, full audio hot-switching, Flatpak/URI/login callbacks, lock/sleep/mixed-DPI behavior, legacy XEmbed, dynamic menus, and full taskbar matrices remain to be expanded.

- Deliver per-app saved presets (clarity, interactive workstation, low-latency gaming) resolved against real capabilities.
- Check downstream credits before capture, schedule media per window, and enforce global + per-window resource and queue budgets.
- Separate file bytes from interactive control with progress, cancellation, and bandwidth caps; adapt media load when network quality degrades.
- Enforce uniform cross-machine read-only file delivery across copy and drag-and-drop so files copied via Dolphin/Explorer edit normally without app-specific Save As hooks or auto write-back.
- Deliver bidirectional rich-text clipboard and single-channel microphone forwarding, improving audio recovery and device switching.
- Implement SNI tray/menu forwarding, icon/status updates, activation/background restore/exit, and assess XEmbed requirements per target app.
- Wire Waybridge branding and upper-right source badges (including custom per-connection badges) across window and tray icons, validating small sizes and DPI switches.
- Provide notification action callbacks, background app entries, and per-app taskbar identity with clean sync across disconnects, app exits, and Explorer restarts.
- Cover core native and Flatpak flows (file/URI and selected auth) plus input release and reconnect recovery across Windows lock and sleep.

Exit criteria: establish repeatable workflows across Blender/GIMP, WPS/LibreOffice/Chrome, Minecraft, one native Linux game, and one major Windows game—plus a real tray/notification-capable background app completing the full tray/menu/notification/reconnect/exit loop—across LAN/VPN, mixed DPI, multi-window, and concurrent file transfers.

## M3 Professional Input and Gaming Preview

- Standard gamepad layout, dual-motor rumble, up to four controllers, hotplug, deadzone, and unfocus/disconnect zeroing.
- Native multitouch, cross-DPI coordinates, contact cancellation, and per-app compatibility mapping; CSD touch move/resize, popups, and native Wayland touch text/file drags are wired, with physical Chrome tab and Windows OLE verification, mixed backends, and broader device matrices continuing.
- Pen input preserving pressure, hover, buttons, and hardware-valid tilt/rotation with adjustable pressure curves.
- Compatibility and diagnostics for Xwayland, Windows game runners, and virtual input device permissions.

Exit criteria: real pen/touch workflows in Blender/GIMP and two-controller play in Split Fiction pass on physical hardware with zero stuck inputs or rumble across window switches, disconnects, and device hotplugs.

## M4 First Complete Stable Release

- Single webcam coordinated with microphone forwarding (device selection, disable, status indicator, recovery).
- Per-window sharing inside Waybridge for conferencing apps and OBS (authorization, preview, streaming, stop).
- Audio/video sync and recovery; printing from Linux apps to selected Windows print queues and PDF output.
- Project-scale transfers with directories, large files, progress, cancellation, rate limiting, and resume across network drops while the remote session survives, keeping committed files readable.
- Full fixed-app suite, network/device fault injection, 8-hour soak testing, and repeated window open/close stability validation.

Exit criteria: gamepads, touch, pen, microphone, webcam, rich text, and printing all meet their target baselines across Windows x64/ARM64 and LAN/VPN, with measured resolution, latency, and transfer-scale reports.

## M5 Future Enhancements

- Evaluate simple-surface direct encoding, partial updates, or selective layering based on real measurements.
- Expand high refresh rates, sustained high resolutions, HDR and color management, advanced controller features, and wider device matrices.
- Add service discovery, cross-device handoff, additional viewer platforms, multi-user sessions, or post-reboot workspace recovery as needed.

High-value optimizations with clean interfaces may land earlier; advanced display experiments never block core milestones.

## Execution and Verification Methodology

M0–M5 goals and exit criteria remain the guiding framework while resource budgets, network experience, and M3 professional input progress in parallel—each shipping runnable vertical slices before expanding full device/app matrices. M2 and M3 serve as usable previews; M4 is the first complete stable release retaining all seven confirmed new capabilities.

Every workstream tracks status, dependencies, exit criteria, and empirical evidence. Uncertain areas (transport choices, shared wallet semantics, virtual input permissions, game runners) start as minimal prototypes before scaling up, and performance budgets are calibrated against controlled baselines.

Planning is milestone-based without unverified calendar promises. Architectural boundary changes update [ARCHITECTURE.md](ARCHITECTURE.md), scope changes update [FEATURES.md](FEATURES.md), and sequencing updates live here.
