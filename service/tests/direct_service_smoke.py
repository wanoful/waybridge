#!/usr/bin/env python3
"""Exercise the actual TLS daemon, pairing, gateway framing and revocation.

Uses only temporary state and loopback endpoints. --compositor can optionally
exercise a built real Waybridge compositor in addition to the controlled peer.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import time


def receive_json(connection):
    data = bytearray()
    while not data.endswith(b"\n"):
        part = connection.recv(1)
        if not part:
            raise EOFError("connection closed before reply")
        data += part
        assert len(data) <= 4 * 1024 * 1024
    return json.loads(data)


def receive_exact(connection, count):
    data = bytearray()
    while len(data) < count:
        block = connection.recv(count - len(data))
        if not block:
            raise EOFError("connection closed before framed payload")
        data += block
    return data


def process_identity(pid):
    return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--service", type=Path, default=Path("target/debug/waybridge-service"))
    parser.add_argument("--compositor", type=Path)
    args = parser.parse_args()
    service = args.service.resolve()
    compositor = args.compositor.resolve() if args.compositor else Path("/bin/true")
    with tempfile.TemporaryDirectory(prefix="waybridge-direct-smoke-") as temporary:
        root = Path(temporary)
        runtime, state = root / "runtime", root / "state"
        runtime.mkdir(mode=0o700)
        state.mkdir(mode=0o700)
        env = os.environ.copy()
        env.update(XDG_RUNTIME_DIR=str(runtime), XDG_DATA_HOME=str(root / "data"),
                   XDG_STATE_HOME=str(root / "app-state"), XDG_CONFIG_HOME=str(root / "config"),
                   XDG_CACHE_HOME=str(root / "cache"))
        config = root / "explicit-server.toml"
        config.write_text('[service]\nlisten = "127.0.0.1:1"\n'
                          '[session]\nfps = 37\nbitrate = 17000\ncodec = "png"\n'
                          '[limits]\nmax_fps = 75\nmax_bitrate = 55000\nallowed_codecs = ["png", "h264"]\n')
        config_bytes = config.read_bytes()
        command = [str(service), "--config", str(config), "--state-dir", str(state), "--runtime-dir", str(runtime / "entry")]
        daemon = subprocess.Popen([*command, "serve", "--listen", "127.0.0.1:0", "--compositor", str(compositor)],
                                  env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        actual = None
        backend = socket.socket()
        backend.bind(("127.0.0.1", 0))
        backend.listen()
        backend.settimeout(5)
        try:
            ready_path = runtime / "entry/ready.json"
            deadline = time.monotonic() + 15
            while not ready_path.exists():
                if daemon.poll() is not None:
                    raise AssertionError(daemon.stderr.read())
                if time.monotonic() > deadline:
                    raise TimeoutError("TLS service startup timed out")
                time.sleep(.02)
            ready = json.loads(ready_path.read_text())
            duplicate = subprocess.run([*command, "serve", "--listen", "127.0.0.1:0", "--compositor", str(compositor)],
                                       env=env, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=5)
            assert duplicate.returncode != 0 and ready_path.exists()
            address, port = ready["listen"].rsplit(":", 1)
            pin = ready["certificate_sha256"]
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
            context.minimum_version = ssl.TLSVersion.TLSv1_2

            def connect():
                connection = context.wrap_socket(socket.create_connection((address, int(port)), timeout=40), server_hostname="waybridge.local")
                assert hashlib.sha256(connection.getpeercert(binary_form=True)).hexdigest() == pin
                return connection

            def rpc(request):
                with connect() as connection:
                    connection.sendall(json.dumps(request).encode() + b"\n")
                    return receive_json(connection)

            assert rpc({"op": "hello"})["result"]["version"] == 1
            context.maximum_version = ssl.TLSVersion.TLSv1_2
            with connect() as connection:
                assert connection.version() == "TLSv1.2"
                connection.sendall(b'{"op":"hello"}\n')
                assert receive_json(connection)["ok"]
            context.maximum_version = ssl.TLSVersion.MAXIMUM_SUPPORTED
            assert rpc({"op": "list"})["error"]["code"] == "unauthorized"
            assert not rpc({"op": "pair", "pair_code": "wrong", "label": "test"})["ok"]
            pair = json.loads(subprocess.check_output([*command, "pair"], env=env))["result"]
            assert pair["certificate_sha256"] == pin
            paired = rpc({"op": "pair", "pair_code": pair["pair_code"], "label": "Windows 测试"})["result"]
            credentials = {key: paired[key] for key in ["client_id", "client_token"]}
            assert not rpc({"op": "pair", "pair_code": pair["pair_code"], "label": "replay"})["ok"]
            assert rpc({"op": "list", **credentials})["result"] == []
            assert not rpc({"op": "list", **credentials, "client_token": "wrong"})["ok"]
            assert rpc({"op": "applications", **credentials})["ok"]

            token = runtime / "controlled.token"
            token.write_text("ControlledSessionToken-NotARealCredential")
            token.chmod(0o600)
            descriptor = runtime / "controlled.session.json"
            descriptor.write_text(json.dumps({"version": 1, "pid": os.getpid(), "started": process_identity(os.getpid()),
                "connection": {"listen": f"127.0.0.1:{backend.getsockname()[1]}", "token_file": str(token)}}))
            descriptor.chmod(0o600)
            information = rpc({"op": "ensure", "session": "controlled", "mode": "attach", **credentials})
            assert information["result"]["token"] == token.read_text()
            assert "token" not in rpc({"op": "info", "session": "controlled", **credentials})["result"]

            with connect() as connection:
                payload = b"\0\0\0\x07\x00opaque"
                connection.sendall(json.dumps({"op": "stream", "session": "controlled", **credentials}).encode() + b"\n" + payload)
                assert receive_json(connection)["result"]["stream_ready"]
                peer, _ = backend.accept()
                with peer:
                    assert peer.recv(len(payload)) == payload
                    peer.sendall(b"opaque-reply")
                    assert connection.recv(64) == b"opaque-reply"
                    # A second client's revocation must not cancel/recreate the
                    # copy future and lose buffered bytes in this live stream.
                    next_pair = json.loads(subprocess.check_output([*command, "pair"], env=env))["result"]
                    second = rpc({"op": "pair", "pair_code": next_pair["pair_code"], "label": "second"})["result"]
                    assert rpc({"op": "revoke", "revoke_client_id": second["client_id"], **credentials})["result"]["revoked"]
                    peer.sendall(b"still-alive")
                    assert connection.recv(64) == b"still-alive"
                    subprocess.check_output([*command, "revoke", credentials["client_id"]], env=env)
                    connection.settimeout(3)
                    assert connection.recv(1) == b""
            assert not rpc({"op": "list", **credentials})["ok"]
            serialized = (state / "trust.json").read_text()
            assert credentials["client_token"] not in serialized
            assert pair["pair_code"] not in serialized

            with connect() as connection:
                connection.sendall(b"x" * 65537)
                assert receive_json(connection)["error"]["code"] == "bad_request"
            with connect() as connection:
                connection.sendall(b"[]\n")
                assert receive_json(connection)["error"]["code"] == "bad_request"

            # Stale PID and over-permissive descriptors fail before tunnelling.
            pair = json.loads(subprocess.check_output([*command, "pair"], env=env))["result"]
            paired = rpc({"op": "pair", "pair_code": pair["pair_code"], "label": "final"})["result"]
            credentials = {key: paired[key] for key in ["client_id", "client_token"]}
            saved = json.loads(descriptor.read_text())
            saved["started"] = "0"
            descriptor.write_text(json.dumps(saved))
            assert not rpc({"op": "stream", "session": "controlled", **credentials})["ok"]
            descriptor.chmod(0o644)
            assert not rpc({"op": "ensure", "session": "controlled", "mode": "attach", **credentials})["ok"]

            if args.compositor:
                for invalid in [{"fps": 76}, {"bitrate": 55001}, {"codec": "hevc-nvenc"}]:
                    rejected = rpc({"op": "ensure", "session": "actual", "mode": "new", **invalid, **credentials})
                    assert not rejected["ok"], rejected
                    assert not list(runtime.glob("actual*.session.json"))
                response = rpc({"op": "ensure", "session": "actual", "mode": "auto", "audio": False, "xwayland": False,
                                "config": "/client-cannot-choose-config", "binary": "/bin/false", **credentials})
                assert response["ok"], response
                actual = response["result"]
                effective = actual["configuration"]
                assert effective["values"]["session"]["fps"] == 37, effective
                assert effective["values"]["session"]["bitrate"] == 17000, effective
                assert effective["sources"]["session.fps"]["kind"] == "file", effective
                assert effective["values"]["limits"]["max_fps"] == 75, effective
                config.write_text("temporarily invalid configuration")
                reconnected = rpc({"op": "ensure", "session": actual["name"], "mode": "auto", "fps": 999, **credentials})
                assert reconnected["ok"] and not reconnected["result"]["created"], reconnected
                assert reconnected["result"]["pid"] == actual["pid"] and "configuration" not in reconnected["result"]
                config.write_bytes(config_bytes)
                marker = root / "launched.json"
                launch_code = "import json,os,pathlib;pathlib.Path(" + repr(str(marker)) + ").write_text(json.dumps({k:os.environ.get(k) for k in ['WAYLAND_DISPLAY','DBUS_SESSION_BUS_ADDRESS']}))"
                planned = rpc({"op": "launch-plan", "session": actual["name"], "argv": [sys.executable, "-I", "-c", launch_code], **credentials})
                assert planned["ok"] and planned["result"]["kind"] == "argv", planned
                assert not marker.exists(), "launch-plan must not execute the application"
                assert "environment" not in planned["result"] and "token" not in planned["result"]
                launched = rpc({"op": "launch", "session": actual["name"], "argv": [sys.executable, "-I", "-c", launch_code], **credentials})
                assert launched["ok"], launched
                deadline = time.monotonic() + 5
                while not marker.exists():
                    if time.monotonic() > deadline:
                        raise TimeoutError("application launch did not complete")
                    time.sleep(.02)
                inherited = json.loads(marker.read_text())
                assert inherited["WAYLAND_DISPLAY"] == actual["name"]
                assert inherited["DBUS_SESSION_BUS_ADDRESS"]
                applications = root / "data/applications"
                applications.mkdir(parents=True, exist_ok=True)
                desktop_id = "org.waybridge.Controlled.desktop"
                desktop_marker, desktop_script = root / "desktop-result", root / "desktop-fixture"
                desktop_script.write_text('#!/bin/sh\nprintf "%s\\n" "$PYTHONHOME" > "$1"\npwd >> "$1"\n')
                desktop_script.chmod(0o700)
                (applications / desktop_id).write_text(
                    f"[Desktop Entry]\nType=Application\nName=Controlled launch\nExec={desktop_script} {desktop_marker}\n")
                overrides = root / "config/waybridge/desktop-overrides"
                overrides.mkdir(parents=True)
                (overrides / (desktop_id + ".toml")).write_text(
                    f'version = 1\n[default]\ncwd = "{root}"\n[default.env]\nPYTHONHOME = "/application-only-python-home"\n')
                desktop_plan = rpc({"op": "launch-plan", "session": actual["name"], "app": desktop_id, **credentials})
                assert desktop_plan["ok"] and desktop_plan["result"]["env_keys"] == ["PYTHONHOME"], desktop_plan
                assert not desktop_marker.exists()
                desktop_launch = rpc({"op": "launch", "session": actual["name"], "app": desktop_id, **credentials})
                assert desktop_launch["ok"] and desktop_launch["result"]["status"] == "dispatched", desktop_launch
                deadline = time.monotonic() + 5
                expected_desktop_result = ["/application-only-python-home", str(root)]
                while not desktop_marker.exists() or desktop_marker.read_text().splitlines() != expected_desktop_result:
                    if time.monotonic() > deadline:
                        raise TimeoutError("TLS desktop override did not launch the application")
                    time.sleep(.02)
                assert desktop_marker.read_text().splitlines() == expected_desktop_result
                with connect() as connection:
                    connection.sendall(json.dumps({"op": "stream", "session": actual["name"], **credentials}).encode() + b"\n")
                    assert receive_json(connection)["result"]["stream_ready"]
                    hello = json.dumps({"type": "hello", "version": 3, "token": actual["token"], "dpi": 96}).encode()
                    connection.sendall(struct.pack(">I", len(hello) + 1) + b"\0" + hello)
                    header = receive_exact(connection, 4)
                    length = struct.unpack(">I", header)[0]
                    message = receive_exact(connection, length)
                    assert message[0] == 0 and json.loads(message[1:])["type"] == "hello"

                    def stream_config(request_id, **settings):
                        body = b"\0" + json.dumps({"type": "stream_config", "request_id": request_id, **settings}).encode()
                        connection.sendall(struct.pack(">I", len(body)) + body)
                        while True:
                            header = receive_exact(connection, 4)
                            size, = struct.unpack(">I", header)
                            payload = receive_exact(connection, size)
                            if payload[0] != 0:
                                continue
                            reply = json.loads(payload[1:])
                            if reply["type"] == "stream_config" and reply["request_id"] == request_id:
                                return reply

                    denied = stream_config(501, fps=76, bitrate=17000, codec="png")
                    assert not denied["ok"] and denied["limits"]["max_fps"] == 75, denied
                    denied_codec = stream_config(502, fps=37, bitrate=17000, codec="hevc")
                    assert not denied_codec["ok"], denied_codec
                    accepted = stream_config(503, fps=39, bitrate=18000, codec="png")
                    assert accepted["ok"] and accepted["fps"] == 39 and accepted["bitrate"] == 18000, accepted
                    assert accepted["persisted"] is False and accepted["source"] == "runtime_request", accepted
                assert config.read_bytes() == config_bytes, "client requests must not rewrite server configuration"
                assert rpc({"op": "info", "session": actual["name"], **credentials})["ok"]
                # A service restart keeps both the pinned identity/trust and the
                # already detached real compositor alive.
                daemon.terminate()
                daemon.wait(timeout=5)
                assert not ready_path.exists()
                daemon = subprocess.Popen([*command, "serve", "--listen", "127.0.0.1:0", "--compositor", str(compositor)],
                                          env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                deadline = time.monotonic() + 15
                while not ready_path.exists():
                    if daemon.poll() is not None:
                        raise AssertionError(daemon.stderr.read())
                    if time.monotonic() > deadline:
                        raise TimeoutError("TLS service restart timed out")
                    time.sleep(.02)
                ready = json.loads(ready_path.read_text())
                address, port = ready["listen"].rsplit(":", 1)
                assert ready["certificate_sha256"] == pin
                assert rpc({"op": "info", "session": actual["name"], **credentials})["ok"]
                assert rpc({"op": "end", "session": actual["name"], **credentials})["ok"]
                actual = None
            print("TLS pairing, pinning, authorization, single-use codes, session metadata, opaque duplex gateway, revocation and bounds passed" + ("; actual compositor handshake, config defaults/limits, nonexecuting launch-plan, live policy and retained-session config independence passed" if args.compositor else ""))
        finally:
            if actual:
                try:
                    os.kill(actual["pid"], signal.SIGTERM)
                except ProcessLookupError:
                    pass
            backend.close()
            daemon.terminate()
            try:
                daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait()
            assert not (runtime / "entry/control.sock").exists(), "owned admin socket was not cleaned"


if __name__ == "__main__":
    main()
