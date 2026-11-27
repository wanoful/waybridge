# Waybridge Control Center

Linux sessions share the user's existing application and wallet data by default. Log out of any local desktop session for the same user before creating a new session (locking the screen is not enough). Auto-connect prefers reattaching to an existing Waybridge session and rejects creating a second one. Logging in through another display manager after Waybridge has started is left to that DM; see [Session Policy](SESSION_POLICY.md).

The Control Center is a C# / .NET 10 + WinUI 3 desktop app paired with the standalone C++ streaming worker. Installers and portable bundles are built for ARM64 and x64. Video decoding, DirectComposition, remote windows, IME, audio, and OLE remain in the native C++ worker. The GUI manages connections and settings over a current-user private named pipe.

## Getting Started

1. Run `Waybridge-Setup-win-x64.exe` or `Waybridge-Setup-win-arm64.exe` and open **Waybridge** from Start. To use the portable bundle, extract the entire matching ZIP and run `Waybridge.Control.exe`.
2. Both packages bundle .NET, the Windows App SDK, and the [Microsoft Visual C++ v14 redistributable](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist). The installer offers to run the redistributable if needed. In the portable folder, run the matching `vc_redist.x64.exe` or `vc_redist.arm64.exe` if the runtime is missing.
3. On Linux, build with `cargo build --release --workspace --locked` and start `./target/release/waybridge-service serve --compositor "$PWD/target/release/waybridge"`. Requires Python 3.11+, GIO, and the compositor's runtime dependencies. See [Desktop Integration](INTEGRATION.md) for installing systemd session targets.
4. In another trusted Linux terminal, run `./target/release/waybridge-service pair --expires 120` to generate a one-time pairing code and display the full `certificate_sha256` fingerprint. Never trust a fingerprint reported solely over an untrusted network response.
5. On Windows, add a workstation with the Linux LAN/VPN address, port `49092`, the verified full certificate fingerprint, and the one-time pairing code. Client tokens are stored in Windows Credential Manager. If the server certificate rotates, re-verify the fingerprint out-of-band and re-pair. On Linux, use `waybridge-service clients` / `revoke CLIENT_ID` to revoke trust; the GUI can also revoke the current device while disconnected.
6. Connect using the default session policy. Search and launch an application, or switch back to a running window from the list. Closing the viewer keeps remote apps running so reconnecting restores the live session. See [Service Documentation](SERVICE.md) for deployment and authorization details.

The Control Center connects to the Linux user service over Waybridge TLS. On networks reachable only via SSH, forward the TLS entry port with system SSH and point the Control Center at the local forwarded host/port while still verifying the Linux service's full certificate fingerprint—for example, `ssh -N -L 127.0.0.1:49092:127.0.0.1:49092 user@linux-host` when the service also listens locally on `49092`. UDP media still requires direct network reachability.

## Pages and Behavior

- **Top workstation bar** displays the selected workstation, connection status, and active session name. From here you can open a native Windows remote terminal, toggle read-only mode, adjust remote audio volume, inspect RTT and the active video pipeline, or open the transfer drawer and settings. Switching workstations disconnects the current viewer (and closes active Waybridge terminals) while leaving remote GUI apps running.
- **Native remote terminal (`waybridge.exe`)** opens directly in your default Windows terminal app via the top-bar terminal button or from the command line (`waybridge.exe shell --connection <name-or-id>`). It automatically starts Waybridge Control Center in the background if needed, ensures the graphical session is connected, and launches an interactive Linux shell over pinned TLS with the active Waybridge graphical and audio environment pre-configured so GUI commands run from the terminal appear on Windows. Disconnecting, switching workstations, ending the remote session, or exiting Control Center terminates active terminals immediately. Enable **Desktop integration → Sync profile to Windows Terminal** (off by default) to maintain a Windows Terminal JSON fragment (`%LOCALAPPDATA%\Microsoft\Windows Terminal\Fragments\Waybridge\profiles.json`) for that workstation.
- **Global search** focuses with `Ctrl+K` or `/`; type an app name, keyword, or window title and press `Enter` to launch or focus it. Prefix with `>` to run a command directly on the workstation, with an optional Linux working directory and `Tab` completion for recent commands. Arguments are passed literally to the Linux process.
- **App links and shortcuts** live in each app card's `…` menu. You can copy a `waybridge://launch/…` link, add an entry under **Start → All Apps → Waybridge**, or create a desktop shortcut carrying the app icon and workstation name. App links can also be opened from the Windows **Run** dialog.
- **Visual presets** in each app card's `…` menu are saved per workstation and application. **Text clarity** prefers lossless PNG and caps that window at 30 FPS. **Interactive low latency** prefers H.264 and prioritizes capture resources for that window over background windows. **Follow connection** restores connection-wide settings. Codec capabilities and server limits still determine the effective path; switching refreshes only the affected window.
- **File transfers** show progress, cancellation, and a shortcut to open the receive folder in the file drawer. Configure bandwidth limits under **Sharing & Permissions** (`0` means unlimited). See [File Transfers](FILE_TRANSFERS.md).
- **General → Display language** offers Follow Windows, English, and Simplified Chinese. Changes apply immediately during an active connection and preserve edits in the settings pages.
- **General → Appearance** offers Follow Windows, Light, and Dark. The effective theme applies live to the Control Center, native streamed captions, and Linux apps that consume the XDG color-scheme preference. See [Linux Portal setup](LINUX_SETUP.md#a-xdg-desktop-portal-file-pickers-and-appearance).
- **Microphone forwarding** is enabled on the **Audio** page (off by default), sending the default Windows microphone to remote **Waybridge Microphone**. See [Microphone Forwarding](MICROPHONE.md).
- App links bind to the local paired profile and remote application ID. Opening a link reuses the active connection or connects automatically to the target workstation, prompting for confirmation before switching away from another active workstation (whose remote apps stay alive). The Control Center can stay in the tray when an app launches cleanly. After re-pairing or removing a workstation, recreate links from the new profile.
- **Remote Apps** scrolls continuously from Pinned & Recent into All Apps. Once scrolled past the catalog header, refresh, category filters, and grid/compact layout toggles stick to the top of the viewport. Both grid and compact list show original app icons and wrap app names to at most two lines with trailing ellipsis. Clicking an app on a paired workstation connects first if needed. Card `…` and context menus offer Pin, Desktop Actions, touch policy, and custom launch preview. Catalog launches honor Linux [Waybridge desktop overrides](CONFIGURATION.md#waybridge-specific-desktop-overrides).
- **Running** in the fixed left navigation rail lists active remote windows and background SNI applications, letting you focus windows, restore background apps, or open their menus.
- **File Transfers** opens from the top bar to send files/directories, inspect transfer activity, and open the receive directory. When closed, a badge on the top bar indicates unseen transfer activity.
- **Diagnostics** shows active encode, decode, and presentation paths alongside RTT, TCP receive rate, window update counts, and per-stage timings, with log export. TCP receive rate covers TCP only, and timing metrics reflect their individual stages.

**Settings** in the left navigation is organized into **Display**, **Audio**, **Input Devices**, **Sharing & Permissions**, **Network**, **Connection**, **Desktop Integration**, and **General**. Changing Light, Dark, or Follow Windows under **General** updates the Control Center and title bar immediately. General settings are global; all other categories are saved per workstation. Pages supporting live changes offer **Apply to current connection**. Actual FPS, bitrate, and codec settings update on the **Display** page. Settings requiring reconnect (such as audio output device) offer **Save & reconnect**. Enabling UDP, HDR, or switching server GPU color input for the first time may require starting a new session.

Closing the window minimizes to the system tray; launching the app again brings back the existing window. The tray menu supports connect/disconnect, mute, diagnostics, and exit. **Disconnecting or exiting stops only the local worker and network connection—remote Linux applications keep running.** **End remote session** requires explicit confirmation and terminates only the same-user session matching the verified process identity.

Remote application trays appear as independent icons in the Windows notification area; **Running → Background apps** lets you open/restore an app or open its menu, where any Exit item comes from the app itself. Background processes without SNI do not appear in this list, and legacy XEmbed is not implemented. Native notifications respect Windows Do Not Disturb and Notification Center; Linux must explicitly opt into the Waybridge notification provider without colliding with another daemon (see [Desktop Integration](INTEGRATION.md) and [Application Trays](DESKTOP-TRAY.md)). **Desktop Integration** has independent toggles for desktop notifications, system media controls (bridging Linux MPRIS2 players to Windows Quick Settings, volume flyout, lock screen, and hardware media keys), remote app trays, and Windows Terminal profile sync; read-only mode continues displaying notifications and media metadata while blocking remote actions and playback controls.

Remote windows preserve original Linux application icons, compositing the Waybridge source badge in the upper-right corner for system title bars, taskbar buttons, and Alt+Tab (falling back to a neutral app placeholder when an app provides no icon). Icons painted inside application client areas are never touched. Taskbar grouping uses a per-profile, per-app `AppUserModelID` so windows of the same app group together; pinning and relaunch shortcuts are not yet implemented. The Control Center and its tray icon use the blue arch-bridge brand icon.

Under **Desktop Integration → Remote icon badge**, pick a PNG, JPEG, BMP, or ICO image to customize the badge composited onto streamed window and tray icons for the current workstation, then click **Apply to current connection**. **Use Waybridge icon** restores the default. Imported images are capped at 8 MiB, 8192×8192, and 16 megapixels, and are aspect-normalized into a 64×64 transparent PNG stored in the connection profile. Brand vector sources live in `assets/branding/`; run `python3 scripts/build-branding.py` (using `rsvg-convert` and Pillow) to regenerate PNGs, ICOs, and embedded native resources.

Under **Input Devices**, enable native multitouch, pressure-sensitive pen, or gamepads. **Save settings** works while disconnected; **Apply to current connection** saves and requests a live update immediately. Touch defaults to automatic detection of native `wl_touch` subscriptions, falling back to mouse compatibility otherwise. Expand **Rumble, pressure curve, and app compatibility** to tune per-app overrides; mouse compatibility supports long-press right-click and drag selection. See [Professional Input](INPUT_DEVICES.md).

Profiles are stored in `%LOCALAPPDATA%\Waybridge\profiles.json` (saving TLS fingerprints and client IDs), while client tokens live in Windows Credential Manager. Linux session logs live in `$XDG_RUNTIME_DIR/waybridge-control-center/<session>.log`. Diagnostics exclude session tokens and clipboard text. Create a fresh server session when enabling UDP or HDR for the first time.

Advanced input settings include pen pressure gamma (`0.25–4`, default `1`), rumble toggle, and stick deadzone (`0–50%`, default `15%`). The device status pane distinguishes local discovery, remote support, requested enable state, foreground focus, and actual forwarding, surfacing backend errors clearly. See [Native Pen](PEN.md) and [Gamepads](GAMEPADS.md).

## Building from Source

On Linux, install the .NET 10 SDK, CMake, Ninja, and MinGW x64 / LLVM-MinGW ARM64:

```bash
./scripts/build-control-center.sh arm64  # or x64 / all for portable bundles
./scripts/build-control-center.sh all --installer  # requires NSIS 3.11+
```

This produces `build/Waybridge-Control-win-arm64.zip` and `build/control-center-arm64/`. The script verifies PE architectures for the GUI, native worker, WinUI, and .NET runtime and writes a package SHA-256 manifest. On Windows, run `scripts/build-control-center.ps1 -Architecture arm64` from a developer prompt with Visual Studio C++ tools, the Windows SDK, and the .NET 10 SDK.

`--installer` also writes `build/Waybridge-Setup-win-<arch>.exe`. To reuse an existing validated bundle, run `python3 scripts/build-client-installer.py <arch>`. Set `MAKENSIS` or pass `--makensis` to select the compiler. The current-user installer registers the `waybridge://` protocol and a Start menu entry. Upgrades use the same path under `%LOCALAPPDATA%\Programs\Waybridge`. Uninstalling preserves `%LOCALAPPDATA%\Waybridge`, Windows credentials, and app shortcuts you created.

`/S` installs or uninstalls silently. For unattended deployments, install the matching Visual C++ runtime separately if needed. Release builds from this script are unsigned until a publisher signs them.

The GUI constructs real WinUI controls programmatically in C# without custom XAML resources. The entry point initializes `XamlControlsXamlMetaDataProvider` manually and supplies control types via `IXamlMetadataProvider`. WinUI's own themes and localized strings still need the default `resources.pri`: the publish target copies the precompiled index from the matching Microsoft framework MSIX for the exact version and architecture and verifies byte-for-byte parity of the WinUI, AppRuntime, and MRT DLLs. This avoids running XAML/PRI compilers on Linux; if app-level XAML/RESW resources are added later, the build must move to a standard PRI merge pipeline. Cross-compilation still lets the Windows App SDK generate the full WinRT registration manifest and merges the app manifest via repository scripts; native Windows builds use `mt.exe`.

Waybridge's persistent controls use `L(...)` with an English source string so their labels can update when the language changes. Text generated on demand uses `Tr(...)`. Add the matching Simplified Chinese entry to `gui/Waybridge.Control.Core/zh-Hans.json`, keeping the same format placeholders. Missing translations fall back to the English source string.

Automated tests:

```bash
dotnet run --project gui/Waybridge.Control.Tests -c Release
python tests/gui_session_smoke.py
python tests/gui_direct_smoke.py  # real TLS service, .NET management, and private worker launch contract
python tests/gui_native_smoke.py  # requires Wine / Xvfb; tests real C++ named-pipe channel
python tests/appearance_smoke.py  # requires xdg-desktop-portal 1.22+ and gdbus
xvfb-run -a bash tests/client_installer_smoke.sh  # x64 Linux/Wine with both installers built
```

Linux tests do not replace physical Windows / WoA verification of WinUI layout, system tray, Credential Manager, audio devices, and file pickers.

## Troubleshooting: No Window or Immediate Exit on Launch

Run **`Diagnose-Startup.cmd`** inside the extracted package folder, or copy `scripts/Diagnose-Startup.ps1` next to an older `Waybridge.Control.exe` and run:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Diagnose-Startup.ps1
```

The script verifies package SHA-256 hashes, records the Windows version, binary/dependency architectures, and process exit code, enables [.NET host tracing](https://github.com/dotnet/runtime/blob/main/docs/design/features/host-tracing.md), collects related Application Error / .NET Runtime / SideBySide event-log entries, and writes `%LOCALAPPDATA%\Waybridge\startup-reports\<timestamp>.zip`. No administrator privileges are required, and it never changes global execution policy, registry keys, or existing processes. If the app stays alive, it waits up to 15 seconds and leaves it running so you can inspect dialogs or the tray.

Recent builds also write startup phases and managed exceptions to `%LOCALAPPDATA%\Waybridge\startup.log` and show a plain Win32 message box on fatal errors. When run under the diagnostic script, that log is included in the report directory. If `startup.log` is missing, inspect `dotnet-host.txt`, `stderr.txt`, and `events.txt` first—the failure likely occurred before managed C# entry. Reports never collect `profiles.json`, SSH keys, Credential Manager entries, or clipboard contents, though they may contain your Windows user profile and install paths.

When reporting startup issues, attach the report ZIP (or share the exit code from `report.txt`, the last phase and exception from `startup.log`, and `events.txt`). For example, `0xE0434352` simply indicates a managed exception and requires the stack trace—avoid repeatedly reinstalling runtimes without checking the log.
