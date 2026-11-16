# Application trays and background applications

The adapter follows the [StatusNotifierWatcher specification](https://specifications.freedesktop.org/status-notifier-item/latest/status-notifier-watcher.html) and the common KDE SNI/DBusMenu interfaces.

Waybridge presents Linux StatusNotifierItem applications as independent native
Windows notification-area icons. The control center's “远端应用 → 后台应用” list
keeps the same entries accessible when an application has closed its main
window, or Windows has put its tray icon in the overflow area. “打开／恢复” invokes
the application's activation method; “应用菜单” contains its own normal exit
operation where provided. No application name/PID search or forced termination
is involved.

`ClientSettings.Tray` defaults to true and is persisted as `tray` in a connection's
settings. The “远端应用托盘” switch controls local icon/background-entry visibility
and actions, with live apply support. Read-only mode keeps entries visible while
blocking their input on both ends. Disconnect removes all of this worker's Shell
icons. Each application icon uses the same owned `NativeIcon` and configured
upper-right source badge as streamed windows; fallback icons are branded too.
Explorer's TaskbarCreated event re-adds icons, and settings/display/DPI changes
refresh them. Source generation, viewer epoch and menu token checks invalidate
old clicks, old background buttons and menus.

On Linux, a dedicated worker hosts and polls standard D-Bus interfaces; no D-Bus,
icon lookup, SVG decoding or filesystem work occurs on the compositor loop. It
joins an existing `org.kde.StatusNotifierWatcher` (or the freedesktop variant)
and registers a `org.kde.StatusNotifierHost-PID` name (or freedesktop equivalent). If neither has an owner, it requests the
standard KDE watcher name with DoNotQueue and no ReplaceExisting, and supplies
the standard watcher object. It validates registration ownership, removes dead
registrations and emits the standard item registered/unregistered signals.
Another desktop's watcher is not displaced.

Both KDE/freedesktop SNI item interfaces are accepted, including pixmap and theme
name icons, attention state, activation, secondary activation and scrolling.
Windows mouse-wheel events are forwarded only when the pointer is inside one
of this worker's own Shell icon rectangles; the low-level hook never consumes
the system event. DBusMenu is translated to native HMENU submenus, disabled
items, checkmarks, radio marks and mnemonics. Existing Linux windows provide a
bounded fallback for applications that expose ContextMenu without DBusMenu.

The worker has 64 action slots, 500 ms per-method bus timeouts, bounded menu
size/depth and a rotating poll budget (up to 8 items per pass, stopping the pass between items
after its 750 ms time budget). It checks fresh lifecycle state between passes.
Normal source changes appear within a polling interval; unresponsive sources
can delay their own refresh. Metadata and icons use separate bounded queues;
unchanged icons are not retransmitted for every title/status change. Missing
Watcher interfaces, conflicts, icon pressure and missing fallback windows
produce explicit availability/errors/warnings in the control center.

Compatibility boundaries:

- XEmbed tray embedding and conversion are not implemented. An application with
  only an old X11 tray icon does not gain an SNI entry automatically.
- A ContextMenu-only item without a matching mapped application window is
  reported as unsupported instead of opening an inaccessible server-side menu.
- The adapter does not infer which application processes belong to a tray item,
  choose another password service, or add application-specific restore/quit
  commands. Applications must implement the standard operation they advertise.
- SNI attention icons are supported; overlay pixmaps, menu item artwork,
  shortcuts, and lazily populated submenus beyond the exported DBusMenu layout
  are not yet given a separate Windows UI path.
- Native Windows Explorer acceptance is tested separately from Wine, whose
  Shell_NotifyIconGetRect can fail despite executing the icon API path. Native
  rendering/accessibility at every DPI and actual Explorer crash/restart still
  need broader device acceptance.

## Validation — 2026-11-16

`python3 tests/tray_smoke.py` uses a disposable HOME, private dbus-daemon, the real
compositor, and a Gio SNI/DBusMenu fixture. It covers joining an existing watcher
without replacing it, providing a watcher when absent, KDE/freedesktop item
interfaces, ARGB pixel conversion, nested/check/radio/disabled/hidden menus,
ancestor policy enforcement, activation/secondary/scroll, layout revisions,
source owner replacement, reconnect epochs, capability absence, server read-only
policy (including policy changes while GetLayout is blocked), and ContextMenu-only paths with/without a matching remote window.

The same integration launches actual VLC on private Xwayland: a background SNI
entry activates a streamed application window; the application's exported Quit
menu action then exits VLC normally. It uses isolated VLC configuration and no
application-specific production code.

`python3 tests/windows_tray_smoke.py` builds a probe against production
`client/remote_tray.h`, creates actual Shell icons and HMENUs inside a disposable
Wine/Xvfb desktop, inspects real native menu state/submenu handles, selects
Restore through the native mnemonic path, and verifies emitted menu identities,
read-only/visibility settings and cleanup. It executes the Explorer re-add/DPI
paths but does not claim Wine is a Windows Explorer acceptance test.

`python3 tests/windows_tray_laptop.py --host laptop` is an opt-in native Windows
probe using a unique temporary executable/report and a Limited interactive
scheduled task. It opens only its own window/menu/tray entry, exits automatically,
and removes its task/files. It never replaces a deployed Waybridge binary or
closes another application's windows. The native x64 `laptop` run passed: Explorer accepted the independent tray icon
(`Shell_NotifyIconGetRect` succeeded), real HMENU hierarchy/check/radio/disabled
states matched, a keyboard mnemonic dispatched the expected application menu
identity, and read-only/hide/cleanup checks passed. The task returned exit code
0; its temporary executable/report and task were removed. A duplicate
TaskbarCreated broadcast repairs existing IDs using NIM_MODIFY if NIM_ADD finds
the icon already present. This exercises recovery logic without restarting the
user's actual Explorer. ARM64 was cross-compiled; this x64 device does not execute
ARM64 binaries.
