# Waybridge TLS service

This user-level entry point provides paired, encrypted access to the existing
Waybridge v3 compositor without an SSH tunnel. It uses rustls TLS 1.2/1.3, a
persistent self-signed ECDSA server certificate, and independently authenticated
management and streaming connections. It does not change the v3 wire protocol.

## Run and pair

```sh
cargo build -p waybridge-service
target/debug/waybridge-service serve --compositor /absolute/path/to/waybridge
target/debug/waybridge-service status
target/debug/waybridge-service pair --expires 120
target/debug/waybridge-service clients
target/debug/waybridge-service revoke CLIENT_ID
```

The default TLS listener is `0.0.0.0:49092`. Use `--listen 127.0.0.1:0` for an
isolated test or a specific LAN/VPN address for deployment. `serve` prints its
actual endpoint and certificate fingerprint as JSON, without client secrets.

Both Linux binaries read `$XDG_CONFIG_HOME/waybridge/config.toml` (normally
`~/.config/waybridge/config.toml`). `--config FILE` replaces that lookup; explicit
CLI values override the file. `--check-config` and `--print-config` work without
starting the daemon or creating identity files. See [configuration](CONFIGURATION.md)
for the schema, sources, server-side media limits and application overrides.

State defaults to `$XDG_STATE_HOME/waybridge/service` or
`~/.local/state/waybridge/service`. The owner-only administration socket and
`ready.json` are under `$XDG_RUNTIME_DIR/waybridge-service`. Global flags
`--state-dir` and `--runtime-dir` override those locations; the latter denotes
the service subdirectory, not the compositor's `XDG_RUNTIME_DIR` base.

The local `pair` command returns an expiring, one-use random pairing code and a
`certificate_sha256` pin. The client must verify that pin through a trusted
local/out-of-band source **before** sending the code or any credentials. The pin
is lowercase SHA-256 hex of the exact leaf certificate DER, without separators.
Checking a fingerprint merely supplied by the same untrusted network endpoint
does not establish trust. A previously paired connection must reject a different
pin, rather than silently accept a new identity.

Persist `client_token` in the client's platform credential store; the Linux trust
file stores an HMAC verifier, not that bearer token. All paired clients are
authorized to manage this Linux user's Waybridge sessions and launch applications
as that user. Each connection authenticates again, including media/audio sockets.
Revoking a client rejects its future requests and closes its active stream tunnels.

The `identity.cert.der`, `identity.key.der` and trust file must be private regular
files. A partial or corrupted identity fails startup rather than generating a new
identity without explicit owner action. File locks prevent conflicting processes
from using the same state directory or local administration endpoint.

## Wire contract version 1

Start a TLS connection and send one UTF-8 JSON object terminated by LF. Requests
are limited to 65,536 bytes including LF. The TLS handshake and first request each
have a ten-second timeout. A management connection handles one request/response,
then closes; session creation may take up to 35 seconds. Responses are limited
to 4 MiB. There are at most 64 remote connections, eight local administrators,
four concurrent helper processes and 32 paired clients.

Success:

```json
{"id":1,"ok":true,"result":{"version":1}}
```

Failure:

```json
{"id":1,"ok":false,"error":{"code":"unauthorized","message":"Client is not authorized"}}
```

`id` echoes the request's value, or null. Failure codes are `bad_request`,
`unauthorized`, and `operation_failed`. Admission overload closes the connection;
an idle TLS or initial-request timeout closes it without an application response.
Request bodies, pairing codes, tokens and session credentials are not logged.

Public service metadata:

```json
{"id":1,"op":"hello"}
```

Pair using the locally generated code:

```json
{"id":2,"op":"pair","pair_code":"LOCAL_ONE_USE_CODE","label":"Windows laptop"}
```

The successful result contains `client_id`, `client_token` and
`certificate_sha256`. Pending codes expire at the locally requested TTL (10–600
seconds, default 120) and do not survive a service restart. Invalid labels or a
full client table do not consume an otherwise valid code.

Authenticated management requests carry both credential fields:

```json
{"id":3,"op":"ensure","client_id":"ID","client_token":"TOKEN","session":"waybridge-0","mode":"auto","fps":60,"bitrate":30000,"codec":"auto","audio":false,"xwayland":false}
```

`list`, `ensure`, `info`, `applications`, `launch-plan`, `launch`, and `end` use the same request
fields/results as `scripts/session-control.py`. The configured compositor binary
overrides any network `binary` field. The helper runs locally with isolated Python
imports, controlled stdio, bounded responses and PID/ownership checks. `ensure`
returns the session's v3 `token` only after client authentication; `info` and
`list` omit it. `mode:"attach"` never starts or adopts a stale session.

Newly created sessions return a `configuration` report with requested values and
their sources. Omitted media fields preserve Linux defaults; explicit client
requests must satisfy the same limits as startup and live stream changes. The
service fixes the compositor and configuration path; a remote `binary` or
`config` field cannot replace them. An existing-session reconnect retains that
session's settings and does not load a new configuration. Live replies identify
effective settings and rejection/fallback reasons without rewriting the TOML.

`launch-plan` computes the effective argv/desktop source, action, working
directory, activation mode and override provenance without executing it. Only
environment variable names are reported. `launch` accepts an installed desktop
ID with optional `action` or `uris`, or a separate literal `argv` mode with
optional `cwd`, `env` and `unset_env`. Desktop requests use the scoped TOML for
process overrides; action and nonempty URI lists cannot be combined. Dispatch
and process creation are not window readiness. See [launch examples](CONFIGURATION.md).

Only one Waybridge session per user is supported. `auto` reconnects an existing
session even if its name differs; `new` rejects an existing session. Creation
checks logind for this UID’s active/online X11 or Wayland user sessions.
Attachment bypasses that check. Later display-manager logins are not intercepted.
Application and wallet data are shared by default; see [session policy](SESSION_POLICY.md).

`clients` lists client identifiers, labels and creation times without verifiers.
`revoke` requires `revoke_client_id` to identify the target; `client_id` still
identifies the authenticated caller. These operations are also available through
the owner-only Unix socket, where credentials are not needed.

## Streaming

For each v3 control, media or audio channel, open a fresh pinned TLS connection:

```json
{"id":4,"op":"stream","client_id":"ID","client_token":"TOKEN","session":"waybridge-0"}
```

After session metadata/PID and client checks, the service connects only to that
session's loopback listener. It replies:

```json
{"id":4,"ok":true,"result":{"stream_ready":true,"version":3}}
```

Following that LF, the TLS connection carries the exact bidirectional v3 bytes.
The native viewer sends its usual length-framed hello and the authorized session
token. Additional media/audio channels use their usual v3 hello and media key,
each inside a newly authenticated TLS tunnel. The gateway preserves bytes already
buffered after the JSON prefix. TLS uses bounded asynchronous copy buffers and
TCP backpressure; it does not pass media through the C# GUI or a Python process.

Stopping or restarting this entry point closes its connections but does not end
detached compositor sessions. Session termination remains a separate `end`
operation. The provided user-unit template uses `KillMode=process` to preserve
that distinction. Host logout/user-service shutdown policy remains a deployment
setting and must be configured for the desired persistence.

## Interactive Shell (`op: "shell"`)

Authenticated clients can also open an interactive Linux PTY over a pinned TLS
connection bound to an active graphical session:

```json
{"id":5,"op":"shell","client_id":"ID","client_token":"TOKEN","session":"waybridge-0","cols":120,"rows":30,"term":"xterm-256color"}
```

After verifying client trust and the live session descriptor/compositor PID, the
service allocates a PTY, spawns the user's shell, and replies:

```json
{"id":5,"ok":true,"result":{"shell_ready":true,"version":1}}
```

Following that LF, the stream switches to 5-byte big-endian frames
(`[u8 kind][u32 length][payload]`): `1` (`DATA`, bidirectional PTY bytes),
`2` (`RESIZE`, `[u16 cols][u16 rows]` → `TIOCSWINSZ`), `3` (`EXIT`,
`[i32 code][u8 reason]`, where reason `0` = normal exit, `1` = remote session
ended, `2` = client revoked), and `4`/`5` (`PING`/`PONG`).

For `bash`, `zsh`, and `fish`, the service generates a private `0700`/`0600`
wrapper RC under its runtime directory that sources the user's normal shell
startup files first, then exports the active Waybridge graphical and audio
session environment (`WAYLAND_DISPLAY`, `DISPLAY`, `DBUS_SESSION_BUS_ADDRESS`,
`PULSE_SINK`, `PIPEWIRE_NODE`, `PULSE_SOURCE`, `WAYBRIDGE_SHELL=1`,
`WAYBRIDGE_SESSION`), unsets host IM overrides (`GTK_IM_MODULE`, `QT_IM_MODULE`,
`XMODIFIERS`, etc.), and finally sources an optional user hook at
`$XDG_CONFIG_HOME/waybridge/shellrc` (or `shellrc.fish` for `fish`). Temporary
wrapper files are deleted on exit. If the session compositor exits or the client
is revoked, the service sends `EXIT` and terminates the shell process group
immediately.

## Install and verify

```sh
cargo build --release -p waybridge -p waybridge-service
python3 service/install-user-service.py
systemctl --user daemon-reload
systemctl --user enable --now waybridge.service
cargo test -p waybridge-service
cargo clippy -p waybridge-service --all-targets -- -D warnings
python3 service/tests/direct_service_smoke.py
python3 service/tests/direct_service_smoke.py --compositor target/debug/waybridge
```

The installer copies the built binaries and writes a user unit. It does not start
the service or change login persistence settings automatically. It also installs
independent Waybridge session targets, refuses to overwrite modified units, and
installs the static Portal default only with explicit `--portal-directory`.
Component selection and lifecycle examples are in [desktop integration](INTEGRATION.md).

## Terminal management

Run `waybridge-service tui` as the Linux user who owns the service. Use a local terminal or allocate a TTY over SSH. To manage an isolated instance, pass the same `--runtime-dir` as `serve`. In offline mode, the editor uses the XDG configuration path or an explicit `--config FILE`. The online view obtains the running configuration and its source from the local administration socket.

| Keys | Action |
|---|---|
| `1`–`4`, `Tab`, `Shift+Tab` | Switch between overview and pairing, sessions and logs, trusted devices, and settings |
| `p` on overview | Show an expiring one-use pairing code and the full certificate fingerprint |
| `h` or `u` on overview | Check the environment, or start/restart the matching user service when available |
| `x` in sessions or devices | Preview and confirm ending the selected session or revoking the selected device |
| `c`, `a`, `i` in settings | Open main configuration, application overrides, or desktop components |
| `Enter`, `s`, `y` in settings | Edit, validate and preview the change, then confirm saving. Use `Ctrl+S` to finish a multiline draft |
| `q` or `Ctrl+C` | Leave the terminal interface |

The main editor covers `[service]`, `[session]`, `[limits]`, and `[media_budget]`. The save preview reports when changes take effect. Restarting the service is a separate action. Application overrides use the [Waybridge TOML grammar](CONFIGURATION.md#waybridge-specific-desktop-overrides) with a launch-plan preview before saving. Desktop components expose the Waybridge Portal preferences and the optional notification provider user unit.

## Smoke test

The smoke test
uses temporary state, private runtime directories, actual loopback TLS and no
real credentials. Its optional compositor mode adds a real v3 handshake through
the encrypted gateway and verifies that disconnect leaves the session available.

API references: [rustls](https://docs.rs/rustls/0.23.46/rustls/),
[rcgen certificate generation](https://docs.rs/rcgen/0.14.10/rcgen/fn.generate_simple_self_signed.html).
