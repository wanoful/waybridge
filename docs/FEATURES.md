# Waybridge Feature Scope and Implementation Status

Updated: 2026-11-18. Status: confirmed feature scope and recommended acceptance baselines annotated with current implementation progress. Cross-machine read-only file delivery, SNI trays/background entries, native notifications, branding, and source badges have landed as the initial M2 wave.

Target use cases include large creative/professional workloads such as Blender, everyday productivity, and gaming across both LAN and remote VPNs. This document records features, user workflows, and acceptance criteria; see [v1 Architecture Baseline](ARCHITECTURE.md) for module boundaries and sequencing. Confirmed requirements and optional candidates are separated, and specific delivery waves, hardware models, and numeric targets converge against empirical verification.

## Confirmed Feature Scope

- Applications launch inside a private Waybridge session.
- Blender is the primary professional workstation target; other installed apps serve as compatibility candidates.
- Gaming targets cover Minecraft, native Linux games, and Windows games/launchers running via compatibility layers.
- Input covers keyboard, mouse, gamepads, multitouch, and pressure-sensitive stylus pens.
- Audio/video covers audio playback, microphone voice uplink, and webcam.
- Data exchange covers text, images, files/directories, rich-text clipboard, and printing.

All seven new capabilities remain in scope for the complete release and ship in staged waves per [ROADMAP.md](ROADMAP.md), calibrated against recommended baselines and real measurements.

On 2026-11-14, physical Windows 11 x64 testing passed GUI startup, pairing and credential storage, Tailscale direct connection, hardware H.264, Unicode-filename uploads, and session detach/reattach/end workflows, with Blender and Ghostty launch-and-render verified.

Subsequent testing verified HMCL button interaction and game launch, Minecraft windowed/maximized/fullscreen menu coordinates, and Chrome login recovery after unlocking the user's native wallet. Regressions and fixes for abnormal exits, status-write failures, and Windows drag-and-drop close races are deployed. These results do not yet generalize to full gameplay sessions, full Blender editing pipelines, arbitrary legacy wallet migrations, or all Windows peripherals.

Plain-text drag-and-drop has been validated between real Windows Chrome and native Wayland Chrome across immediate release, injected latency, rejection/cancellation, cross-window remote drags, and multi-target drags. Updated peers negotiate target revisions and completion receipts with bounded waits and an independent **Text drag-and-drop** toggle separate from file sharing. See [DRAG_AND_DROP.md](DRAG_AND_DROP.md) for design and latency bounds; X11 source/target and mixed-backend generic paths are also wired, while cross-machine rich-format drags and broader app matrices remain open.

## Installed Applications and Games Snapshot

Reference application and game catalog used to assemble representative compatibility matrices. Presence in this list is distinct from verified compatibility; items listed here are test candidates until explicitly verified.

### Professional and Productivity Applications

| Application | Host Evidence | Suggested User Workflows |
|---|---|---|
| Blender | System package & binary (primary target) | 3D viewport navigation, middle-mouse orbit, continuous transforms, popups/menus, Unicode text/paths, previews, and project saving |
| GIMP | System package & binary | Large canvas zoom/pan, tool switching, brush strokes with pressure, color accuracy, and alpha transparency |
| WPS Writer, Spreadsheets, Presentation, PDF | System package & binary | CJK IME input, rich text, cells and images via clipboard, file dialogs, and printing |
| LibreOffice Writer, Calc, Impress | System package & desktop entries | Documents/spreadsheets, rich-text interop, fullscreen presentations, files, and printing |
| IntelliJ IDEA, VS Code, Ghostty | Install directory or binary | Small-font legibility, keyboard shortcuts, terminal input, popups, and multi-window workflows |
| Chrome, Dolphin, mpv, VLC | System package or binary | Web browsing, file management, copy/paste, drag-and-drop, video and audio playback |
| Feishu, QQ, WeChat, Telegram, OBS Studio | Package, Flatpak, or desktop entry | Candidate apps for microphone, webcam, media, and notifications (priority TBD) |

Blender and GIMP combine crisp UI chrome with continuously updating canvases, requiring sharp text, fine lines, accurate colors, and responsive viewports simultaneously. Large creative projects and asset folders also motivate lifting the current 1 GiB single-file and 8 GiB directory-batch transfer limits.

### Steam Titles (Reference Catalog)

| Title | Steam App ID | Notes |
|---|---|---|
| Cyberpunk 2077 | 1091500 | Windows executable via Proton |
| Split Fiction | 2001120 | Windows executable via Proton (co-op dual-gamepad candidate) |
| Stellar Blade | 3489700 | Windows executable via Proton |
| Stellar Blade Demo | 3564860 | Demo build |
| Assassin's Creed Odyssey | 812140 | Windows executable via Proton |
| The Great Ace Attorney Chronicles | 1158850 | Windows executable via Proton |
| Brotato | 1942280 | Native Linux ELF |
| Stardew Valley | 413150 | Native Linux ELF |
| Human Resource Machine | 375820 | Native Linux ELF |
| Message From Aliens Demo | 5195140 | Demo build |
| Wallpaper Engine | 431960 | Utility entry |

Proton Experimental, Proton 11.0, and Steam Linux Runtime are installed; each game's selected runner, launchability, and Waybridge compatibility are recorded individually during verification.

### Other Games and Launchers

- **Minecraft & HMCL**: HMCL and Minecraft Launcher with vanilla, Fabric, NeoForge, and modpack instances; acceptance covers 3D camera look, menus/inventory, IME chat, and focus handoff between launcher and game.
- **Wuthering Waves**: Windows game binary; compatibility runner and native launchability recorded separately.

Start representative gaming acceptance with Minecraft, one native Linux game, and one large Windows game before expanding to the full catalog. Game windows spawned by Steam or other launchers must appear inside the Waybridge session; runner or anti-cheat issues are tracked separately from Waybridge input/display issues.

## Currently Implemented Features

"Implemented" means code exists in the repository. Linux integration tests, Wine Windows-API tests, and physical Windows/WoA runs are tracked separately and do not substitute for each other. WinUI Control Center startup and selected connection, app, file, and session flows have passed on physical Windows 11 x64; updated WoA, physical HDR, and broader app/device matrices remain open; SNI native menu actions and Qt `QAction` exits have passed on Windows x64.

| Capability | Currently Implemented | Current Boundaries |
|---|---|---|
| Connection & profiles | TLS fingerprint pinning, one-time pairing, client revocation, multiple profiles, session create/reattach/reconnect | GUI manages one active connection at a time; trusted clients can manage all Waybridge sessions for the same Linux user |
| App launching | Installed `.desktop` catalog, search, launch terminal, CLI launch of extra commands | Launches into Waybridge's private session; existing host-desktop windows are not migrated |
| Session persistence | Viewer disconnects keep Linux apps alive and reconnects restore windows; explicit session termination | One active viewer per session; recovering app state after a compositor or Linux host reboot requires separate design |
| Independent Windows windows | Toplevels, popups/menus, transparency, modality, SSD/CSD negotiation, min/max size, minimize/maximize/fullscreen | Platform/app subsets apply; cross-process transparent click-through and blur-behind are not implemented |
| Display & scaling | Follows Windows DPI, manual scale, fractional scaling, live resize | X11 apps share one client density; physical multi-monitor and mixed-DPI combinations still need full validation |
| Keyboard, mouse & IME | Keyboard, mouse, scroll, native cursors, Windows IME → Wayland `text-input-v3` | Basic Qt + Microsoft Pinyin tested on physical WoA; broader IMEs, switching combos, and X11/XIM remain uncovered |
| Native multitouch (experimental) | Windows `WM_POINTER` → Wayland `wl_touch`, up to 10 contacts, fractional scale, generations, cancel recovery; app-confirmed Windows move/snap handoff | Saved per profile, live toggle, automatic subscription detection, optional per-app mouse compatibility; x64 system injection verified move/snap; physical touchscreens, mixed DPI, and Windows OLE remain open ([INPUT_DEVICES.md](INPUT_DEVICES.md)) |
| Precision Touchpad (experimental) | Windows Direct Manipulation two-finger scroll/pan and pinch mapped to Wayland finger axis and native pinch gestures | Auto-negotiated on both ends; falls back to wheel when unnegotiated; Windows 3/4-finger OS gestures stay local ([TOUCHPAD.md](TOUCHPAD.md)) |
| Pressure pen (experimental) | Windows `PT_PEN` → `tablet-v2` (hover, tip, pressure, tilt, rotation, barrel button, eraser, window ops, native Wayland drag-and-drop) | Off by default; unsubscribed apps and X11 use mouse compatibility; physical pen Windows OLE handoff pending verification ([PEN.md](PEN.md)) |
| Gamepads (experimental) | Up to 4 SDL3 gamepads → Linux `uinput` (buttons, sticks, triggers, deadzone, dual-motor rumble, short same-device reconnect) | Off by default; requires `/dev/uinput` access; real `evdev` verified window switches and 2-controller short reconnects; Steam player order and physical rumble pending ([GAMEPADS.md](GAMEPADS.md)) |
| Gaming mouse | Raw Input relative motion, pointer lock, cursor hide/clip, unfocus release, emergency unlock | No `confined-pointer` yet; gamepads forward standard layout; game compatibility verified per title |
| Clipboard | Bidirectional UTF-8, HTML, RTF, PNG, files, and directories with direction, image, and file toggles | Rich-text fidelity depends on formats offered/accepted by each app; proprietary app formats tracked separately |
| File transfer & drag-and-drop | Dedicated authenticated file channel for files and directories; drawer shows progress/cancel; bandwidth cap in Sharing & Permissions | Cross-machine COPY only; intra-session preserves COPY/MOVE; read-only delivery with writable user copies; pause and disconnect resume pending ([FILE_TRANSFERS.md](FILE_TRANSFERS.md)) |
| Text drag-and-drop & feedback | Bidirectional Windows Unicode ↔ Wayland/X11 UTF-8 COPY, cross-HWND intra-session drags with OLE return, animated alpha DND icons | Cross-machine HTML/RTF/image drags, X11 drag visuals, and pre-transfer file hover not yet covered ([DESKTOP_INTERACTIONS.md](DESKTOP_INTERACTIONS.md)) |
| Audio playback & capture | Private session audio, Windows WASAPI playback, volume, mute, output device selector, optional default microphone uplink | Stereo PCM playback, mono PCM uplink; physical mics, conferencing apps, and output hot-switching pending verification ([MICROPHONE.md](MICROPHONE.md)) |
| Core display | PNG, H.264, independent lossless alpha, Windows GPU decode with PNG fallback; per-app presets (text clarity, interactive low latency, follow connection) | Effective FPS, encoder, and decoder remain governed by negotiated capabilities and resource budgets |
| Optional display features | HEVC, AV1, CUDA GPU color input, experimental scRGB HDR | Startup and live `auto`/HDR resolution unified with real NVENC regressions passing; WoA verification of new codecs, Main10, and physical HDR pending |
| Networking | Waybridge TLS, encrypted UDP, packet repair, TCP fallback; delivery feedback drives UDP pacing and NVENC bitrate; external SSH can forward TLS | Reachable UDP address configured manually; FPS adaptation pending; global media ordering can block across windows |
| I/O responsiveness | Bounded file queues on both sides, connection/selection generations, dedicated file channel, pre-capture backpressure, dual-side media budgets | Legacy peers use the older file message path; disconnect resume scheduled for M4 |
| Control & diagnostics | Control Center + remote SNI trays, native notifications, read-only toggle, sharing toggles, live FPS/bitrate, active pipeline, stage percentiles/frame age/raw queue metrics, log export | XEmbed and wider app matrices pending; no in-game HUD overlay; stage timings and RTT do not equal physical input-to-display latency |
| Config & app launching | Linux TOML defaults/limits/source inspection, per-desktop-ID overrides, GIO fields/actions, `HOME`/`Path`/`cwd` rules | No GUI editor on Windows (Linux TUI has full editor); `action` and `uris` are mutually exclusive; single-instance forwarding cannot force overrides onto existing processes |
| Desktop services | Shared user D-Bus/data, activation environment, standalone systemd targets; native wallets and GTK Portal via standard config | Components are user-chosen and explicitly installed; legacy isolated-wallet migration and Flatpak document-portal grants unverified; ScreenCast is M4 |
| Apps & platforms | Native Wayland, private Xwayland, Windows x64 and native ARM64 builds; native desktop secret services inside session | X11 lacks `PRIMARY`, XIM, and full EWMH; XDND scope is limited to documented verified paths; building cleanly does not guarantee every app/hardware combo |

Experimental native multitouch, pen, and gamepads are opt-in while M3 expands physical device and application verification. M2 has landed rich-text clipboard and single-channel microphone uplink. Webcam, window sharing, and printing are scheduled for M4.

Current file limits are 1 GiB per file and 8 GiB / 512 entries per directory batch, with strict Windows path/name, symlink, and reparse-point rules. See [Data Exchange](README.md#data-exchange) and [PROTOCOL.md](PROTOCOL.md).

## Existing Capabilities Prioritized for Hardening

| Area | User Outcome | Acceptance Criteria |
|---|---|---|
| Reliable, diagnosable Control Center | Extract, connect, launch apps, minimize to tray, disconnect, and exit cleanly; surface clear diagnostics on failure | Complete end-to-end flow on physical Windows x64 and WoA |
| Core application compatibility | Everyday productivity apps and games handle input, rendering, audio, and focus transitions reliably | Fixed app list covering CJK IME, copy/paste, menus, fullscreen, and pointer-lock release |
| Network drop recovery | Reattach cleanly to running sessions after brief outages or client restarts | Verify on both LAN and VPN for window restoration, input release, and sharing state |
| Transparent settings & fallbacks | Desired config, active state, and fallback reasons always agree | Verify `auto`/HDR/codec combinations and warnings when capabilities are missing |
| Multi-window everyday feel | Switching, dragging, resizing, hiding, and restoring stay predictable even during active file transfers | Run office apps + games alongside concurrent file transfers and window resizes |

## New and Enhanced Capabilities

### High-Priority Experience Tracks

| Feature | User Value | Difference from Earlier Baseline |
|---|---|---|
| Office clarity mode | Crisp small/colored text and gridlines with low static bandwidth | Goes beyond manual PNG by introducing policy + UI presets independent of one hardcoded codec |
| Interactive creative graphics | Smooth Blender viewports and GIMP canvases while keeping toolbars, fine lines, and colors legible | Verified on real creative software rather than static text or single fullscreen video alone |
| Low-latency gaming mode | Consistent input response and frame pacing while keeping foreground games sharp | Builds on existing video encode, pointer lock, and fullscreen with explicit presets and targets |
| Per-application settings | Remember visual, FPS, scale, input, and audio preferences per app rather than only per connection | Replaces purely connection-wide settings |
| Automatic network adaptation | Automatically adapt quality across LAN and VPN and explain stalls | Reduces reliance on manual bitrate, FPS, and UDP tuning while keeping reconnect recovery |
| File transfer management | View progress, speed, remaining bytes, cancel transfers, and cap transfer bandwidth | Extends activity history with active controls; pause/resume scoped separately |
| Favorites & quick launch | Pin apps in the Control Center and launch paired workstation apps via `waybridge://` links, Start Menu, or Desktop shortcuts | Links bind to local paired profiles and confirm before switching active workstations |
| Remote application trays | Show Linux app icons, status, activation, and menus in the Windows notification area so background apps can reopen or exit cleanly | SNI/DBusMenu and background list landed; legacy XEmbed and wider app/Shell combos pending |
| Shortcut routing settings | Control whether `Alt+Tab`, `Win`, etc. stay local or go remote while keeping emergency unlock | Makes shortcut routing explicit and user-configurable |
| Full multi-monitor experience | Move windows cleanly across mixed-DPI displays with accurate sharpness, sizing, and fullscreen targets | Establishes an explicit multi-monitor device matrix and consistent behavior |
| A/V sync & audio recovery | Lip-synced video playback and automatic recovery across headphone hotplug or audio channel drops | Upgrades current best-effort playback with sync and hot-switching |

"Clarity mode" and "low-latency mode" define user outcomes; direct encoding, partial updates, surface layering, codecs, and transports are implementation candidates evaluated in [ARCHITECTURE.md](ARCHITECTURE.md).

### Icons and Remote Identity

Remote windows and SNI tray icons display a configurable upper-right source badge per connection. The Control Center catalog and Windows notifications display original app icons; the Control Center itself and its tray use the primary Waybridge brand icon.

The initial brand mark is a white arch bridge on blue: rounded square for the primary app icon and a compact circular badge for overlays. Vector sources live at [primary icon](../assets/branding/waybridge.svg), [badge](../assets/branding/waybridge-badge.svg), and [monochrome mark](../assets/branding/waybridge-mark.svg).

- Title bars, taskbar buttons, and Alt+Tab use badged window icons; Windows notification area uses badged remote tray icons.
- Badges sit in the upper-right corner at ~40% of icon edge length, composited at the exact target pixel size per DPI (verified at 16/24/32/48/64 px) with a subtle outline for contrast on light, dark, and similar-colored icons while checking legibility alongside app status emblems.
- Default badge is the Waybridge mark; users can import a custom PNG/JPEG/BMP/ICO badge per connection (normalized to 64×64 PNG with aspect ratio preserved), replacing the default badge as a client-only visual customization without altering Linux files.
- When a remote app provides no icon, Waybridge uses a neutral app placeholder plus the badge to avoid double-badging the Waybridge mark.
- Icon updates, badge changes, DPI switches, and reconnects rebuild composites while caching raw and badged variants separately. Dynamic tray state remains app-driven; notification images composite on creation/replacement without re-toasting already shown notifications on appearance changes.

Assets are embedded into both the native EXE and Control Center bundle and configurable live under **Desktop Integration → Remote icon badge**. Native windows set per-profile, per-app `AppUserModelID` taskbar identities so different apps in one worker never merge into one taskbar group (pinning/relaunch shortcuts remain deferred). See [GUI.md](GUI.md).

### Confirmed New Capabilities

Gamepads, multitouch, stylus pen, microphone uplink, webcam, rich-text clipboard, and printing are all confirmed in scope. Recommended defaults, supported subsets, and acceptance criteria appear below under [Recommended Support Baseline](#recommended-support-baseline), mapped to milestones in [ROADMAP.md](ROADMAP.md).

### Other Candidates

| Feature | Optional Scope | Open Questions |
|---|---|---|
| Remote file browser & pull download | Browse selected Linux directories from the Control Center to upload/download directly | Whether a dedicated browser is needed beyond clipboard and drag-and-drop |
| Open links or files locally | Let users open remote links in Windows browsers or pull files into local Windows apps | Type routing rules and per-action vs. persistent confirmation |
| Launch groups / workspaces | Save a group of apps, arguments, and directories to restore a workspace in one click | Whether launch list alone suffices vs. in-app document state |
| Cross-device handoff | Take over a live session from a second Windows device with explicit takeover semantics | Whether single-user alternating access suffices vs. concurrent multi-viewer |

### Future Exploration

- **Concurrent multi-viewer / collaborative control**: requires explicit control-transfer, sharing, and permission semantics beyond the single-viewer model.
- **Scanners and generic USB passthrough**: scoped separately from printing, touch, and pen.
- **Higher display tiers**: sustained high refresh rates (1–120 FPS is configurable today without certifying sustained 120 FPS), sustained 4K60, and full HDR/wide-gamut pipelines calibrated per target hardware.

## Recommended Support Baseline

Adjustable engineering baselines and verification targets—these numbers represent test targets rather than claims of current completion across every device.

### Platform and Core Experience

- Linux server baselined on NVIDIA hosts; Windows 11 native ARM64 and x64 as primary clients, with Windows 10 tracked separately.
- Windows clients connect over Waybridge TLS to the Linux user service on LAN/VPN; system SSH handles deployment, administration, and optional TLS forwarding. Initial Windows 11 x64/Tailscale passed; WoA, wider networks, and full app matrices continue under [Roadmap M1](ROADMAP.md#m1-standalone-sessions-and-connectivity).
- Host-DE-independent startup verified with no local graphical desktop running and one-way rejection when a same-UID local desktop exists; see [Session Independence Principles](ARCHITECTURE.md#session-independence-principles).
- Apps (and child windows spawned by Steam/HMCL) launch inside the session with one active viewer per session.
- Initial multi-window workload: four concurrent toplevels (one continuously updating foreground window plus productivity windows) across dual displays at mixed DPI (100%, 175%, 200%), surfacing X11 shared-density behavior transparently.
- Standard dynamic video profiles: 1080p60 and 1440p60; 4K static office clarity tested separately; 1080p120, sustained 4K60, HDR, and wide gamut tracked as enhanced tiers.
- Initial color baseline: SDR/sRGB checking text, fine lines, alpha, and color; full ICC and HDR tracked separately.
- Three per-app saved presets (clarity, interactive workstation, low-latency gaming) with manual override, decoupled from specific codecs or compositing strategies.
- Default shortcut routing keeps `Alt+Tab` and `Win` local on Windows while forwarding ordinary app shortcuts remotely, with `Ctrl+Alt+Shift+F12` permanently reserved as local emergency unlock.
- Diagnostics inspect and display real device names, input capabilities, A/V formats, and print queues rather than hardcoding hardware brand lists.

### Baseline for the Seven New Capabilities

| Capability | Recommended First Complete Baseline | Acceptance Criteria |
|---|---|---|
| Gamepads | Standard Xbox-style layout (dual sticks, analog triggers, D-pad, standard buttons, dual-motor rumble); up to 4 controllers, hotplug, reconnect, deadzone; zero inputs & stop rumble on unfocus/disconnect | At least 2 physical controllers simultaneously (4-slot verified with controlled sources); Steam, one native Linux game, and one Windows game checked for discovery, input, and rumble |
| Multitouch | Native touchscreen input up to 10 contacts (hardware-limited); per-app native touch vs. mouse/gesture compatibility, defaulting to native | Single-touch, two-finger, and multi-contact independence; accurate cross-DPI coordinates; clean cancel/unfocus recovery; target app gestures work |
| Stylus pen | Distinct pen identity, absolute coordinates, tip/proximity, continuous pressure, hover, barrel button, eraser; tilt & rotation forwarded when hardware-valid; adjustable pressure curve | Verified in GIMP and Blender for accuracy, pressure variation, lift, and window switching; tilt/buttons verified per hardware without collapsing pen into plain mouse clicks |
| Microphone | Single active mic at 48 kHz mono voice baseline; device selection, gain, mute, push-to-talk, hotplug, reconnect | Recognized and sustained in remote Chrome call test + one conferencing/voice app; headset scenario; silence/stop on mute or disconnect |
| Webcam | Single active camera at 720p30 default (1080p30 optional when hardware/link permit); device selection, switching, disable, active indicator | Selectable for continuous preview and 30-minute simultaneous webcam + mic call in Chrome + one conferencing app with clean reconnect recovery |
| Rich-text clipboard | Bidirectional HTML and RTF with plain-text and PNG fallbacks covering multilingual text, basic typography, paragraphs, links, tables with basic styling, and embedded static images | Bidirectional copy/paste of fixed text/image/table fixtures across Chrome, WPS, and LibreOffice preserving editable text and structure (equations/charts/proprietary embeds tracked separately) |
| Printing | Print from Linux apps to selected Windows print queues plus Save to PDF; paper size, orientation, page ranges, copies, and printer-supported color/duplex; job status & cancel without duplicate reprints on reconnect | Print CJK text, images, and tables from WPS or LibreOffice to both a physical printer and PDF, checking Windows queue status and physical output |

Gamepad baseline references [Windows XInput four-controller conventions](https://learn.microsoft.com/en-us/windows/win32/xinput/getting-started-with-xinput) for scope (independent of backend API choice). Touch events follow [Wayland `wl_touch` semantics](https://wayland.freedesktop.org/docs/html/apa.html), separate from app-level gesture mapping. Pen axes honor Windows validity masks per [Pen Mask Constants](https://learn.microsoft.com/en-us/windows/win32/inputmsg/pen-mask-constants) and [`POINTER_PEN_INFO`](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-pointer_pen_info) rather than treating zero defaults as supported axes. Rich-text HTML follows [Windows HTML Clipboard Format](https://learn.microsoft.com/en-us/windows/win32/dataxchg/html-clipboard-format), verified per app pair. Print capabilities query live queue attributes per the [PWG IPP model](https://www.pwg.org/ipp/ippguide.html).

### Application and Desktop Service Integration

Users should complete target workflows entirely from Windows without ever walking over to the Linux host desktop for a hidden file picker, wallet prompt, or dialog. Service installation, D-Bus reachability, and end-to-end interactive completion are recorded separately.

| Scenario | Recommended Default Behavior | Acceptance Focus |
|---|---|---|
| KWallet / Secret Service | Native secret service and existing data on the shared user bus; unlock/auth prompts render as remote windows; no wallet forwarding | Temporary test wallets covering unlocked, locked, first-time auth, cancel, service restart, and headless startup; alternating local/remote use with creation check |
| Open & save files | Keep native Linux file dialogs; cross-machine transfers use drag/copy with uniform read-only delivery; users copy via Dolphin/Explorer into editable directories | Verify native Qt/GTK and Flatpak local dialogs separately; confirm delivered inputs resist in-place overwrite while user copies are writable; no forced Save As or auto write-back |
| Links & cross-app launches | Stay inside the remote session by default with explicit "Open in Windows" option; login callbacks stay in the remote browser; custom URIs route to remote apps | Test `https:`, `mailto:`, `steam:`; confirm OAuth/login callbacks return to the calling app rather than a host process |
| Remote app trays | Independent Windows tray icons with primary/secondary activation, menus, and status updates plus Control Center background list | Verify icons, tooltips, submenus, checked/disabled items, reopen/exit after closing main window, and clean removal on exit/disconnect/reconnect |
| Desktop notifications | Map Linux notifications to Windows native notifications with click/action callbacks, replacement, close, and expiry | Verify app attribution, actions, close events, sharing toggles, Windows Do Not Disturb, and no stale replay on reconnect |
| Background apps & window close | Surface managed background apps in Control Center; keep window close, hide-to-tray, clean exit, disconnect, and session shutdown distinct | Zero visible windows ≠ exited; restore via app activation interface; invoke app-provided quit rather than killing same-UID processes by name |
| Taskbar identity | Group windows by remote app name and icon while preserving parent/modal relationships | Separate Waybridge Control Center from remote apps; keep identity consistent across multi-instance, reconnect, foreground activation, and DPI changes |
| Flatpak & Portals | Provide working file-picker and URI-open Portal flows inside the session; enforce sharing toggles for devices and printing | Verify installed Flatpak apps (Feishu, OBS, etc.) separately from native packages |
| Conferencing screen sharing & capture | Prioritize sharing individual windows inside the Waybridge session (sharing the Windows desktop is separate) | Conferencing app or OBS can select, preview, and stop sharing a Blender window with auth prompts visible in-session |
| System authorization (polkit) | Show auth dialogs in-session for selected apps or report unsupported cleanly without weakening host rules | Bind auth agent properly to real Linux login session/process with working cancellation |
| SSH / GPG & dev tools | Use existing Linux identities and agents with in-session prompts (Windows agent forwarding is optional) | Verify Git, SSH passphrases, and GPG signing independently rather than assuming KWallet covers agents |

Implemented session environment and lifecycle code lives in [session_services.rs](../src/session_services.rs) and [INTEGRATION.md](INTEGRATION.md). Native Secret Service/KWallet on the user bus handles unlocking natively (SSH key login does not supply wallet passwords). KWallet 6.31.0 pairs `kwalletd6` compatibility with the `ksecretd` backend. See [SESSION_POLICY.md](SESSION_POLICY.md) for alternating local/remote data sharing.

Portal backends and service activation environments must stay synchronized (see [Portal System Integration](https://flatpak.github.io/xdg-desktop-portal/docs/system-integration.html) and [`portals.conf`](https://flatpak.github.io/xdg-desktop-portal/docs/portals.conf.html)), preserving Flatpak document-portal grants; Windows paths are never passed directly as Linux-readable paths. Conferencing window sharing additionally requires [ScreenCast Portal + PipeWire](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.ScreenCast.html), and [polkit](https://www.freedesktop.org/software/polkit/docs/latest/polkit.8.html) operates over the system bus with session-bound agents rather than plain session-bus forwarding.

Wallets and basic file pickers are verified; M2 landed the initial wave of trays, notifications, and background entries while expanding link/file flows; single-window ScreenCast lands in M4.

### Desktop Integration Delivery Checklist

Distinguishes current capabilities from remaining targets:

| Capability | Current Status | Planned Scope & Boundaries |
|---|---|---|
| SNI app trays & menus | Initial wave landed: SNI, DBusMenu, independent Windows icons | M2: StatusNotifierItem Host/Watcher + DBusMenu, independent Windows icons/status/actions, icon updates, hide/reopen, exit, and reconnect |
| Legacy X11 trays | Not implemented | Probe XEmbed/System Tray per target app; pull forward if required before M4 full app sign-off (SNI does not substitute) |
| Notifications & action callbacks | Initial wave landed: opt-in standard provider + Windows native notifications/actions | M2: Standard notification spec + Windows presentation, identity, replace/close, actions, and Do Not Disturb; no offline backlog by default |
| Background app entries | SNI-based background list, restore, and app menus landed (non-SNI processes excluded) | M2: Track manageable apps and tray/window relationships; surface running background apps with native activate and quit |
| Taskbar grouping & activation | Per-profile/app `AppUserModelID` landed; full Shell matrix pending | M2: Complete app identity mapping, window restoration, and grouping; verify multi-instance and modal relationships |
| Files / URIs & login callbacks | GIO launches, partial file pickers, and read-only cross-machine delivery + writable user copies landed | M2: Keep native open/save, expand app compatibility, verify URIs/login callbacks; explicit "Open in Windows" without hijacking global defaults |
| Global shortcuts & media keys | Basic keyboard routing exists; full config and backends missing | M2 defines local/remote shortcut rules; GlobalShortcuts Portal and MPRIS media control tracked as optional subsets |
| Lock, sleep & resume | Full handling and test matrix pending | M2 adds input release, session preservation, and reconnect across Windows power/lock transitions; M3/M4 extend to new peripherals |
| Auth & dev-tool interactions | Wallets wired; other auth paths not assumed | Verify polkit and SSH/GPG agent prompts individually using standard user configuration |

Trays prioritize [StatusNotifierItem](https://specifications.freedesktop.org/status-notifier-item/latest-single/) with [X11 System Tray](https://specifications.freedesktop.org/systemtray/latest-single/) (XEmbed) tracked separately; items offering only `ContextMenu` without exportable menus verify Linux-window fallback rather than assuming native Windows menus for every app. Shared user buses respect existing tray watchers and [notification service](https://specifications.freedesktop.org/notification/latest-single/) owners without hijacking or snooping.

Theme/font sync, autostart/launch bundles, MPRIS, global menus, and auto-updates remain separate candidates rather than v1 commitments.

### Default Sharing Behavior

Microphone and webcam are opt-in per session with visible active indicators, stopping transmission on disconnect and recovering cleanly from transient local device faults. Gamepads, touch, pen, clipboard, and printing each have independent toggles showing active state across connection switches.

Compatibility results are tracked separately for native Wayland, Xwayland, Windows game runners, and Flatpak apps, pairing peripherals to representative apps (Blender/GIMP for pen, games for gamepads, conferencing apps for A/V, WPS/LibreOffice for rich text and printing).

### Read-Only Delivery of Cross-Machine Files

Implemented 2026-11-16: cross-machine files are always delivered as read-only inputs. Linux exposes completed caches via a read-only FUSE view; Windows protects materialized clipboard caches with NTFS DACLs; OLE virtual-file streams reject writes. Drag-and-drop `COPY` alone is never treated as sufficient read-only enforcement.

- Receiving apps can read and import delivered files but cannot overwrite them in place or atomically replace them via temp-file rename; source files stay untouched.
- To keep or edit files persistently, users drag or paste them into Dolphin or Explorer to copy into their own directory; the resulting user copy has normal writable destination permissions without propagating read-only restrictions.
- One uniform file/directory/virtual-stream adapter serves all apps without inspecting app names or extensions, synthesizing unnamed projects, or intercepting Save As shortcuts. Apps handle Save As natively if an in-place save fails.
- Receive caches accommodate lazy reads, reopens, and random seeks; user copies survive cache cleanup. On-demand network FUSE reads remain a future optimization.
- Waybridge does not add transparent remote drive letters, replace native file dialogs globally, or auto-sync edits back to the sender.

Read-only delivery is not a same-UID/admin security sandbox (it does not block a user from altering ACLs, unmounting, or bypassing public paths). Linux requires `fusermount3` and accessible `/dev/fuse`; Windows temp directories require NTFS DACLs that do not map to a propagating `READONLY` attribute. When protection is unavailable (including under Wine's attribute mapping), publication is rejected and staged caches are cleaned up rather than downgrading to writable delivery; Wine uploads and read-only virtual streams remain usable.

Acceptance covers: directly dropped projects cannot overwrite or atomically replace in place; images/assets still read and import cleanly; copies made through file managers edit and save normally; source files stay intact; cancels, disconnects, and cache cleanups never delete user copies.

### Large Projects and File Exchange

Target lifting the fixed 1 GiB single-file limit in favor of user-configured quotas and available disk space, testing at 10 GiB single files, 50 GiB project trees, and 10,000-file batches (test scales, not new hard caps).

Transfer management provides per-task progress, speed, cancellation, rate limiting, and partial-completion reporting on error. Resuming interrupted transfers across network drops while the remote session remains alive is part of the full specification (resume across process restarts is tracked as an enhancement). Committed files remain readable across viewer disconnects.

### Display and Network Acceptance Targets

| Scenario | Controlled Network Conditions | Target Behavior |
|---|---|---|
| LAN | RTT ≤ 5 ms, bandwidth ≥ 100 Mbit/s, 0% injected loss | Sustained 1080p60 and 1440p60 whenever the server app produces frames; crisp readable office text |
| Typical VPN | RTT 50 ms, bandwidth 30 Mbit/s, 1% random loss | Optimize for 1080p60 with automatic quality scaling; input, audio, and file controls remain responsive |
| Constrained VPN | RTT 100 ms, bandwidth 10 Mbit/s, 2% random loss | Automatically scale down quality or 720p30 (user-overridable); bounded queues and resources with responsive interaction |

Initial physical input-to-display latency targets (to be calibrated against real hardware baselines): P95 ≤ 80 ms on LAN and P95 ≤ 130 ms on typical VPN, measuring actual server-rendered visual response to input (excluding local client cursor motion).

Record local baselines alongside remote overhead for real games and Blender; never sum stage timers, RTT, and presentation submit times as physical latency. Target |A/V sync offset| ≤ 80 ms for video playback and conferencing, likewise calibrated empirically.

Reconnect reliability: 10 repetitions each of 1 s, 5 s, and 30 s network outages reattaching within 10 s once connectivity returns with zero stuck keys, touches, pen contacts, or rumble. Stability: 8-hour workstation soak with repeated window open/close confirming bounded steady-state Waybridge resource use.

### Initial Representative Application Acceptance Suite

| Suite | Target Workflows |
|---|---|
| Blender & GIMP | Open, edit, and save projects; 3D viewport and large canvas navigation; menus, Unicode paths, pen pressure, continuous input, and window switching |
| Minecraft & HMCL | Launch from HMCL, load modpacks, camera look and inventory/menu switches, chat input, pointer-lock release, and return to launcher |
| Brotato or Stardew Valley | Native Linux game keyboard/mouse, gamepad, audio, fullscreen, and reconnect |
| Cyberpunk 2077 | Windows game runner display, keyboard/mouse, gamepad, audio, and fullscreen (after verifying local playability), plus parallel microphone voice chat |
| Split Fiction | Dual independent gamepads, hotplug, rumble, and foreground/background switches ([Steam page](https://store.steampowered.com/app/2001120/Split_Fiction/)) |
| WPS, LibreOffice & Chrome | Multilingual rich-text/image/table copy-paste, file transfers, PDF and physical printing; Chrome + conferencing app for webcam and microphone |

Additional titles expand the matrix incrementally with runner versions and measured outcomes, keeping game-runner issues distinct from Waybridge issues.

## Recommended Delivery Batches

Foundational work and full milestones are tracked in [ROADMAP.md](ROADMAP.md): M0 consolidates the core, M1 delivers standalone sessions and connectivity, and the three batches below map to M2, M3, and M4.

| Batch | Scope | Exit Milestone |
|---|---|---|
| Batch 1 — Daily Workstation | GUI & window/input polish, Blender & initial apps, wallet & file-picker integration, clarity/low-latency presets, network adaptation, rich text, microphone, basic transfer management | Everyday productivity and core gaming work repeatably end-to-end |
| Batch 2 — Professional Input & Gaming Devices | Gamepads & rumble, multitouch, stylus pen with hardware-valid axes, and per-app configs | Creative pen workflows, dual-gamepad play, and input cancellation/recovery pass on physical hardware |
| Batch 3 — Full Collaboration | Webcam, A/V sync, single-window conferencing share, Windows print queues, project-scale transfers & resume | Conferencing, printing, and large project workflows pass; all seven new capabilities meet baseline |

Batches 1 and 2 serve as usable previews; the first full stable release follows completion of all three batches. High refresh/HDR extensions, advanced controller features, multi-camera setups, and broader device matrices follow in subsequent phases.

## Feature Scoping Process

Every feature records target user workflows, current status, v1 necessity, supported subset, and acceptance criteria. Candidate features enter version plans only after explicit selection, with architecture updated to cover cross-feature interactions.

Ongoing work marks each baseline item as adopted, adjusted, or deferred, logs exact device capabilities and game runners, and calibrates numeric targets against controlled baselines.

For current usage limits, see [Usage and Building](README.md) and [Control Center](GUI.md).
