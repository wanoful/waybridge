# Configuration Ownership and Desktop Integration Policy

Decision date: 2026-11-14. Status: initial primary configuration, desktop overrides, and standard session integration are implemented. This document complements [Session Policy](SESSION_POLICY.md) by defining responsibility boundaries; see [Configuration and Application Launching](CONFIGURATION.md) and [Desktop Components](INTEGRATION.md) for exact syntax and operations.

## Responsibility Boundaries

Waybridge owns compositing, remote windows, input, media, a reliable graphical session entry point, and generic application launching. Publishing the session environment, readiness, and shutdown ordering belong to the project; choosing external components such as wallets, notification providers, and authentication agents belongs to the user or optional integration packages.

- Use the standard user D-Bus bus and the user's existing data; never guess a wallet or browser password backend based on installed packages.
- Integrate services through standard systemd user units, targets, and declared dependencies rather than building a core service-management framework keyed on names like KWallet or GNOME Keyring.
- Build on standard contracts (`.desktop`, D-Bus, Portals). Capabilities requiring compositor cooperation—such as a Waybridge ScreenCast backend—remain project responsibilities.
- Do not build a core compatibility engine that alters behavior automatically by application name (Chrome, Steam, HMCL, etc.); explicit user launch customization uses the generic override mechanism below.
- The project may maintain recommended component combinations, unit/drop-in examples, and app setup guides without silently taking over user desktop configs.
- One-way creation admission checks and shared user data remain unchanged; logins via other display managers after startup are not intercepted.

Session shutdown stops graphical services explicitly bound to the session before tearing down the display and revoking the environment. The user bus, persistent data, and user-managed background tasks follow their own lifecycles; Waybridge never scans or force-kills all same-UID processes. A service existing on the bus does not prove it is connected to the current display—that state is surfaced via diagnostics rather than silently patched by process name.

## Configuration Ownership and Locations

Implementations respect valid XDG environment variables and never blanket-replace an application's `HOME` or XDG data paths.

| Content | Recommended Location | Owner |
|---|---|---|
| Linux service, session defaults, resource limits | `$XDG_CONFIG_HOME/waybridge/config.toml` | Waybridge user config |
| Waybridge-specific app launch appends/overrides | `$XDG_CONFIG_HOME/waybridge/desktop-overrides/`, keyed by full desktop ID | Waybridge user config |
| Windows connections, quality, and device preferences | `%LOCALAPPDATA%\Waybridge\profiles.json` | Windows Control Center |
| Waybridge service and session lifecycle | Standard systemd user units, targets, and user drop-ins | systemd (package defaults, user overrides) |
| Default Portal backend selection | `/usr/share/xdg-desktop-portal/waybridge-portals.conf` | Optional integration package |
| User Portal overrides | `$XDG_CONFIG_HOME/xdg-desktop-portal/waybridge-portals.conf` | User (native Portal precedence) |
| Cross-desktop app configs and desktop overrides | Native app configs, `$XDG_DATA_HOME/applications/` | User and respective apps |
| Persistent wallet, browser, and app data | Native application paths | Respective apps |
| Pairing identity, trust store, persistent logs | `$XDG_STATE_HOME/waybridge/` | Waybridge (restricted permissions on key material) |
| Rebuildable caches | `$XDG_CACHE_HOME/waybridge/` | Waybridge (rebuildable) |
| Sockets, locks, session descriptors, derived launch files | `$XDG_RUNTIME_DIR/waybridge/` | Waybridge runtime state |

User and third-party files are never modified by starting or upgrading Waybridge. Credentials, private keys, and wallet contents are excluded from normal config exports; Windows tokens stay in Windows Credential Manager.

## Waybridge Core Configuration

- Uses TOML with a single primary user configuration file—no implicit script execution, working-directory auto-discovery, or complex include trees.
- Precedence is: built-in defaults → user config file → explicit CLI arguments. Passing `--config` replaces the default user file location without stacking hidden layers.
- Windows stores client preferences; Linux stores server defaults and limits. Clients request only dynamically adjustable settings; the server replies with effective values and any constraint or fallback reason without rewriting Linux config files.
- Temporary runtime changes do not persist automatically; saving defaults is always an explicit action.
- Every setting documents whether it applies immediately, on the next session, or on service restart. Invalid fields and values produce explicit errors rather than silently falling back to unrelated behavior.
- The session layer manages display and activation variables only; toolkit preferences and app flags are not injected globally by core defaults. Explicit user environment overrides state their scope.

## Waybridge-Specific Desktop Appends and Overrides

This capability applies exclusively to desktop launches initiated through Waybridge and never alters how the same application launches on local KDE, niri, or other desktops.

It is an explicit, generic user launch mechanism: for example, a user may add a password-backend flag for their Chrome desktop ID, while core Waybridge never inspects for Chrome or guesses flags automatically.

### Resolution and Override Order

1. Resolve the original `.desktop` file using standard application directories and user overrides, matching strictly by full desktop ID—never by executable name or display name.
2. Apply Waybridge-specific appends/overrides for that ID to produce an effective view scoped solely to this launch.
3. Hand off to the generic launcher and standard GIO implementation, preserving quoting, escaping, file/URI expansion, and desktop action semantics.

Original files and user overrides in `$XDG_DATA_HOME/applications/` remain untouched. Any derived files live in the runtime directory, recording the original ID, source, and effective settings without breaking app identity or activation semantics via temporary filenames.

### Behavioral Constraints

- Separate field replacement from argument appending, supporting explicit customization of command, working directory, terminal flag, display metadata, and environment set/unset.
- Extra arguments use a literal argument array with deterministic placement relative to `--` and `%f`/`%F`/`%u`/`%U` field expansions, never shell string concatenation.
- The default action and named desktop actions are configured independently without implicit inheritance across actions.
- Clearing or removing inherited values uses an explicit mechanism rather than overloading empty strings.
- Replacing `Exec` still follows Desktop Entry parsing rules; shell semantics apply only when the user explicitly invokes a shell.
- `DBusActivatable=true` can conflict with process argument, working-directory, or environment overrides. Waybridge neither silently drops overrides nor silently disables D-Bus activation; it validates and requires the user to choose standard activation or explicit `exec` launch.
- When a request is forwarded to an already running instance, overrides cannot be guaranteed to alter that instance's environment, directory, or windows. Diagnostics distinguish spawning a new process from forwarding a request.
- Direct `argv` launches preserve explicit command semantics and never auto-apply desktop overrides by executable name.

Generic working-directory rules: desktop applications use their effective `Path` (defaulting to `HOME`); direct commands use explicit `cwd` (defaulting to `HOME`); child processes launched inside a terminal inherit the shell's current working directory.

The initial format uses `<desktop-id>.toml`, `[default]`, and `[actions."ActionID"]`, inserting literal extra arguments after the executable token and before original arguments. See [Configuration and Application Launching](CONFIGURATION.md) for the exact schema and examples.

## Standard Configs and Recommended Combinations

The repository maintains systemd units, targets, default Portal configs, and integration examples under `integration/`; once installed, standard components read them directly without any unified config-sync daemon.

- Package-provided files are updated by packages; user overrides always belong to the user.
- Recommended configs are installed/enabled via explicit steps, never regenerated or overwritten on every startup.
- Third-party formats retain their native schemas and override semantics (systemd drop-ins, standard desktop overrides, and Portal configs are not forced through one merge engine).
- Wallet/provider choice and data migration stay in explicit user configs and native tools; Waybridge never merges wallet files automatically.
- Waybridge-specific configs live in Waybridge's own namespace to avoid unintended side effects on other desktops.

## Diagnostics, Upgrades, and Verification

Waybridge provides config validation, effective-value/source inspection, desktop launch-plan previews, and standard desktop interface checks—showing real paths, launch modes, and config sources while redacting secrets and never exporting wallet or login data. Diagnostic tools report issues rather than rewriting third-party configs.

Upgrades update only package-owned defaults; if user config migration is ever needed, changes are reported explicitly rather than silently overwritten. Legacy isolated application data remains preserved per [Session Policy](SESSION_POLICY.md).

M1 sign-off covers default and overridden working directories, argument/field expansion and shell boundaries, D-Bus activation conflicts, independent action overrides, environment unsetting, immutability of user files, isolation from non-Waybridge launches, and clean shutdown of graphical services bound to the session target. Real systemd user-session integration is verified independently rather than relying solely on mocked D-Bus/`logind` fixtures.

Core code no longer contains hardcoded wallet activation or dynamic Portal config generation; standard targets, static Portal selection, and user-component examples reside in `integration/`. The shared config crate handles main config checks and source queries, and the desktop launch helper builds on GIO; these implementations do not expand milestone feature claims beyond their verified scope.

References: [XDG Base Directory](https://specifications.freedesktop.org/basedir/latest/), [Desktop Entry](https://specifications.freedesktop.org/desktop-entry/latest/), [Portal Configuration](https://flatpak.github.io/xdg-desktop-portal/docs/portals.conf.html), [systemd.unit](https://www.freedesktop.org/software/systemd/man/latest/systemd.unit.html).
