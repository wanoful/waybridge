# Session and User Data Policy

Decision date: 2026-11-14. This document supersedes the initial M1 design that defaulted to multi-desktop parallel sessions, private wallets, and isolated browser profiles.

For configuration ownership, standard desktop integration boundaries, and Waybridge-specific `.desktop` appends/overrides, see [CONFIGURATION_POLICY.md](CONFIGURATION_POLICY.md). For operational instructions, see [Configuration and Application Launching](CONFIGURATION.md) and [Desktop Components](INTEGRATION.md).

## Confirmed Behavior

- By default, Waybridge shares the Linux user's `HOME`, application configurations, caches, and data—including Chrome profiles and KWallet. Waybridge does not automatically copy, migrate, or overwrite legacy isolated data, nor does it force a browser password backend; explicitly passed application arguments remain effective.
- Only one Waybridge graphical session is supported per user. Before creating a session, Waybridge queries `logind`: if the same UID already has an active or background Wayland/X11 user session, creation is rejected with a prompt to log out first. Locking the screen does not count as logging out; login greeters, SSH, TTYs, user service managers, and other users do not block session creation.
- Reconnecting to an existing Waybridge session takes precedence; viewer disconnects and connection-service restarts do not terminate the session. Requesting a second new session prompts the user to end the existing session first. Direct compositor startup runs the same check and uses a runtime lock to prevent concurrent Waybridge session creation.
- Logging in via another display manager (DM) after Waybridge has started is left to that DM. Waybridge does not add PAM login hooks, does not implement bidirectional mutual exclusion, and does not continuously monitor or terminate desktops started later.
- Waybridge manages its own graphics, input, audio nodes, and temporary resources. Ordinary user services and application data locks follow standard system and application mechanisms; Waybridge does not scan for or force-kill other background processes belonging to the same user.

## Service Boundaries

Waybridge uses the standard user D-Bus session bus and the user's existing keyring/secret service, updating the service activation display environment for the active session. The native wallet service handles unlocking and authorization; Waybridge does not forward wallet calls across desktops or launch a private storage backend backed by the same wallet files.

Component selection is controlled by the user or optional integration configs. Waybridge provides a reliable session entry point and standard lifecycle integration without building a core compatibility layer that patches behavior by wallet or application name. Users can explicitly customize launch behavior through standard configs or Waybridge-specific desktop overrides while leaving original application files untouched.

Portals select backends suitable for Waybridge via static native configuration while preserving user configs and persistent data; unimplemented capabilities such as ScreenCast are still not advertised as supported. Existing long-lived services follow their own lifecycles and are not killed by process name. On session shutdown, Waybridge first stops services explicitly bound to the session, then revokes activation environment variables still owned by the session; it does not restore stale displays or overwrite the environment of a desktop started later. The limitation that `dbus-daemon` lacks an unset-variable API is explicitly documented in diagnostics and integration docs.

## Compatibility and Verification

If the `logind` query fails, Waybridge reports that the check could not be performed rather than assuming no desktop is running. No privileged login daemon is required; Waybridge manages its own sessions via existing descriptor files and runtime locks, without adding a PAM/`logind` session registration entry point.

Default data sharing does not guarantee that an application supports opening the same data directory concurrently. Users accept alternating between local desktop and Waybridge sessions for applications like Chrome; if an application reports a data lock conflict, Waybridge surfaces its error normally instead of bypassing locks. Chromium selects its password backend based on desktop identity, and Waybridge does not spoof KDE or GNOME. If existing data uses KWallet 6, explicitly keep `--password-store=kwallet6` in the launch command or browser flags; sharing a directory does not convert password storage backends.

Legacy `~/.local/share/waybridge/sessions/*/browser-profiles` and `wallet` directories are left untouched so users can explicitly reference old Chrome directories if desired. Any wallet migration must be handled separately with native tools; automatic merging of wallet files is forbidden.

Automated tests use temporary user buses, temporary XDG directories, and simulated `logind` state so they never access real wallets or modify the active desktop's user service environment. Historical M0/M1 verification proves only the earlier isolated implementation and does not substitute for validating this policy.
