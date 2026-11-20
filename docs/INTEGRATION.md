# Desktop Components and Session Lifecycle

Waybridge provides the display, application launch environment, and session lifecycle. Components such as wallets, notifications, and authentication agents integrate through their own D-Bus, systemd, and configuration mechanisms. Core Waybridge does not guess wallet backends from installed packages, does not start Plasma, and does not modify browser or third-party configs.

Applications keep using the user's `HOME`, configs, data, and user D-Bus bus. Native components can be D-Bus-activated on demand by applications; components that should start and stop with Waybridge declare the unit dependencies below. For user data sharing and one-way creation checks, see [Session Policy](SESSION_POLICY.md).

## Installing Standard Integration Files

The repository's [integration/](../integration/) directory holds systemd targets and a static default Portal config. Build first, then install explicitly:

```sh
cargo build --release --workspace --locked
python3 service/install-user-service.py
systemctl --user daemon-reload
systemctl --user enable --now waybridge.service
```

The installer copies the user binaries, the TLS entry unit, and the three Waybridge session targets without starting services automatically. Modified existing units are not overwritten and require manual merging. `waybridge.service` is the TLS entry point; restarting it does not terminate existing compositor sessions. Session targets are started by the compositor once the display and environment are ready—do not manually `enable` or `start` them.

The default install does not write a Portal configuration. To install the repository's GTK file-chooser Portal preset, pass an explicit target directory:

```sh
python3 service/install-user-service.py \
  --portal-directory "${XDG_CONFIG_HOME:-$HOME/.config}/xdg-desktop-portal"
```

This installs `waybridge-portals.conf` and likewise refuses to overwrite a differing existing file. Distribution packages may install the default file under `/usr/share/xdg-desktop-portal/`, where user files still take precedence under native Portal lookup rules. Running the compositor never generates temporary Portal configs or modifies `XDG_CONFIG_DIRS`.

This preset configures `xdg-desktop-portal` with the GTK backend and explicitly sets unimplemented `ScreenCast`, `Screenshot`, `RemoteDesktop`, and `GlobalShortcuts` backends to `none`. It does not imply support for those features or full Flatpak document-portal verification.

## Connecting a Session Component

Write a standard systemd user unit for a chosen component, for example `~/.config/systemd/user/example-waybridge-agent.service`:

```ini
[Unit]
Description=My Waybridge session component
After=waybridge-session-pre.target
Before=waybridge-session.target
PartOf=waybridge-session.target

[Service]
Type=simple
ExecStart=/absolute/path/to/component

[Install]
WantedBy=waybridge-session.target
```

Then enable it:

```sh
systemctl --user daemon-reload
systemctl --user enable example-waybridge-agent.service
```

The component starts on the next Waybridge session creation and stops via `PartOf` when the session shuts down. `WantedBy` pulls it in on session startup, while `PartOf` declares shutdown ownership—each serves a distinct role. Component flags, environment, and permissions belong in the unit and the component's native config. Existing distribution units can use standard drop-ins; check installed unit names and startup conventions.

Shared long-lived user services may keep their own lifecycles instead of binding to Waybridge. Background processes without declared ownership are never scanned or killed by name or UID. Binding an existing provider to the session is an explicit user decision based on its lifecycle.

If a component already ships a systemd unit and a D-Bus activation file with `SystemdService=`, attach to that unit via drop-ins rather than creating a differently named `Type=dbus` wrapper claiming the same `BusName`; even if not run simultaneously, systemd may reject activation due to duplicate bus-name ownership.

For example, pull in installed Portals via `~/.config/systemd/user/waybridge-session.target.d/portals.conf`:

```ini
[Unit]
Wants=xdg-desktop-portal.service xdg-desktop-portal-gtk.service
```

And add a `waybridge.conf` drop-in to each native unit:

```ini
[Unit]
PartOf=waybridge-session.target
After=waybridge-session-pre.target
Before=waybridge-session.target
```

The portal frontend can additionally declare `After=xdg-desktop-portal-gtk.service` to make backend startup ordering explicit. Run `systemctl --user daemon-reload` afterward. This keeps D-Bus activation files, systemd units, and running services consistent; wallets and other components should likewise avoid duplicate bus-name declarations.

## Readiness and Shutdown Order

1. The compositor completes the one-way admission check, creates its display, and publishes the session's display and identity environment.
2. It asynchronously starts `waybridge-session.target` and its declared dependencies while the main thread continues servicing Wayland so `Type=notify` and D-Bus services requiring display interaction can finish starting.
3. Once the target reaches `active`, lifecycle diagnostics report `ready`. Whether an individual component succeeded is still determined by its own unit status; a failing `Wants=` unit need not prevent the target from activating, nor does activation prove an application has mapped a window.
4. On normal exit or returnable errors during app startup or the event loop, Waybridge starts the shutdown target to stop services bound via `PartOf` while continuing to service display requests.
5. After bound services stop, Waybridge revokes activation environment variables still owned by the session, then destroys the display. Environment variables already overwritten by a later desktop login are preserved, and stale pre-logout display addresses are not restored.

These three targets are independent of `graphical-session.target`, `plasma-workspace.target`, and other DE targets. A standard systemd user manager plus the installed integration files provide declarative lifecycle management; when absent, diagnostics report `unavailable` rather than pretending external services are managed.

The status file is diagnostic only: write failures log a warning without blocking service startup, shutdown, or environment cleanup. On session errors, Waybridge completes teardown before returning the original error (and reports teardown errors alongside it if any occur). Under forced termination (`SIGKILL`) or if the display connection itself has failed, components cannot be guaranteed to finish display interactions.

systemd's activation environment supports truly unsetting previously unset variables while preserving explicitly empty user values. `dbus-daemon`'s `UpdateActivationEnvironment` lacks an unset API, so stale display addresses are neutralized with empty values and diagnostics note this limitation. Cleanup updates D-Bus first and then lets systemd restore or unset variables so D-Bus's systemd activation forwarding does not turn unset variables into empty strings. Without a queryable user manager environment, Waybridge cannot safely determine whether a later desktop login took over the D-Bus daemon environment. Sessions never push toolkit backend overrides (Qt/GDK), per-app audio settings, or data directories into the global user service environment.

## Wallets, Browsers, and Diagnostics

Wallet storage and unlocking remain with the native component. SSH key login does not supply a wallet unlock password, and having a running secret service does not automatically pick a browser's password storage backend. To keep an existing Chrome backend, configure an explicit [Waybridge desktop override](CONFIGURATION.md#waybridge-specific-desktop-overrides); core Waybridge never appends flags automatically.

`WAYBRIDGE_DESKTOP_SERVICES` points to a session status JSON file containing lifecycle status, read-only observations of standard interfaces, and unsupported capabilities. A D-Bus name having an owner proves only bus registration, not that its windows are connected to the current display; Waybridge does not auto-restart shared services based on that.

Inspect status from a Waybridge terminal and query declared components with standard tools:

```sh
cat "$WAYBRIDGE_DESKTOP_SERVICES"
systemctl --user status waybridge-session.target
systemctl --user status example-waybridge-agent.service
journalctl --user -u example-waybridge-agent.service
```

For application catalog launch plans and override diagnostics, see [Configuration and Application Launching](CONFIGURATION.md).

## Explicitly Opting Into the Notification Provider

Remote desktop notifications use a standard `org.freedesktop.Notifications` provider. Waybridge never eavesdrops on another daemon's `Notify` calls, never steals an existing bus name owner, never stops another DE's notification service, and never alters other notification configs. Only one notification daemon can own the name on a user bus; if you want Waybridge forwarding, select it in your session component config and avoid starting a competing notification daemon at the same time.

The optional unit is neither installed nor enabled by default. Install it explicitly alongside built binaries:

```sh
python3 service/install-user-service.py --notification-provider
systemctl --user daemon-reload
systemctl --user enable waybridge-notifications.service
```

`enable` hooks it into `waybridge-session.target` for future Waybridge sessions. Inside an already running Waybridge session—after confirming no competing provider is active—you can also start it directly with `systemctl --user start waybridge-notifications.service`. No D-Bus activation file edits or global alias overrides are needed. The unit runs `waybridge --notification-provider` and stops with the session via `PartOf`. It requires `WAYBRIDGE_SESSION_ID`'s private runtime directory and the user bus, and cannot run as a standalone daemon outside a Waybridge session.

When another owner already holds the bus name, the provider requests the name with `DoNotQueue` and without `ReplaceExisting`, so it fails cleanly instead of queuing to take over. If the system already provides a native systemd unit for the same `BusName`, inspect its unit and activation configuration so your selection is explicit without creating duplicate bus-name assignments. Diagnostics:

```sh
systemctl --user status waybridge-notifications.service
journalctl --user -u waybridge-notifications.service
busctl --user status org.freedesktop.Notifications
```

The Windows Control Center also surfaces states such as "not opted in", "another notification daemon owns the bus", and "notification provider disconnected". **Sharing & Permissions → Desktop notifications** controls display and actions for the current connection (`settings.notifications`, default `true`) without changing Linux component selection. When native Windows notifications are unavailable, OS permissions are denied, the client is disconnected, or sharing is turned off, `Notify` returns an explicit unavailable error rather than queuing an offline backlog. Disabling, disconnecting, or losing the provider clears active notifications and invalidates stale actions. Read-only mode keeps notifications visible and dismissible locally while blocking app actions on click.

Windows renders notifications through native AppNotifications, respecting system Do Not Disturb, banners, and Notification Center rules. Inline replies, custom sounds/animations, rich-text bodies, and persistent notification archives are not implemented. Each notification supports at most 4 action buttons plus a default click action; larger payloads are explicitly rejected. Notification icons use the unified upper-right source badge (or the badged placeholder when no icon is provided). Notification body text is never written to diagnostic logs.
