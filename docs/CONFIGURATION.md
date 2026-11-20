# Configuration and Application Launching

Run all commands from the repository root. For ownership boundaries and design rationale, see [Configuration Policy](CONFIGURATION_POLICY.md); for external desktop services, see [Desktop Integration](INTEGRATION.md).

## Linux Primary Configuration

Waybridge reads `$XDG_CONFIG_HOME/waybridge/config.toml` by default, falling back to `~/.config/waybridge/config.toml` if `XDG_CONFIG_HOME` is unset or not an absolute path. When the default file does not exist, built-in defaults apply; Waybridge never searches the current working directory. Passing `--config /absolute/path.toml` replaces the default path and fails if that file does not exist.

Precedence is: built-in defaults → config file → explicit command-line flags. Unknown fields, wrong types, and out-of-range values are rejected with an error. Validate before starting:

```sh
./target/release/waybridge --check-config
./target/release/waybridge --print-config
./target/release/waybridge --config /absolute/path.toml --fps 45 --print-config
./target/release/waybridge-service --config /absolute/path.toml --check-config
```

`--print-config` outputs JSON containing `values`, per-field `sources`, and `apply` stages; it never dumps identity private keys, pairing tokens, or environment variables. Neither check command creates a graphical session, starts a wallet, or binds network listeners.

Example configuration (omitted fields use built-in defaults):

```toml
[service]
listen = "0.0.0.0:49092"
compositor = "/absolute/path/to/waybridge"
python = "python3"

[session]
listen = "127.0.0.1:49090"
socket = "waybridge-0"
fps = 60
bitrate = 30000 # kbps per video stream
codec = "auto"
gpu_input = "cpu"
hdr = "off"
layout = "us"

[limits]
max_fps = 120
max_bitrate = 200000
allowed_codecs = ["png", "h264", "hevc", "av1"]
```

`codec` accepts `auto`, `png`, `h264-nvenc`, `hevc-nvenc`, or `av1-nvenc`; `gpu_input` accepts `cpu` or `cuda`; `hdr` accepts `off` or `experimental-scrgb`. Under `auto`, SDR selects H.264 and HDR preference selects HEVC. `allowed_codecs` lists codec families, and the resolved result of `auto` must also satisfy `allowed_codecs`. Runtime safety fallbacks and hardware capabilities still govern the actual active path.

Frame rate supports `1–120` FPS and bitrate supports `1000–200000` kbps; user limits can tighten these bounds. Session defaults must satisfy the configured limits—for example, lowering `max_fps` to `30` also requires setting `session.fps = 30`.

Changes to `[service]` take effect after restarting the TLS service; `[session]` and `[limits]` apply to newly created sessions without reconfiguring existing processes. Clients may request live updates for supported settings such as FPS, bitrate, codec, and HDR; Linux validates the request and replies with the effective values or a rejection/fallback reason. Windows `profiles.json` stores client preferences while Linux config stores server defaults and limits; client requests never modify Linux TOML files.

`session.listen` and `session.socket` are defaults for launching the compositor directly; the TLS/SSH session manager assigns addresses and sockets dynamically and reports their source as `session_manager`. Reattaching to an existing session via `auto`/`attach` does not load new media config. Service subcommands `pair`, `status`, `clients`, and `revoke` talk directly to the running entry point; `--config` applies only to `serve` and config checks.

`allowed_codecs` restricts requested codecs. If hardware fails at runtime or video is disabled for a session, Waybridge can still fall back to PNG; the effective settings reply explains why via `fallback_reason`, so allowing a codec family is not a hardware guarantee.

## Waybridge-Specific Desktop Overrides

Create `<full-desktop-id>.toml` under `$XDG_CONFIG_HOME/waybridge/desktop-overrides/`, for example `google-chrome.desktop.toml`. Use the exact `id` field returned by the application catalog API—not the executable name or window title. Applications installed in subdirectories still use their standard flattened desktop ID.

The example below adds a user-chosen KWallet 6 flag only when launching Chrome through Waybridge's application catalog, preserving the original `Exec` and profile (whether this backend matches your existing profile depends on what your data uses):

```toml
version = 1

[default]
launch_mode = "exec"
extra_argv = ["--password-store=kwallet6"]
```

`extra_argv` is an array of literal arguments inserted immediately after the original executable token and before original arguments (so before any original `--` or file field codes). It does not pass through a shell, and literal `%` characters do not become field codes. If shell evaluation is needed, invoke a shell explicitly via `exec`.

Supported fields:

| Field | Behavior |
|---|---|
| `exec` | Replaces the Desktop Entry `Exec` string; still parsed by GIO for quoting, escaping, and field codes |
| `extra_argv` | Inserts literal arguments after the executable and before original arguments |
| `cwd` | Sets an absolute working directory |
| `env` | String table of environment variables to set |
| `unset_env` | Array of environment variable names to unset |
| `launch_mode` | `desktop` keeps standard activation; `exec` explicitly forces command execution |
| `name`, `icon`, `terminal` | Overrides display metadata or terminal flag |
| `clear` | Removes original `exec`, `cwd`, or `icon` fields |

The default launch action uses `[default]` and named desktop actions use `[actions."ActionID"]`; sections do not inherit from each other:

```toml
version = 1

[default]
cwd = "/home/alice/Projects"
extra_argv = ["--example-option"]
unset_env = ["EXAMPLE_OLD_SETTING"]

[default.env]
EXAMPLE_MODE = "work"

[actions."NewWindow"]
extra_argv = ["--example-option"]
```

When an application declares `DBusActivatable=true`, D-Bus activation may hand the request to an existing instance and cannot guarantee new arguments, working directory, or environment variables. Overrides affecting process execution therefore require setting `launch_mode = "exec"` explicitly, or launch fails with an error; even with `exec`, an application's own single-instance mechanism may still forward the request.

Original `.desktop` files and standard user application directories are never modified. Derived entries live only inside the session runtime directory and are cleaned up with the session; `%k` still points to the original file. Launches outside Waybridge and direct `argv` launches do not apply these overrides.

## Working Directory and Diagnostics

Desktop launches use the effective `Path`/`cwd`, defaulting to `HOME` when unset so launchers such as HMCL do not create relative data directories inside the service or repository directory. Direct commands also default to `HOME`, with `--cwd` available to override:

```sh
./target/release/waybridge --launch --cwd /home/alice/Projects -- blender
```

Subsequent commands typed inside a terminal inherit the shell's current working directory as usual.

When `--launch` omits `--socket`, it reads config to determine the session name; passing `--launch --socket NAME` attaches directly to that session without parsing config, allowing access to a live session even if the config file currently has a syntax error.

The management helper accepts one JSON request per line, shared across both TLS and local management paths. Querying a launch plan never executes the application:

```json
{"id":1,"op":"launch-plan","session":"waybridge-0","app":"google-chrome.desktop"}
{"id":2,"op":"launch","session":"waybridge-0","app":"google-chrome.desktop"}
{"id":3,"op":"launch","session":"waybridge-0","app":"example.desktop","action":"NewWindow"}
{"id":4,"op":"launch","session":"waybridge-0","app":"example.desktop","uris":["file:///home/alice/example.txt"]}
```

Locally, requests can be piped to `python3 scripts/session-control.py`; TLS requests include paired client credentials (see [Service Interface](SERVICE.md)). Because GIO's action contract does not accept files or URIs, a single request cannot specify both `action` and non-empty `uris`.

Launch plans report field sources, working directory, action, and activation mode; environment customization reports only `env_keys` and `unset_env` without exposing values or session tokens. Desktop launch results `dispatched` or `activation-requested` indicate request submission, and direct command `spawned` indicates process creation—none guarantee that the application has already mapped a window.

## Media Resource Budgets

Use `[media_budget]` to cap CPU scratch buffers, estimated GPU memory, and codec instance counts per Linux session and per window. See [Media Budget](MEDIA_BUDGET.md) for defaults, accounting scope, pressure eviction, and diagnostics. These settings apply to new sessions and are included in `--check-config` and `--print-config`.
