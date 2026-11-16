#!/usr/bin/env python3
"""Real PE worker's hosted mode: private pipe, settings, status, files, detach."""
import base64
import json
import os
from pathlib import Path
import queue
import signal
import subprocess
import tempfile
import threading
from gui_session_smoke import Rpc, wait
from smoke import ROOT


def main():
    subprocess.run(["bash", "tests/build-fixture.sh"], cwd=ROOT, check=True)
    subprocess.run(["x86_64-w64-mingw32-g++", "-municode", "-static", "-O2", "tests/windows_control_probe.cpp",
                    "-o", "build/tests/windows-control-probe.exe"], cwd=ROOT, check=True)
    processes = []
    with tempfile.TemporaryDirectory(prefix="waybridge-gui-native-") as directory, open(ROOT/"build/tests/gui-native.log", "w+") as log:
        root = Path(directory)
        read_fd, write_fd = os.pipe()
        xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1280x1024x24", "-nolisten", "tcp"], pass_fds=[write_fd], stdout=log, stderr=log)
        processes.append(xvfb); os.close(write_fd)
        with os.fdopen(read_fd) as pipe:
            display = ":" + pipe.readline().strip()
        prefix = ROOT/"build/wine-gui-native"
        prefix.mkdir(mode=0o700, exist_ok=True)
        wine_env = {**os.environ, "DISPLAY": display, "WINEPREFIX": str(prefix), "WINEDEBUG": "-all",
                    "WINEDLLOVERRIDES": "winemenubuilder.exe=d;mscoree,mshtml=", "WAYBRIDGE_GDI": "1"}
        rpc = Rpc({**os.environ, "XDG_RUNTIME_DIR": directory})
        server_pid = app_pid = None
        try:
            subprocess.run(["wineboot", "-u"], env=wine_env, stdout=log, stderr=log, check=True, timeout=60)
            session = rpc.ok(op="ensure", session="gui-native-0", binary=str(ROOT/"target/debug/waybridge"), port=0, codec="png", audio=False, xwayland=False)
            server_pid = session["pid"]
            app = rpc.ok(op="launch", session="gui-native-0", binary=str(ROOT/"target/debug/waybridge"), argv=[str(ROOT/"build/tests/test-app")])
            app_pid = app["pid"]
            native_log = ROOT/"build/tests/gui-native-worker.log"
            native = subprocess.Popen(["wine", str(ROOT/"build/tests/windows-control-probe.exe"),
                                       "Z:" + str(ROOT/"build/windows/waybridge-client.exe"), "Z:" + str(native_log)],
                                      env=wine_env, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True)
            processes.append(native)
            events = queue.Queue()
            def read():
                for line in native.stdout:
                    events.put(json.loads(line))
            threading.Thread(target=read, daemon=True).start()
            def send(**value):
                native.stdin.write(json.dumps(value) + "\n"); native.stdin.flush()
            def until(check):
                for _ in range(100):
                    event = events.get(timeout=20)
                    if check(event):
                        return event
                raise AssertionError("Expected native IPC event missing")
            settings = dict(codec="png", fps=60, bitrate=30000, audio=False, files=True,
                            badge_png=base64.b64encode((ROOT/"assets/branding/waybridge-badge.png").read_bytes()).decode())
            send(type="start", version=1, host="127.0.0.1", port=session["port"], token=session["token"], settings=settings)
            connected = until(lambda e: e["type"] == "connected")
            assert "token" not in connected and "media_key" not in connected
            status = until(lambda e: e["type"] == "status" and e["windows"])
            assert status["windows"][0]["codec"] == "PNG" and status["tcp_bytes"] > 0
            settings.update(fps=30, bitrate=12000, read_only=True, clipboard="off", dpi=144, badge_png="")
            send(type="settings", settings=settings)
            assert until(lambda e: e["type"] == "local_settings_applied")["settings"]["dpi"] == 144
            ack = until(lambda e: e["type"] == "stream_config" and e["fps"] == 30)
            assert ack["ok"] and ack["read_only"] and ack["bitrate"] == 12000
            settings.update(read_only=False, clipboard="both")
            send(type="settings", settings=settings)
            until(lambda e: e["type"] == "stream_config" and not e["read_only"])
            file = root/"你好.txt"; file.write_text("native named pipe file payload")
            send(type="send_files", paths=["Z:" + str(file)])
            until(lambda e: e["type"] == "transfer" and e["event"] == "upload_started")
            until(lambda e: e["type"] == "transfer" and e["event"] == "tree_upload_ready" and e["stage"] == "commit")
            send(type="disconnect"); native.stdin.close()
            native.wait(timeout=15)
            assert native.returncode == 0, native_log.read_text()
            os.kill(server_pid, 0)
            assert rpc.ok(op="ensure", session="gui-native-0", mode="attach")["pid"] == server_pid
            assert session["token"] not in native_log.read_text()
            rpc.ok(op="end", session="gui-native-0"); server_pid = None
            print("PASS: real Windows worker custom-badge startup/reset, named-pipe startup, sanitized capabilities/status, live FPS/bitrate/read-only/DPI, actual Unicode file upload, graceful disconnect and retained remote session")
        finally:
            rpc.close()
            for child in reversed(processes):
                if child.poll() is None:
                    child.terminate()
                    try: child.wait(timeout=5)
                    except subprocess.TimeoutExpired: child.kill(); child.wait()
            for pid in (app_pid, server_pid):
                if pid:
                    try: os.kill(pid, signal.SIGTERM)
                    except ProcessLookupError: pass


if __name__ == "__main__":
    main()
