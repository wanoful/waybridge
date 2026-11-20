<p align="center">
  <img src="assets/branding/waybridge.svg" alt="Waybridge" width="84" height="84" />
</p>

<h1 align="center">Waybridge</h1>

<p align="center">
  <strong>Standalone Linux → Windows remote application sessions with native per-app Windows windows.</strong>
</p>

Waybridge runs Linux Wayland and X11 applications inside a private, headless [Smithay](https://github.com/Smithay/smithay)-based Wayland compositor and streams each remote toplevel as an independent native Win32 / DirectComposition window on Windows—built for creative workstations (such as Blender and GIMP), everyday productivity, and gaming over LAN or VPN.

> **Status:** Active development prototype (`0.3.0`). Uses an independent v3 wire protocol (incompatible with standard Moonlight clients). Update and restart both Linux and Windows ends together.

## Highlights

- **Independent native Windows windows** — Each remote Linux toplevel maps to its own Win32 window with native DPI scaling, fractional scaling, per-pixel transparency, owned popups/menus, modal dialogs, `xdg-decoration` negotiation, and `xdg-toplevel-drag-v1` (e.g., Chrome tab tear-off and merge).
- **No host desktop environment required** — Runs a standalone headless Smithay compositor with optional private rootless Xwayland (`--xwayland`), managing its own session environment and optional systemd user targets without requiring KWin, Plasma, or GNOME Shell.
- **Hardware-accelerated display pipeline** — NVENC H.264 / HEVC / AV1 with independent lossless alpha, PNG lossless fallback, optional NVIDIA GL → CUDA → NVENC color input, experimental scRGB Main10 HDR subset, and D3D11 / Media Foundation / DirectComposition presentation on Windows (x64 & native ARM64).
- **Authenticated TLS connectivity & adaptive UDP** — Linux user-level service (`waybridge-service`) with pinned-certificate TLS pairing, Windows Credential Manager token storage, optional AES-256-GCM UDP/FEC media channel with delivery-feedback pacing and NVENC bitrate coordination, and full `waybridge-service tui` terminal administration.
- **Native desktop integration** — Windows Control Center (C# / .NET 10 / WinUI 3) with app wall, global search, `waybridge://` app links, Windows Start Menu/Desktop shortcuts, remote SNI/DBusMenu system trays, native Windows notifications, customizable per-connection icon badges, and `AppUserModelID` taskbar grouping.
- **Language and appearance** — The Control Center supports English and Simplified Chinese. Its appearance setting also updates native streamed window captions and supported Linux apps through the XDG Settings portal.
- **Rich input & peripherals** — Windows native IME → Wayland `text-input-v3`, Raw Input relative mouse & pointer lock (`Ctrl+Alt+Shift+F12` emergency unlock), native cursors, WASAPI stereo audio playback, optional mono microphone uplink, plus opt-in experimental native multitouch, Precision Touchpad gestures, `tablet-v2` pressure pen, and 4-slot SDL3 → `uinput` gamepads with rumble.
- **Safe cross-machine data exchange** — Bidirectional text, HTML, RTF, PNG, file, and directory clipboard/drag-and-drop over a dedicated file channel with uniform read-only cross-machine delivery (read-only FUSE view on Linux, NTFS DACLs on Windows) and on-demand virtual-file drag-out.

## Architecture & Repository Layout

```text
waybridge/
├── src/          # Headless Linux Wayland compositor (Rust / Smithay) + Xwayland, audio, DnD, trays
├── native/       # Native FFmpeg/NVENC codec wrapper and optional CUDA GL interop kernels
├── config/       # Shared validated Linux TOML configuration crate
├── service/      # Authenticated TLS user entry point (waybridge-service) and ratatui TUI
├── client/       # Native Windows C++ engine (Win32, D3D11, DirectComposition, Media Foundation, OLE, WASAPI, SDL3)
├── gui/          # Windows Control Center (C# / .NET 10 / WinUI 3) + Core library & tests
├── integration/  # Optional systemd user targets, notification provider unit, and xdg-desktop-portal preset
├── assets/       # Brand vector/raster icon assets
├── scripts/      # Build, cross-compile, packaging, session control, and test scripts
├── tests/        # Automated Linux, Wine/Xvfb, and opt-in physical Windows verification suites
├── vendor/       # Vendored Smithay 0.7.0 with documented Waybridge patches (see vendor/smithay/PATCHES.md)
└── docs/         # Architecture, protocol, configuration, feature, and validation documentation
```

## Quick Start

### 1. Build Linux Server & Native Windows Client

On Linux (requires Rust, EGL/GLES, `libdrm`, Wayland, `xkbcommon`, FFmpeg dev headers, `pkg-config`; plus MinGW-w64 / LLVM-MinGW, CMake, and Ninja for cross-compiling Windows binaries):

```bash
# Build Linux compositor + user service (and Windows x64 engine via MinGW)
./scripts/build.sh

# Optional: cross-compile native Windows ARM64 engine (requires LLVM-MinGW)
./scripts/build-windows-arm64.sh

# Optional: build a portable Windows bundle (requires .NET 10 SDK)
./scripts/build-control-center.sh x64   # or arm64 / all

# Build portable bundles and Windows installers (also requires NSIS 3.11+)
./scripts/build-control-center.sh all --installer
```

### 2. Start the Linux Service and Pair

On Linux (log out of any local graphical desktop for the same user before creating a session; see [Session Policy](docs/SESSION_POLICY.md)):

```bash
cargo build --release --workspace --locked
./target/release/waybridge-service serve --compositor "$PWD/target/release/waybridge"

# In another trusted Linux terminal, generate a one-time pairing code & fingerprint:
./target/release/waybridge-service pair --expires 120

# Or open the interactive server management TUI:
./target/release/waybridge-service tui
```

On Windows, run `Waybridge-Setup-win-x64.exe` or `Waybridge-Setup-win-arm64.exe` and open **Waybridge** from Start. For a portable copy, extract the entire matching `Waybridge-Control-win-*.zip` and run `Waybridge.Control.exe`. Add your Linux workstation using its LAN/VPN address, port `49092`, the verified `certificate_sha256` fingerprint, and the one-time pairing code. Then connect and launch applications.

For a full step-by-step Linux host walkthrough (distro packages, `systemd --user`, `loginctl enable-linger`, firewall, Portals, notifications, Chrome/KWallet overrides, and `/dev/uinput` gamepads), see the **[Linux Host Setup Guide](docs/LINUX_SETUP.md)**. For direct low-level CLI + SSH tunnel usage, see [Usage & Build Guide](docs/README.md).

## Testing

Run the main automated Linux verification suite (uses isolated temporary user buses, simulated `logind`, and temporary user directories so your active desktop is untouched):

```bash
./scripts/test.sh
dotnet run --project gui/Waybridge.Control.Tests -c Release
```

For full M0/M1 Wine regressions, GPU/HDR benchmarks, and targeted smoke tests, see [Running Tests](docs/README.md#running-tests).

## Documentation

### Getting Started & Operations
- [Linux Host Setup Guide](docs/LINUX_SETUP.md) — Step-by-step Linux installation, `systemd --user`, `linger`, firewall, Portals, overrides, and troubleshooting
- [Usage, Architecture Summary & Build Guide](docs/README.md) — End-to-end setup, CLI flags, codecs, UDP, CUDA/HDR, building, and testing
- [Windows Control Center](docs/GUI.md) — Launcher UI, settings, packaging from source, and startup diagnostics
- [Linux User Service & TUI](docs/SERVICE.md) — TLS pairing, trust management, systemd service, and terminal UI
- [Configuration & Application Launching](docs/CONFIGURATION.md) — Linux TOML config, Waybridge-specific `.desktop` overrides, and working-directory rules
- [Desktop Components & Session Lifecycle](docs/INTEGRATION.md) — Installing systemd session targets, Portal preset, and opt-in notification provider

### Architecture, Policies & Planning
- [Architecture Baseline](docs/ARCHITECTURE.md) — System design, process/module boundaries, state ownership, and pipeline research
- [Feature Scope & Implementation Status](docs/FEATURES.md) — Current capabilities, target baselines, and representative app/game matrix
- [Roadmap](docs/ROADMAP.md) — Delivery milestones (M0–M5), current progress, and next work items
- [Configuration Policy](docs/CONFIGURATION_POLICY.md) — Configuration ownership and desktop integration boundaries
- [Session & User Data Policy](docs/SESSION_POLICY.md) — Shared user data and one-way session admission rules
- [Launcher & Server TUI Design](docs/LAUNCHER_AND_TUI_DESIGN.md) — Client launcher, batch icons, app links, and TUI specification

### Subsystems & Wire Protocol
- [Wire Protocol (v3)](docs/PROTOCOL.md) — Implemented control, media, audio, IME, drag-and-drop, tray, and notification messages
- [Media Resource Budgets](docs/MEDIA_BUDGET.md) — Linux & Windows memory, texture, and codec budgeting and eviction
- [Network Experience](docs/NETWORK.md) — UDP delivery-feedback pacing and NVENC bitrate adaptation
- [File Transfers](docs/FILE_TRANSFERS.md) — Dedicated file data channel, progress, cancellation, and rate limiting
- [Desktop Interactions](docs/DESKTOP_INTERACTIONS.md) — Window activation, trays, and drag-and-drop support matrix
- [Application Trays](docs/DESKTOP-TRAY.md) — StatusNotifierItem / DBusMenu adapter and background app entries
- [Drag-and-Drop Design](docs/DRAG_AND_DROP.md) — Immutable offers, synchronous OLE bounds, and touch drag handoff
- [X11 / XDND Bridge](docs/XDND.md) — Rootless Xwayland drag-and-drop bridging and mixed-backend semantics
- [Professional Input & Multitouch](docs/INPUT_DEVICES.md) — Native multitouch, window/popup/drag integration, and device coverage
- [Precision Touchpad](docs/TOUCHPAD.md) — Direct Manipulation two-finger scroll and pinch gestures
- [Stylus Pen](docs/PEN.md) — Windows `PT_PEN` → Wayland `tablet-v2` forwarding and pressure calibration
- [Gamepads](docs/GAMEPADS.md) — SDL3 discovery, 4-slot Linux `uinput` virtual controllers, rumble, and reconnect
- [Microphone Forwarding](docs/MICROPHONE.md) — WASAPI capture → Linux PulseAudio/PipeWire-Pulse virtual source

### Patches
- [Smithay Local Patches](vendor/smithay/PATCHES.md) — Documented patches applied to vendored Smithay 0.7.0

## License

Waybridge is licensed under the [MIT License](LICENSE). Third-party licenses for vendored Smithay ([`vendor/smithay/LICENSE.txt`](vendor/smithay/LICENSE.txt)) and `nlohmann/json` ([`client/third_party/json.LICENSE.MIT`](client/third_party/json.LICENSE.MIT)) are retained in their respective directories.
