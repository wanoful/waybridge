#!/usr/bin/env python3
"""Exercise authenticated TLS shell channels, wrapper RC injection, resize, session end, and revocation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import struct
import subprocess
import tempfile
import time

KIND_DATA = 1
KIND_RESIZE = 2
KIND_EXIT = 3
KIND_PING = 4
KIND_PONG = 5

REASON_EXITED = 0
REASON_SESSION_ENDED = 1
REASON_REVOKED = 2


def recv_json(conn):
    data = bytearray()
    while not data.endswith(b"\n"):
        part = conn.recv(1)
        if not part:
            raise EOFError("connection closed before JSON reply")
        data += part
        assert len(data) <= 4 * 1024 * 1024
    return json.loads(data)


def recv_exact(conn, count):
    data = bytearray()
    while len(data) < count:
        part = conn.recv(count - len(data))
        if not part:
            raise EOFError("connection closed before framed payload")
        data += part
    return bytes(data)


def send_frame(conn, kind, payload=b""):
    conn.sendall(struct.pack(">BI", kind, len(payload)) + payload)


def recv_frame(conn):
    header = recv_exact(conn, 5)
    kind, length = struct.unpack(">BI", header)
    payload = recv_exact(conn, length) if length else b""
    return kind, payload


def proc_started(pid):
    return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19]


def write_session_descriptor(runtime: Path, name: str, pid: int):
    payload = {
        "version": 1,
        "pid": pid,
        "started": proc_started(pid),
        "environment": {
            "WAYLAND_DISPLAY": name,
            "DISPLAY": ":1099",
            "XDG_SESSION_TYPE": "wayland",
            "XDG_CURRENT_DESKTOP": "Waybridge",
            "PULSE_SINK": "waybridge_test_sink",
            "GTK_IM_MODULE": None,
            "QT_IM_MODULE": None,
        },
        "connection": {
            "listen": "127.0.0.1:49099",
            "token_file": str(runtime / f"{name}.token"),
        },
    }
    path = runtime / f"{name}.session.json"
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as fp:
        json.dump(payload, fp)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--service", type=Path, default=Path("target/debug/waybridge-service"))
    args = parser.parse_args()
    service = args.service.resolve()
    bash_path = shutil.which("bash")
    assert bash_path, "bash is required for shell_smoke"

    with tempfile.TemporaryDirectory(prefix="waybridge-shell-smoke-") as tmp:
        root = Path(tmp)
        runtime, state, home = root / "runtime", root / "state", root / "home"
        config_home = root / "config"
        runtime.mkdir(mode=0o700)
        state.mkdir(mode=0o700)
        home.mkdir(mode=0o700)
        (config_home / "waybridge").mkdir(parents=True, mode=0o700)

        # User's own .bashrc intentionally sets conflicting IM vars and custom vars
        (home / ".bashrc").write_text(
            "export MY_USER_RC=preserved\n"
            "export GTK_IM_MODULE=fcitx\n"
            "export QT_IM_MODULE=fcitx\n"
            "export DISPLAY=:0\n",
            encoding="utf-8",
        )
        # Optional Waybridge-specific shellrc hook
        (config_home / "waybridge" / "shellrc").write_text(
            "export WB_CUSTOM_RC=loaded\n",
            encoding="utf-8",
        )

        env = os.environ.copy()
        env.update(
            HOME=str(home),
            SHELL=bash_path,
            XDG_RUNTIME_DIR=str(runtime),
            XDG_CONFIG_HOME=str(config_home),
            XDG_STATE_HOME=str(root / "app-state"),
            XDG_DATA_HOME=str(root / "data"),
            XDG_CACHE_HOME=str(root / "cache"),
        )
        entry_runtime = runtime / "entry"
        cmd = [str(service), "--state-dir", str(state), "--runtime-dir", str(entry_runtime)]
        daemon = subprocess.Popen(
            [*cmd, "serve", "--listen", "127.0.0.1:0", "--compositor", "/bin/true"],
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        fake_compositors = []
        try:
            ready = json.loads(daemon.stdout.readline())
            host, port_s = ready["listen"].rsplit(":", 1)
            port = int(port_s)
            fingerprint = ready["certificate_sha256"]

            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE

            def connect_tls():
                raw = socket.create_connection((host, port), timeout=5)
                tls = ctx.wrap_socket(raw, server_hostname=host)
                tls.settimeout(5)
                assert hashlib.sha256(tls.getpeercert(binary_form=True)).hexdigest() == fingerprint
                return tls

            # 1. Verify hello advertises "shell"
            with connect_tls() as tls:
                tls.sendall(b'{"id":1,"op":"hello"}\n')
                hello = recv_json(tls)
                assert hello["ok"] and "shell" in hello["result"]["features"], hello

            # 2. Pair a client
            pair_out = json.loads(subprocess.check_output([*cmd, "pair", "--expires", "60"], env=env, text=True))
            code = pair_out["result"]["pair_code"]
            with connect_tls() as tls:
                tls.sendall((json.dumps({"id": 2, "op": "pair", "pair_code": code, "label": "shell-test"}) + "\n").encode())
                paired = recv_json(tls)
                assert paired["ok"], paired
                creds = {
                    "client_id": paired["result"]["client_id"],
                    "client_token": paired["result"]["client_token"],
                }

            # 3. Missing session fails cleanly
            with connect_tls() as tls:
                tls.sendall((json.dumps({"id": 3, "op": "shell", "session": "missing-0", **creds}) + "\n").encode())
                err = recv_json(tls)
                assert not err["ok"], err

            # 4. Live session + bash wrapper RC + resize + ping/pong + exit code
            comp1 = subprocess.Popen(["sleep", "60"])
            fake_compositors.append(comp1)
            write_session_descriptor(runtime, "waybridge-0", comp1.pid)

            with connect_tls() as tls:
                tls.sendall((json.dumps({
                    "id": 4,
                    "op": "shell",
                    "session": "waybridge-0",
                    "cols": 100,
                    "rows": 30,
                    "term": "xterm-256color",
                    **creds,
                }) + "\n").encode())
                reply = recv_json(tls)
                assert reply["ok"] and reply["result"]["shell_ready"], reply

                # Ping/Pong
                send_frame(tls, KIND_PING)
                # Resize to 132x43
                send_frame(tls, KIND_RESIZE, struct.pack(">HH", 132, 43))
                # Query env & stty size & exit with code 7
                probe = (
                    "printf 'ENV:%s|%s|%s|%s|%s|%s|%s|%s\\n' "
                    "\"$WAYLAND_DISPLAY\" \"$DISPLAY\" \"$PULSE_SINK\" "
                    "\"$WAYBRIDGE_SHELL\" \"$MY_USER_RC\" \"$WB_CUSTOM_RC\" "
                    "\"${GTK_IM_MODULE:-UNSET}\" \"${QT_IM_MODULE:-UNSET}\"\n"
                    "printf 'SIZE:%s\\n' \"$(stty size)\"\n"
                    "exit 7\n"
                ).encode()
                send_frame(tls, KIND_DATA, probe)

                output = bytearray()
                got_pong = False
                exit_info = None
                deadline = time.monotonic() + 8
                while time.monotonic() < deadline:
                    kind, payload = recv_frame(tls)
                    if kind == KIND_DATA:
                        output.extend(payload)
                    elif kind == KIND_PONG:
                        got_pong = True
                    elif kind == KIND_EXIT:
                        exit_info = struct.unpack(">iB", payload)
                        break
                text = output.decode("utf-8", errors="replace")
                assert got_pong, "did not receive PONG"
                assert "ENV:waybridge-0|:1099|waybridge_test_sink|1|preserved|loaded|UNSET|UNSET" in text, text
                assert "SIZE:43 132" in text, text
                assert exit_info == (7, REASON_EXITED), exit_info

            # 5. Real C# Waybridge.Cli end-to-end against Rust waybridge-service over pinned TLS
            dotnet = shutil.which("dotnet")
            cli_dll = Path("gui/Waybridge.Cli/bin/Release/net10.0/waybridge.dll").resolve()
            if dotnet and cli_dll.is_file():
                cli_state = root / "cli-state"
                cli_state.mkdir(mode=0o700)
                profile_id = "22222222-3333-4444-5555-666666666666"
                (cli_state / "profiles.json").write_text(
                    json.dumps([{
                        "id": profile_id,
                        "name": "Lab Linux",
                        "host": host,
                        "port": port,
                        "certificate_sha256": fingerprint,
                        "client_id": creds["client_id"],
                        "session": "waybridge-0",
                        "session_mode": "auto",
                        "auto_reconnect": True,
                        "settings": {},
                    }]),
                    encoding="utf-8",
                )
                cli_env = env.copy()
                cli_env["WAYBRIDGE_CLIENT_TOKEN"] = creds["client_token"]
                listed = subprocess.check_output(
                    [dotnet, str(cli_dll), "list", "--state-dir", str(cli_state)],
                    env=cli_env,
                    text=True,
                )
                assert "Lab Linux" in listed and profile_id in listed, listed

                proc = subprocess.run(
                    [dotnet, str(cli_dll), "shell", "--connection", "Lab Linux",
                     "--state-dir", str(cli_state), "--no-connect"],
                    input="printf 'CLI:%s|%s|%s\\n' \"$WAYLAND_DISPLAY\" \"$WB_CUSTOM_RC\" \"${GTK_IM_MODULE:-UNSET}\"\nexit 5\n",
                    env=cli_env,
                    text=True,
                    capture_output=True,
                    timeout=10,
                )
                assert proc.returncode == 5, (proc.returncode, proc.stdout, proc.stderr)
                assert "CLI:waybridge-0|loaded|UNSET" in proc.stdout, (proc.stdout, proc.stderr)
                print("PASS: C# waybridge CLI connects over pinned TLS to Rust waybridge-service shell channel")

            # 6. Ending the remote session terminates the active shell immediately
            with connect_tls() as tls:
                tls.sendall((json.dumps({"id": 5, "op": "shell", "session": "waybridge-0", **creds}) + "\n").encode())
                reply = recv_json(tls)
                assert reply["ok"] and reply["result"]["shell_ready"], reply
                send_frame(tls, KIND_DATA, b"echo ALIVE_MARKER\n")
                seen = bytearray()
                while b"ALIVE_MARKER" not in seen:
                    kind, payload = recv_frame(tls)
                    if kind == KIND_DATA:
                        seen.extend(payload)
                comp1.terminate()
                comp1.wait(timeout=5)
                exit_info = None
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    kind, payload = recv_frame(tls)
                    if kind == KIND_EXIT:
                        exit_info = struct.unpack(">iB", payload)
                        break
                assert exit_info == (0, REASON_SESSION_ENDED), exit_info

            # 6. Revoking client terminates active shell immediately
            comp2 = subprocess.Popen(["sleep", "60"])
            fake_compositors.append(comp2)
            write_session_descriptor(runtime, "waybridge-0", comp2.pid)
            with connect_tls() as tls:
                tls.sendall((json.dumps({"id": 6, "op": "shell", "session": "waybridge-0", **creds}) + "\n").encode())
                reply = recv_json(tls)
                assert reply["ok"] and reply["result"]["shell_ready"], reply
                subprocess.check_output([*cmd, "revoke", creds["client_id"]], env=env)
                exit_info = None
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    kind, payload = recv_frame(tls)
                    if kind == KIND_EXIT:
                        exit_info = struct.unpack(">iB", payload)
                        break
                assert exit_info == (0, REASON_REVOKED), exit_info

            # Temp wrapper RC dirs cleaned up
            shells_dir = entry_runtime / "shells"
            if shells_dir.exists():
                time.sleep(0.1)
                assert list(shells_dir.iterdir()) == [], list(shells_dir.iterdir())

            print("PASS: TLS shell channel, bash wrapper RC, resize, session-end teardown, revocation, and cleanup")
        finally:
            for proc in fake_compositors:
                if proc.poll() is None:
                    proc.terminate()
                    proc.wait(timeout=5)
            daemon.terminate()
            daemon.wait(timeout=5)

    zsh_path = shutil.which("zsh")
    if zsh_path:
        with tempfile.TemporaryDirectory(prefix="waybridge-zsh-smoke-") as tmp:
            root = Path(tmp)
            runtime, state, home, config_home = root / "runtime", root / "state", root / "home", root / "config"
            runtime.mkdir(mode=0o700)
            state.mkdir(mode=0o700)
            home.mkdir(mode=0o700)
            (config_home / "waybridge").mkdir(parents=True, mode=0o700)
            (home / ".zshrc").write_text(
                "export MY_ZSH_RC=preserved\nexport GTK_IM_MODULE=fcitx\n",
                encoding="utf-8",
            )
            (config_home / "waybridge" / "shellrc").write_text("export WB_ZSH_HOOK=loaded\n", encoding="utf-8")
            env = os.environ.copy()
            env.pop("ZDOTDIR", None)
            env.update(
                HOME=str(home),
                SHELL=zsh_path,
                XDG_RUNTIME_DIR=str(runtime),
                XDG_CONFIG_HOME=str(config_home),
                XDG_STATE_HOME=str(root / "app-state"),
                XDG_DATA_HOME=str(root / "data"),
                XDG_CACHE_HOME=str(root / "cache"),
            )
            entry_runtime = runtime / "entry"
            cmd = [str(service), "--state-dir", str(state), "--runtime-dir", str(entry_runtime)]
            daemon = subprocess.Popen(
                [*cmd, "serve", "--listen", "127.0.0.1:0", "--compositor", "/bin/true"],
                env=env,
                stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            comp = subprocess.Popen(["sleep", "60"])
            try:
                ready = json.loads(daemon.stdout.readline())
                host, port_s = ready["listen"].rsplit(":", 1)
                port = int(port_s)
                ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                ctx.check_hostname = False
                ctx.verify_mode = ssl.CERT_NONE
                pair_out = json.loads(subprocess.check_output([*cmd, "pair", "--expires", "60"], env=env, text=True))
                code = pair_out["result"]["pair_code"]
                with ctx.wrap_socket(socket.create_connection((host, port), timeout=5), server_hostname=host) as tls:
                    tls.settimeout(5)
                    tls.sendall((json.dumps({"id": 1, "op": "pair", "pair_code": code, "label": "zsh-test"}) + "\n").encode())
                    paired = recv_json(tls)
                    creds = {"client_id": paired["result"]["client_id"], "client_token": paired["result"]["client_token"]}
                write_session_descriptor(runtime, "waybridge-0", comp.pid)
                with ctx.wrap_socket(socket.create_connection((host, port), timeout=5), server_hostname=host) as tls:
                    tls.settimeout(5)
                    tls.sendall((json.dumps({"id": 2, "op": "shell", "session": "waybridge-0", **creds}) + "\n").encode())
                    assert recv_json(tls)["ok"]
                    probe = (
                        "printf 'ZSH:%s|%s|%s|%s|%s\\n' "
                        "\"$WAYLAND_DISPLAY\" \"$MY_ZSH_RC\" \"$WB_ZSH_HOOK\" "
                        "\"${GTK_IM_MODULE:-UNSET}\" \"${ZDOTDIR:-UNSET}\"\nexit 0\n"
                    ).encode()
                    send_frame(tls, KIND_DATA, probe)
                    out = bytearray()
                    exit_info = None
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline:
                        kind, payload = recv_frame(tls)
                        if kind == KIND_DATA:
                            out.extend(payload)
                        elif kind == KIND_EXIT:
                            exit_info = struct.unpack(">iB", payload)
                            break
                    text = out.decode("utf-8", errors="replace")
                    assert "ZSH:waybridge-0|preserved|loaded|UNSET|UNSET" in text, text
                    assert exit_info == (0, REASON_EXITED), exit_info
                print("PASS: zsh ZDOTDIR wrapper RC preserves user .zshrc, restores ZDOTDIR, and overrides IM env")
            finally:
                comp.terminate()
                comp.wait(timeout=5)
                daemon.terminate()
                daemon.wait(timeout=5)


if __name__ == "__main__":
    main()
