# Gamepads (Experimental)

Updated: 2026-11-18. Windows discovers physical gamepads via SDL3 Gamepad and forwards them over the authenticated connection as session-owned Linux `uinput` virtual controllers. Disabled by default. When enabled, the local device count can be displayed in the background, while devices are only opened and forwarded once a foreground remote window holds the input lease.

## GUI and Configuration

The connection profile's input settings expose the following fields, persisted under `settings` in `profiles.json` without environment variables:

| GUI Setting | Profile Field | Default | Range and Meaning |
| --- | --- | --- | --- |
| Gamepads | `gamepads` | `false` | Enable gamepad forwarding |
| Gamepad rumble | `gamepad_rumble` | `true` | Allow games in the active session to send rumble feedback |
| Stick deadzone (%) | `gamepad_deadzone` | `15` | Integer `0–50`; currently applies to both sticks and analog triggers |

Sticks use a radial deadzone and rescale the remaining range; triggers apply the threshold independently and rescale. The initial `hello` negotiates `gamepad_v1` and `gamepad_enabled`; servers supporting `gamepad_settings_v1` can toggle forwarding live via `gamepad_enabled`. When an older endpoint lacks capability, status reports the mismatch rather than claiming a saved preference is active.

`gamepad_enabled` reflects the negotiated preference, and `gamepad_active` indicates whether the current foreground window holds an input lease. `gamepad_local_count` reports forwardable Windows devices (up to four), and `gamepad_devices` lists created Linux virtual devices. Diagnostics also report `rumble_supported`, `gamepad_error`, and `gamepad_backend`.

## Input, Focus, and Rumble

SDL3 handles device discovery, standard button mapping, and dual-motor rumble on supported controllers. Up to four slots are assigned by SDL instance ID so plugging or unplugging one controller does not reorder other connected slots. When SDL sees no devices, a single-device Bluetooth HID fallback for Xbox Series (`045e:0b13`) is temporarily retained pending SDL verification on that physical controller; this fallback does not support rumble. Active input polling is capped at 125 Hz, and the device list refreshes every 500 ms. Actual cadence is subject to UI timers and OS scheduling.

The Windows build downloads and verifies a pinned [SDL3 release](https://github.com/libsdl-org/SDL/releases/tag/release-3.4.20) and links the gamepad library statically into the worker. Portable packages include the SDL license.

A lease is granted only when a local foreground remote window, server-side focus, and non-read-only policy all match. Each input snapshot is bound to window ID, server generation, slot, device incarnation, and a monotonic sequence number; stale, duplicate, or out-of-range states produce no input. Switching windows or losing focus zeroes buttons, sticks, and rumble, while keeping connected Linux virtual devices and their slots intact for the duration of the connection so games retain their open handles. When focus returns, the client submits the current controller state under a new lease. While disabled by default, the client neither polls gamepads nor clears motors belonging to other applications.

Linux exposes a standard Xbox 360 BTN/ABS/HAT layout with inverted stick Y axes per `evdev` conventions. Game `FF_RUMBLE` uploads, plays, stops, and finite-duration expirations are routed back over the active lease to the originating Windows slot; feedback from an old incarnation cannot affect a reconnected device. Continuous rumble is refreshed every 250 ms with a 750 ms feedback lease, checked and zeroed on the client poll loop (not a hard real-time deadline independent of UI scheduling).

When `gamepad_reconnect_v1` is negotiated, a brief disconnect of a controller with a recognizable device path zeroes inputs immediately and preserves its virtual slot for up to 5 seconds. SDL devices match returning hardware by SDL path and GUID; the Bluetooth HID fallback uses its device path. If the same device returns in time, games keep using the existing `evdev` handle while input and rumble switch to the new device incarnation. Unrecognized unplugs or expired grace windows destroy the virtual device. Read-only mode, disabling forwarding, or disconnecting destroys all corresponding virtual devices. Virtual devices are created and destroyed with the session lifecycle, though kernel input nodes remain visible system-wide.

## Backend and Verification

Linux checks read/write access and version for `/dev/uinput` (requiring v5+ for device setup) and creates virtual devices only when valid connected input arrives under an active focus lease. Missing nodes, insufficient permissions, or `ioctl` failures are reported via `gamepad_error` and stop gamepad forwarding without altering system permissions or disrupting other input paths.

Strict x64/ARM64 cross-compiles and the following targeted checks pass:

```sh
cargo test --locked gamepad::tests
cargo test --locked gamepad::tests::uinput_roundtrip_and_real_rumble -- --ignored
cargo test --locked gamepad::tests::uinput_short_reconnect_preserves_player_handles -- --ignored
python3 tests/windows_gamepad_smoke.py
python3 tests/session_fixture.py python3 tests/gamepad_smoke.py
```

The real Linux tests operate exclusively on temporary virtual devices verified by unique name, `phys`, and `sysname`, checking `evdev` buttons/axes/hats, real `EVIOCSFF` uploads and rumble round-trips, effect-expiration zeroing, hotplug isolation, handle preservation across short two-controller reconnects, and timeout removal. Negative tests cover missing/unprivileged backends, non-`uinput` nodes, invalid values, stale leases, and replays. The Windows test runs the production adapter and SDL virtual gamepads in private Wine/Xvfb, covering four slots, hotplug, D-pad, axes and triggers, focus, deadzone, same-slot reconnect, and rumble routing.

End-to-end compositor-to-`evdev` tests verify default-off behavior, bidirectional focus matching, buttons/axes/HATs, rejection of invalid/stale/duplicate input, zeroing and recovery on focus switches on the same `evdev` node, unplug while unfocused, and read-only/disable/disconnect cleanup—reading only temporary nodes verified by compositor PID, name, `phys`, and `sysname`.

Input forwarding with a physical Xbox Bluetooth controller has been confirmed by users. Player ordering in real SDL/Steam games, Bluetooth reconnects, and physical rumble still require dedicated on-device verification. Windows x64 WinUI settings save, reload, and status reporting pass via private provider tests.
