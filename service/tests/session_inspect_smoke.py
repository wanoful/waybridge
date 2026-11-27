#!/usr/bin/env python3
"""End-to-end test: inspect remote session status, windows, SNI background apps,
and session processes over pinned TLS WITHOUT connecting a streaming viewer, then
end the session cleanly via both TLS RPC and the Waybridge CLI.
"""
import hashlib
import json
import os
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
from session_fixture import SessionFixture


def receive_json(connection):
    data = bytearray()
    while not data.endswith(b"\n"):
        part = connection.recv(1)
        if not part:
            raise EOFError("connection closed before reply")
        data += part
        assert len(data) <= 4 * 1024 * 1024
    return json.loads(data)


def wait_until(predicate, timeout=15.0, message="condition timed out"):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = predicate()
        if last:
            return last
        time.sleep(0.1)
    raise AssertionError(f"{message}; last={last!r}")


def main():
    service = ROOT / "target/debug/waybridge-service"
    compositor = ROOT / "target/debug/waybridge"
    assert service.exists() and compositor.exists(), "build waybridge and waybridge-service first"

    subprocess.run(["bash", "tests/build-fixture.sh"], cwd=ROOT, check=True)
    test_app = ROOT / "build/tests/test-app"
    assert test_app.exists()

    with tempfile.TemporaryDirectory(prefix="waybridge-inspect-") as temporary, SessionFixture(temporary) as fixture:
        root = Path(temporary)
        state = root / "service-state"
        entry_runtime = root / "service-runtime"
        state.mkdir(mode=0o700)
        entry_runtime.mkdir(mode=0o700)
        env = {**fixture.env, "GVFS_DISABLE_FUSE": "1"}
        command = [str(service), "--state-dir", str(state), "--runtime-dir", str(entry_runtime)]
        daemon = subprocess.Popen(
            [*command, "serve", "--listen", "127.0.0.1:0", "--compositor", str(compositor)],
            env=env,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        children = []
        try:
            ready_path = entry_runtime / "ready.json"
            wait_until(lambda: ready_path.exists() or daemon.poll() is not None, 15.0, "service ready")
            assert daemon.poll() is None, daemon.stderr.read()
            ready = json.loads(ready_path.read_text())
            address, port_str = ready["listen"].rsplit(":", 1)
            port = int(port_str)
            pin = ready["certificate_sha256"]

            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
            ctx.minimum_version = ssl.TLSVersion.TLSv1_2

            def rpc(request):
                with ctx.wrap_socket(
                    socket.create_connection((address, port), timeout=35),
                    server_hostname="waybridge.local",
                ) as conn:
                    assert hashlib.sha256(conn.getpeercert(binary_form=True)).hexdigest() == pin
                    conn.sendall(json.dumps(request).encode() + b"\n")
                    return receive_json(conn)

            hello = rpc({"op": "hello"})
            assert hello["ok"] and "session_details" in hello["result"]["features"], hello

            pair = json.loads(subprocess.check_output([*command, "pair"], env=env))["result"]
            paired = rpc({"op": "pair", "pair_code": pair["pair_code"], "label": "Inspect test"})["result"]
            creds = {"client_id": paired["client_id"], "client_token": paired["client_token"]}

            # 1. No session running yet.
            empty = rpc({"op": "list", "details": True, **creds})
            assert empty["ok"] and empty["result"] == [], empty

            # 2. Start a real Waybridge session WITHOUT connecting any viewer.
            ensured = rpc({
                "op": "ensure",
                "session": "inspect-0",
                "mode": "auto",
                "audio": False,
                "xwayland": False,
                "codec": "png",
                **creds,
            })
            assert ensured["ok"], ensured
            session_info = ensured["result"]
            session_name = session_info["name"]

            # 3. Launch a real Wayland GUI window, an SNI background app, and a headless background process.
            launched_sleep = rpc({
                "op": "launch",
                "session": session_name,
                "argv": ["sleep", "60"],
                **creds,
            })
            assert launched_sleep["ok"], launched_sleep

            launched_window = rpc({
                "op": "launch",
                "session": session_name,
                "argv": [str(test_app)],
                "env": {"WAYBRIDGE_TEST_APP_ID": "org.waybridge.InspectApp"},
                **creds,
            })
            assert launched_window["ok"], launched_window

            tray_log = root / "tray.jsonl"
            tray_child = subprocess.Popen(
                ["/usr/bin/python3", "tests/tray_fixture.py", "--log", str(tray_log)],
                cwd=ROOT,
                env=env,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            children.append(tray_child)
            assert tray_child.stdout.readline().strip() == "ready"

            # 4. Inspect session over TLS without connecting a streaming viewer.
            def inspect_complete():
                reply = rpc({"op": "list", "details": True, **creds})
                assert reply["ok"], reply
                items = reply["result"]
                if len(items) != 1:
                    return None
                item = items[0]
                has_win = any(w["app_id"] == "org.waybridge.InspectApp" for w in item.get("windows", []))
                has_bg = any(b["app_id"] == "org.waybridge.TrayFixture" for b in item.get("background_apps", []))
                has_proc = any(p["name"] == "sleep" for p in item.get("processes", []))
                return item if (has_win and has_bg and has_proc) else None

            observed = wait_until(inspect_complete, 15.0, "session details did not populate")
            assert observed["name"] == session_name
            assert observed["state_available"] is True
            assert observed["viewer_connected"] is False
            assert observed["uptime_seconds"] >= 0
            assert "token" not in observed
            # Internal daemons and raw command-line arguments must never appear.
            for proc in observed["processes"]:
                assert proc["name"] not in ("waybridge", "waybridge-servi", "Xwayland"), proc
                assert "cmdline" not in proc and "environ" not in proc, proc

            # 5. Mismatched expected_pid must be rejected cleanly.
            bad_end = rpc({
                "op": "end",
                "session": session_name,
                "expected_pid": observed["pid"] + 99999,
                "expected_started": observed["started"],
                **creds,
            })
            assert not bad_end["ok"], bad_end

            # 6. Exercise Waybridge.Cli status & end without connecting if dotnet is installed.
            dotnet = shutil.which("dotnet")
            if dotnet:
                subprocess.run(
                    [dotnet, "build", "gui/Waybridge.Cli/Waybridge.Cli.csproj", "-c", "Release", "--nologo", "-v", "q"],
                    cwd=ROOT,
                    check=True,
                )
                cli_dll = ROOT / "gui/Waybridge.Cli/bin/Release/net10.0/waybridge.dll"
                cli_state = root / "cli-state"
                cli_state.mkdir(mode=0o700)
                profile_id = str(uuid.uuid4())
                (cli_state / "profiles.json").write_text(json.dumps([{
                    "id": profile_id,
                    "name": "InspectHost",
                    "host": address,
                    "port": port,
                    "certificate_sha256": pin,
                    "client_id": creds["client_id"],
                    "session": session_name,
                    "session_mode": "auto",
                    "auto_reconnect": False,
                    "settings": {},
                }]))
                cli_env = {**os.environ, "WAYBRIDGE_CLIENT_TOKEN": creds["client_token"]}
                status_out = subprocess.check_output(
                    [dotnet, str(cli_dll), "status", "--connection", "InspectHost", "--state-dir", str(cli_state), "--json"],
                    env=cli_env,
                    text=True,
                    timeout=15,
                )
                cli_sessions = json.loads(status_out)
                assert len(cli_sessions) == 1 and cli_sessions[0]["name"] == session_name, cli_sessions
                assert any(w["app_id"] == "org.waybridge.InspectApp" for w in cli_sessions[0]["windows"]), cli_sessions
                subprocess.run(
                    [dotnet, str(cli_dll), "end", "--connection", "InspectHost", "--state-dir", str(cli_state)],
                    env=cli_env,
                    check=True,
                    timeout=15,
                )
            else:
                ended = rpc({
                    "op": "end",
                    "session": session_name,
                    "expected_pid": observed["pid"],
                    "expected_started": observed["started"],
                    **creds,
                })
                assert ended["ok"], ended

            # 7. Session disappears cleanly after ending.
            wait_until(
                lambda: rpc({"op": "list", "details": True, **creds})["result"] == [],
                15.0,
                "session did not stop after end",
            )
            print("PASS: out-of-band remote session status, windows, background apps, processes, and end without connecting")
        finally:
            for child in children:
                if child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        child.kill()
            if daemon.poll() is None:
                daemon.terminate()
                try:
                    daemon.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    daemon.kill()


if __name__ == "__main__":
    main()
