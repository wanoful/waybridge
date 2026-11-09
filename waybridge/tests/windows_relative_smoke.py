#!/usr/bin/env python3
"""Full PE viewer locked-pointer lifecycle, restricted to a private Wine/Xvfb desktop."""
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time
from smoke import ROOT
from relative_smoke import wait_for


def main():
    subprocess.run(["bash", "tests/build-relative-fixture.sh"], cwd=ROOT, check=True)
    subprocess.run(["x86_64-w64-mingw32-g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-static", "-municode", "tests/windows_relative_probe.cpp", "-o", "build/tests/windows-relative-probe.exe"], cwd=ROOT, check=True)
    processes = []
    prefix = ROOT / "build/wine-relative"
    prefix.mkdir(parents=True, mode=0o700, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="waybridge-windows-relative-") as runtime, open(ROOT / "build/tests/windows-relative.log", "w+") as log, open(ROOT / "build/tests/windows-relative-app.log", "w+") as app_log:
        read_fd, write_fd = os.pipe()
        xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1280x1024x24", "-nolisten", "tcp"], pass_fds=[write_fd], stdout=log, stderr=log)
        processes.append(xvfb)
        os.close(write_fd)
        with os.fdopen(read_fd) as pipe:
            display = ":" + pipe.readline().strip()
        wine_env = {**os.environ, "DISPLAY": display, "WINEPREFIX": str(prefix), "WINEDEBUG": "-all", "WINEDLLOVERRIDES": "winemenubuilder.exe=d;mscoree,mshtml=", "WAYBRIDGE_GDI": "1"}
        env = {**os.environ, "XDG_RUNTIME_DIR": runtime, "WAYLAND_DISPLAY": "windows-relative-0"}
        with socket.socket() as free:
            free.bind(("127.0.0.1", 0))
            port = free.getsockname()[1]
        token = "windows-relative-" + "a" * 40
        token_path = Path(runtime) / "token"
        token_path.write_text(token)
        token_path.chmod(0o600)

        def probe(action, check=True):
            return subprocess.run(["wine", str(ROOT / "build/tests/windows-relative-probe.exe"), action], env=wine_env, text=True, capture_output=True, check=check, timeout=15)

        def app_text():
            return Path(app_log.name).read_text()

        def locked_count():
            text = app_text()
            return text.count("LOCKED\n") - text.count("UNLOCKED\n")

        def state():
            output = probe("state").stdout
            values = re.search(r"CLIP (-?\d+) (-?\d+) (-?\d+) (-?\d+) SHOWING (\d)", output)
            assert values, output
            x, y, right, bottom, shown = map(int, values.groups())
            return right - x, bottom - y, shown

        def start_app():
            process = subprocess.Popen([str(ROOT / "build/tests/relative-app")], env=env, stdin=subprocess.PIPE, text=True, stdout=app_log, stderr=app_log)
            processes.append(process)
            return process

        def focus_lock():
            previous = locked_count()
            probe("focus")
            wait_for(lambda: locked_count() > previous)
            wait_for(lambda: state() == (1, 1, 0))

        try:
            subprocess.run(["wineboot", "-u"], env=wine_env, stdout=log, stderr=log, check=True, timeout=60)
            host = subprocess.Popen([str(ROOT / "target/debug/waybridge"), "--codec", "png", "--socket", "windows-relative-0", "--listen", f"127.0.0.1:{port}", "--token-file", str(token_path)], env=env, stdout=log, stderr=log)
            processes.append(host)
            wait_for(lambda: (Path(runtime) / "windows-relative-0").exists())
            app = start_app()
            client = subprocess.Popen(["wine", str(ROOT / "build/windows/waybridge-client.exe"), "127.0.0.1", str(port), token], env=wine_env, stdout=log, stderr=log)
            processes.append(client)
            wait_for(lambda: "CONFIGURE 2" in app_text() and "GDI fallback" in Path(log.name).read_text(), seconds=20)
            time.sleep(.3)
            focus_lock()
            motion_before = app_text().count("MOTION ")
            probe("motion")
            wait_for(lambda: "REL " in app_text())
            assert app_text().count("MOTION ") == motion_before, app_text()
            probe("emergency")
            wait_for(lambda: app_text().count("UNLOCKED\n") >= 1)
            wait_for(lambda: state() == (1280, 1024, 1))
            # Explicit focus loss is also the required gesture to rearm a persistent lock.
            probe("blur")
            focus_lock()
            probe("blur")
            wait_for(lambda: app_text().count("UNLOCKED\n") >= 2)
            wait_for(lambda: state() == (1280, 1024, 1))
            focus_lock()
            probe("close")
            wait_for(lambda: probe("present", check=False).returncode != 0)
            wait_for(lambda: state() == (1280, 1024, 1))
            app = start_app()
            wait_for(lambda: probe("present", check=False).returncode == 0)
            focus_lock()
            app.stdin.write("destroy_surface\n")
            app.stdin.flush()
            wait_for(lambda: probe("present", check=False).returncode != 0)
            wait_for(lambda: state() == (1280, 1024, 1))
            app = start_app()
            wait_for(lambda: probe("present", check=False).returncode == 0)
            focus_lock()
            host.terminate()
            host.wait(timeout=5)
            wait_for(lambda: client.poll() is not None)
            # The viewer must restore the desktop even when the compositor disappears.
            wait_for(lambda: state() == (1280, 1024, 1))
            print("PASS: full PE viewer in private Wine/Xvfb: native handshake, hidden one-pixel clip, synthetic Raw Input reaches real Wayland relative-pointer without absolute motion, emergency/focus/close/server-destruction/disconnect cleanup. Physical Windows mouse and ARM64 runtime remain unverified.")
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)


if __name__ == "__main__":
    main()
