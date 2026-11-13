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
        command = [str(service), "--state-dir", str(state), "--runtime-dir", str(runtime / "entry")]
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
                response = rpc({"op": "ensure", "session": "actual", "mode": "auto", "audio": False, "xwayland": False, **credentials})
                assert response["ok"], response
                actual = response["result"]
                marker = root / "launched.json"
                launch_code = "import json,os,pathlib;pathlib.Path(" + repr(str(marker)) + ").write_text(json.dumps({k:os.environ.get(k) for k in ['WAYLAND_DISPLAY','DBUS_SESSION_BUS_ADDRESS']}))"
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
                with connect() as connection:
                    connection.sendall(json.dumps({"op": "stream", "session": actual["name"], **credentials}).encode() + b"\n")
                    assert receive_json(connection)["result"]["stream_ready"]
                    hello = json.dumps({"type": "hello", "version": 3, "token": actual["token"], "dpi": 96}).encode()
                    connection.sendall(struct.pack(">I", len(hello) + 1) + b"\0" + hello)
                    header = connection.recv(4)
                    assert len(header) == 4
                    length = struct.unpack(">I", header)[0]
                    message = bytearray()
                    while len(message) < length:
                        message += connection.recv(length - len(message))
                    assert message[0] == 0 and json.loads(message[1:])["type"] == "hello"
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
            print("TLS pairing, pinning, authorization, single-use codes, session metadata, opaque duplex gateway, revocation and bounds passed" + ("; actual compositor handshake passed" if args.compositor else ""))
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
