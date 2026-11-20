# Waybridge Client Launcher and Server TUI Design Specification

Current implementation includes the WinUI main launcher, eight client settings categories, the single TLS connection path, and the Linux management TUI. The main launcher features the global bar, app wall, app menus, app links, Windows shortcuts, and TLS batch app icons. During the prototype phase both ends can be updated together, and users can re-add and re-pair workstations when adopting the new version. Avalonia migration and pasteable pairing URIs are staged for subsequent iterations.

## 1. Goals and Core Decisions

This redesign improves everyday connection, application launching, and server administration ergonomics while eliminating the complex XAML-less WinUI 3 cross-compilation pipeline.

| Decision | Choice and Boundary |
|---|---|
| Client UI stack | Validate **Avalonia UI (.NET 10 + FluentAvalonia)** first; replace WinUI 3 and the Windows App SDK only after portable packaging and OS integration pass verification |
| Client build | Keep Linux cross-publishing of portable `win-x64` and `win-arm64` bundles; remove legacy PRI extraction and manifest-merging scripts after migration sign-off |
| Client core reuse | Reuse `Waybridge.Control.Core`, Windows Credential Manager, and system tray; update regressions for the streamlined connection flow |
| Client transport | Connect to the Linux user service exclusively over authenticated, encrypted Waybridge TLS; retire embedded SSH and raw tunnel modes |
| Client form factor | **App wall + OS shortcuts**: Control Center manages remote apps, while `waybridge://` links and Windows Start Menu / Desktop shortcuts launch directly |
| Catalog & icons | Extend `applications` metadata and add an `app_icons` batch PNG endpoint so the Control Center caches and renders original app icons |
| Pairing UX | Keep one-time pairing codes with full out-of-band fingerprint verification first; add pasteable `wbpair://` URIs from trusted channels later |
| Server admin UI | **`waybridge-service tui`** (Rust `ratatui` + `crossterm`) supporting both online and offline modes |

---

## 2. Implementation Boundaries and Acceptance Baseline

The initial implementation preserves the following separation of responsibilities and verification contracts:

1. **Media vs. control boundary** ([ARCHITECTURE.md](ARCHITECTURE.md)): Video decoding, DirectComposition, remote windows, IME, audio, and OLE remain in the native C++ worker `waybridge-client.exe`. C# communicates only control/status JSON over a current-user private named pipe and never relays media bytes.
2. **Native engine named-pipe protocol** ([client/main.cpp](../client/main.cpp), [EngineChannel.cs](../gui/Waybridge.Control.Core/EngineChannel.cs)) governs connection lifecycle, window activation, file transfers, trays, notifications, settings, and diagnostics. Protocol fields evolve in lockstep with the GUI and worker.
3. **Profiles and credentials** ([Profiles.cs](../gui/Waybridge.Control.Core/Profiles.cs)) store only TLS connection data and client preferences. Persistent client tokens remain in Windows Credential Manager; diagnostics and profile JSON exclude tokens and pairing codes.
4. **Core and service tests** cover the direct TLS flow in `Waybridge.Control.Tests`, `tests/gui_direct_smoke.py`, and `service/tests/direct_service_smoke.py`. Legacy SSH-specific agent code and `tests/gui_ssh_smoke.py` retire alongside the old connection modes.
5. **UI automation** tracks the new layout, covering touch, pen, and gamepads, verifying that offline saved preferences and live applied states remain distinguishable and that saving settings never opens an unintended connection.

---

## 3. Cross-Platform Protocol and Server Extensions

### 3.1 Extended `op: "applications"` Catalog Metadata

Extend `app_catalog()` in [scripts/session-control.py](../scripts/session-control.py) to retain `id`, `name`, `icon` (and internal `path`) while parsing lightweight standard `[Desktop Entry]` fields:

```json
{
  "id": "google-chrome.desktop",
  "name": "Google Chrome",
  "icon": "google-chrome",
  "generic_name": "Web Browser",
  "comment": "Access the Internet",
  "categories": ["Network", "WebBrowser"],
  "keywords": ["Internet", "Web", "Browser"],
  "actions": [
    { "id": "new-window", "name": "New Window" },
    { "id": "new-private-window", "name": "New Incognito Window" }
  ]
}
```

Constraints:
* Bound every string field (e.g., `name`/`generic_name` ≤ 256, `comment` ≤ 512, `categories`/`keywords`/`actions` ≤ 32 entries) while keeping the existing 65,536-byte per-`.desktop` cap and 1,024-application catalog cap.
* Expose only public display and launch metadata; keep `Exec`, environment variables, and filesystem paths on the server.
* `actions` directly reuses `session-control.py`'s existing `{"op": "launch", "app": "<id>", "action": "<action_id>"}` contract.

### 3.2 Batch `op: "app_icons"` Endpoint

Runtime window icons are resolved by the Linux compositor via [src/app_icon.rs](../src/app_icon.rs), whereas unlaunched catalog entries lacked bitmaps. The launcher fetches them over the authenticated TLS management interface independently of active streaming sessions.

* **TLS management request**:
  ```json
  {
    "id": 10,
    "op": "app_icons",
    "apps": ["google-chrome.desktop", "blender.desktop"]
  }
  ```
* **Response**:
  ```json
  {
    "icons": {
      "google-chrome.desktop": {
        "sha256": "<64-char hex>",
        "png": "<base64 of 128x128 RGBA PNG>"
      },
      "blender.desktop": null
    },
    "omitted": []
  }
  ```
* **Size and security budgets**:
  * Reuse [src/app_icon.rs](../src/app_icon.rs)'s `Resolver`, regular-file validation, `rlimit`-isolated `rsvg-convert` subprocess, and `128×128` (`EDGE = 128`, `MAX_PNG = 96 * 1024`) normalization (via a shared crate or `waybridge --resolve-app-icons` CLI helper rather than duplicating XDG icon-theme scanning in Python).
  * Accept at most 24 apps per request and paginate by final JSON line size (including Base64 and framing) so responses stay within the 4 MiB TLS management line limit; return remaining IDs in `omitted` for subsequent batches.
  * Resolve only icon names or absolute icon paths belonging to trusted catalog entries—never allow arbitrary path traversal.

### 3.3 Streamlined Pairing UX and Security Boundary

Preserve the core security invariant from [SERVICE.md](SERVICE.md): **never auto-trust a certificate fingerprint received solely over an untrusted network response**; human or out-of-band verification remains mandatory.

1. **Initial flow keeps the existing mechanism.** `waybridge-service pair` generates a short-lived one-time pairing code and prints the full certificate fingerprint and listener address. Failed requests carry no authenticated pairing-code identity; failure throttling is designed separately.
2. **Step-by-step host, fingerprint, and code entry.** Users copy the full 64-hex-character SHA-256 fingerprint from a trusted terminal. If IP probing is offered, display the observed leaf certificate fingerprint for comparison, grouped for readability while verifying the complete hash on confirmation. The client pins that certificate for TLS before sending the pairing code and stores `client_token` in Windows Credential Manager.
3. **Follow-up: pasteable pairing URI.** The TUI can render `wbpair://v1?host=<host>&port=<port>&fp=<certificate_sha256>&code=<code>` using a reachable host address. The wizard parses the URI from a trusted channel into address, full fingerprint, and code, displays the target for explicit confirmation, and runs the exact same pinned-certificate TLS verification. The URI carries a one-time secret valid only for that pairing attempt.

---

## 4. Server Administration UI: `waybridge-service tui`

### 4.1 Runtime Architecture and Online/Offline Modes

Add a `waybridge-service tui` subcommand in [service/](../service) (built with Rust `ratatui` + `crossterm`) for local Linux TTYs and system SSH terminals. On SSH-only networks, system SSH can forward the TLS entry port while Windows still pairs and connects over TLS (UDP media remains independently reachable).

* **Online mode (`waybridge-service serve` is running)**:
  * To avoid conflicting writes to `trust.json` and `service.lock` ([service/src/main.rs#L151](../service/src/main.rs#L151)), the TUI communicates with the daemon over the same-UID `0o600` socket `$XDG_RUNTIME_DIR/waybridge-service/control.sock`.
  * Extend `control.sock` with **local-only** operations (never exposed over remote TLS): `sessions` (list active sessions), `end_session` (safely terminate a session), and `preflight` (environment checks), reusing `helper()` for `applications` and `launch_plan`.
* **Offline mode (`waybridge-service serve` is stopped)**:
  * The TUI still launches cleanly, displays "service not running" with quick actions (attempt `systemctl --user start waybridge.service` or show the foreground command), and fully supports preflight checks, config editing, and desktop override editing.

### 4.2 Interface Layout (4 Keyboard Tabs)

Full keyboard navigation via `1–4`, `Tab`/`Shift+Tab`, arrow keys, `Enter`, and `Esc` with no mouse required:

1. **`[1] Overview & Pairing`**:
   * Displays `waybridge.service` / daemon status, TLS listen address, grouped certificate fingerprint, and loaded config path.
   * **Environment preflight panel**:
     * Runs the equivalent of `waybridge --check-session` to flag any active same-UID X11/Wayland desktop that must log out before session creation;
     * Checks NVENC/CUDA availability, Xwayland, `pactl`/`parec`, `rsvg-convert`, `fusermount3`, Portal configuration, and `waybridge-notifications.service` bus conflicts.
   * **Pairing area**: press a shortcut (e.g., `p`) to mint a one-time pairing code showing the copyable full certificate fingerprint and countdown timer (pasteable URI follows with the wizard update).
2. **`[2] Sessions & Logs`**:
   * Lists running Waybridge sessions (name, PID, listen port, uptime).
   * Lower/side pane tails `$XDG_RUNTIME_DIR/waybridge-control-center/<session>.log` live (with pause, keyword filter, and paging).
   * Safe session termination with confirmation, sending `SIGTERM` after `pidfd` identity verification.
3. **`[3] Trusted Devices`**:
   * Lists paired devices from `trust.json` (`label`, `client_id`, paired timestamp).
   * Supports selecting a device and revoking trust after confirmation (immediately terminating active TLS tunnels for that device).
4. **`[4] Settings`**:
   * **Service & listeners, session defaults, and performance/resources** cover `[service]`, `[session]`, `[limits]`, and `[media_budget]` using the service's actual loaded config path (XDG default or explicit `--config`). Online mode distinguishes running, saved, and draft values along with sources (default, file, CLI); offline mode shows effective values for the next start, reusing validation and apply-stage reporting from [config/src/lib.rs](../config/src/lib.rs).
   * **Validate & save** validates the full draft, previews diffs and apply stages, and writes atomically—prompting to reload if the file changed on disk during editing. Service restarts are offered as an explicit separate action explaining connection impact; session defaults take effect on the next new session.
   * **Application launches** edit Waybridge-specific overrides by full Desktop ID for both `[default]` and named Desktop Actions, previewing the effective command and `field_sources` against the unsaved draft (working offline against installed entries when no session is running), highlighting `DBusActivatable` conflicts, and showing expected next-launch behavior.
   * **Desktop components** manage Waybridge-owned Portal and notification integration settings, displaying effective config sources, user units, and readable runtime status while writing standard configs and Waybridge user-unit drop-ins per [Desktop Integration](INTEGRATION.md).

---

## 5. Client Redesign: UI Stack and Dual-Mode Launcher

### 5.1 Project Structure and WinUI 3 Removal Checklist

First build a minimal portable Avalonia bundle cross-published from Linux for x64 and ARM64 and verify extraction, startup, tray, UI Automation, native notifications, and notification action callbacks on physical Windows. Only after portable notification identity and activation registration are confirmed should WinUI and Windows App SDK packaging be removed.

1. **Remove legacy dependencies and build workarounds**:
   * Remove `UseWinUI`, `WindowsPackageType`, `WindowsAppSDKSelfContained`, `Microsoft.WindowsAppSDK`, `AppxGeneratePriEnabled`, `EnablePriGenTooling`, `DISABLE_XAML_GENERATED_MAIN`, custom MSBuild task `WaybridgeExtractResourceIndex`, and `TrackCrossHostManifestTool`.
   * Delete [scripts/extract-runtime-resources.cs](../scripts/extract-runtime-resources.cs) and [scripts/merge-windows-manifest.py](../scripts/merge-windows-manifest.py) (and their build checks); simplify [scripts/build-control-center.sh](../scripts/build-control-center.sh) to standard `dotnet publish -r win-$arch --self-contained true` plus copying `waybridge-client.exe`, docs, and licenses.
   * Keep the single-instance `Mutex` (`Local\Waybridge.ControlCenter`) and startup diagnostic logging in [Program.cs](../gui/Waybridge.Control/Program.cs), switching entry to standard Avalonia `AppBuilder.Configure<App>().UsePlatformDetect()`.
2. **Suggested module layout**:
   * `Waybridge.Control.Core/` (cross-platform testable):
     * `LauncherModels.cs` & `LauncherStore.cs` (persisting `launcher.json` and local icon disk cache);
     * `PairingHelper.cs` (full fingerprint entry/verification, later `wbpair://v1` parsing);
     * `SessionController.cs` extensions (`AppIconsAsync`, `LaunchPlanAsync`, `ClickToLaunchAsync`, and launches with `action`/`uris`/`argv`/`cwd`).
   * `Waybridge.Control/` (Avalonia desktop project):
     * `Views/MainWindow.axaml(.cs)`: full app wall and control center;
     * `Views/PairingWizardDialog.axaml(.cs)`: step-by-step TLS pairing;
     * `Views/SettingsView.axaml(.cs)`: categorized settings hub;
     * `Views/ActivityDrawer.axaml(.cs)`: file transfer activity and diagnostics;
     * `ViewModels/`: clean separation of state, commands, and UI dispatcher marshaling;
     * `Services/DesktopNotifications.cs`: verify portable notification identity, activation registration, and session-scoped action routing while keeping existing notification protocol and lifecycle rules;
     * `IconBranding.cs`: normalize custom badges to 64×64; native Windows windows and trays composite badges at target pixel size.

### 5.2 Local Storage and Caching

TLS profiles, launcher state, and icon caches persist separately:

* **`%LOCALAPPDATA%\Waybridge\profiles.json`**: TLS address, certificate fingerprint, client ID, and connection preferences (`ConnectionProfile` / `ClientSettings`).
* **`%LOCALAPPDATA%\Waybridge\launcher.json`**:
  * Global preferences: layout, selected workstation, theme, and close-to-tray.
  * Per-`profile_id` data: `pinned_apps` (ordered `desktop_id` array), `recent_apps` (recent launches with timestamps, cap 32), `command_history` (custom `argv` + `cwd` history, cap 20), and `cached_applications` (last fetched extended catalog snapshot).
* **`%LOCALAPPDATA%\Waybridge\cache\icons\`**:
  * Raw icon cache `raw/<sha256>.png` shared across launcher views.

### 5.3 Main Window (Full Launcher) Layout and Interactions

1. **Top global bar (Omnibar)**:
   * Left: **Workstation switcher pill** showing status indicator (green = connected, amber = connecting, gray = disconnected), workstation name, and session name; dropdown switches workstations, connects/disconnects, opens "Add/Manage Workstations", or triggers confirmed "End remote session".
   * Center: **Global search box** (`Ctrl+K` / `/` to focus):
     * Normal input filters installed apps (`name`, `generic_name`, `id`, `keywords`, `comment`) and active windows live;
     * Input starting with `>` switches to **Command Runner mode** (history autocomplete, optional working directory, `Enter` launches via `argv`).
   * Right: **Quick controls** — Read-only toggle, Audio/Volume flyout, Status/Latency (RTT + active codec path) flyout, File Transfers drawer (with activity badge), and Settings entry.
2. **Launcher and Running views**:
   * **Running** (accessed from the fixed left navigation rail) consolidates remote windows and SNI background apps:
     * Active window cards show icon + title + codec/decode badge (e.g., `HEVC · GPU`); clicking calls `ActivateWindowAsync` immediately;
     * SNI background app cards offer "Restore window" and "App menu".
   * **Pinned & Recent**: prominent top grid of pinned and recently launched apps.
   * **All Apps** shares the same scroll container; as you scroll down, the catalog title, refresh, layout toggle, and category filter stick to the top of the viewport. Supports both grid and compact list views.
   * **App names** wrap to at most two lines with trailing ellipsis in both grid and compact list, showing the full name on hover.
   * **Click and context actions**:
     * **Single click launches immediately**: calls `LaunchAsync` when connected; when disconnected with a valid paired workstation selected, triggers **Click-to-Launch** (lightweight inline connection progress without jarring navigation) and dispatches the launch once ready.
     * **Right-click / hover `…` menu**:
       * Launch;
       * Named Desktop Actions (if any);
       * Pin / Unpin;
       * Touch mode (Auto / Prefer native touch / Mouse compatibility), saved per profile and full Desktop ID, mapping confirmed catalog identity to the native worker's window `app_id` (unknown identities can be configured from running windows and default to auto detection);
       * Custom launch (override/append working directory or arguments with `launch-plan` preview).
   * **First-run empty state**: when no paired workstation exists yet, displays an "Add your first Linux workstation" onboarding card opening the pairing wizard.
3. **Secondary views**:
   * **Workstations & Pairing Wizard**: enters TLS address, verifies full fingerprint, and submits one-time pairing code (later adding pasteable pairing URIs from trusted channels). Paired workstations show a clean summary and "Revoke this device", with full fingerprint and client ID expandable on demand.
   * **Settings Hub**: organized into **Display**, **Audio**, **Input Devices**, **Sharing & Permissions**, **Network**, **Connection**, **Desktop Integration**, and **General**. Clearly labels per-workstation vs. global scope and live vs. reconnect vs. new-session apply stages. Top-level toggles and live local/remote/focus/forwarding statuses are visible directly for touch, pen, and gamepads, with pressure curve, deadzone, and per-app compatibility under Advanced. Offline edits save cleanly; online applies display effective results after confirmation, preserving UI automation probe contracts.
   * **File Transfers & Diagnostics**: file/folder send actions, receive directory shortcut, transfer history, RTT / TCP receive rate / presentation update / decode & present stage telemetry, and log export (excluding tokens, clipboard, and notification bodies).

### 5.4 App Links and OS Shortcuts

* The app card menu offers Copy link, Add to Windows Start Menu (under the `Waybridge` folder), and Create desktop shortcut.
* Link format: `waybridge://launch/<profile-uuid>/<base64url-utf8-desktop-id>`. The Control Center looks up the local paired profile and launches by catalog identity.
* An already running Control Center receives external invocations directly; cold starts initialize in the background. Launches reuse Click-to-Launch, confirm before switching away from another active workstation (keeping its remote session alive), and surface errors in the Control Center.

---

## 6. Recommended Implementation Order and Acceptance Checkpoints

1. **Client main window, settings, and portable bundle verification.** Converge on the single TLS path and retire embedded SSH and raw tunnel modes. Deliver session reattach, search-to-launch, window switching, and complete settings using existing pairing and catalog endpoints. Validate the minimal portable Avalonia bundle in parallel before migrating Windows OS integrations and build scripts. Run direct TLS and Windows UI automation regressions.
2. **Server TUI and config editing in parallel.** Verify online/offline transitions over local TTY and SSH, pairing, controlled session termination, log tailing, config sources and apply stages, draft validation, and offline/online launch-plan previews.
3. **Full launcher and catalog.** Deliver the global bar, grid/compact views, pinned/recent, running windows and background apps, and app context actions. Extend catalog metadata and batch icon fetching over TLS, verifying response size budgets and missing-icon fallbacks. Verify pasteable pairing URIs separately for full fingerprint matching, one-time use, and expiration.
4. **App links and Windows shortcuts.** Create shortcuts keyed by paired workstation and Desktop ID; verify external invocation from Windows, forwarding to an already-running instance, and auto-connect when disconnected.
