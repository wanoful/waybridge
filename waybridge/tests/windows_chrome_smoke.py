#!/usr/bin/env python3
"""Real PE viewer and Chrome: one titlebar, native cross-HWND tab drag and Escape."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
from chrome_drag_smoke import wait
from smoke import ROOT


def main():
    subprocess.run(["x86_64-w64-mingw32-g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-static", "-municode", "tests/windows_chrome_probe.cpp", "-o", "build/tests/windows-chrome-probe.exe"], cwd=ROOT, check=True)
    processes = []
    prefix = ROOT / "build/wine-chrome"
    prefix.mkdir(parents=True, mode=0o700, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="waybridge-windows-chrome-") as runtime, \
            open(ROOT / "build/tests/windows-chrome-host.log", "w") as log, \
            open(ROOT / "build/tests/windows-chrome-app.log", "w") as app_log:
        read_fd, write_fd = os.pipe()
        xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1600x1100x24", "-nolisten", "tcp"], pass_fds=[write_fd], stdout=log, stderr=log)
        os.close(write_fd)
        with os.fdopen(read_fd) as pipe:
            display = ":" + pipe.readline().strip()
        wine_env = {**os.environ, "DISPLAY": display, "WINEPREFIX": str(prefix), "WINEDEBUG": "-all", "WINEDLLOVERRIDES": "winemenubuilder.exe=d;mscoree,mshtml=", "WAYBRIDGE_GDI": "1"}
        env = {**os.environ, "XDG_RUNTIME_DIR": runtime, "WAYLAND_DEBUG": "1"}
        token = "windows-chrome-" + "a" * 40
        token_path = Path(runtime) / "token"
        token_path.write_text(token)
        token_path.chmod(0o600)
        with socket.socket() as free:
            free.bind(("127.0.0.1", 0))
            port = free.getsockname()[1]
        def probe(action, check=True):
            return subprocess.run(["wine", str(ROOT / "build/tests/windows-chrome-probe.exe"), action], env=wine_env, text=True, capture_output=True, check=check, timeout=25)
        try:
            subprocess.run(["wineboot", "-u"], env=wine_env, stdout=log, stderr=log, check=True, timeout=60)
            host = subprocess.Popen([str(ROOT / "target/debug/waybridge"), "--socket", "windows-chrome-0", "--listen", f"127.0.0.1:{port}", "--token-file", str(token_path), "--no-audio", "--codec", "png"], env=env, stdout=log, stderr=log)
            processes.append(host)
            wait(lambda: (Path(runtime) / "windows-chrome-0.session.json").exists())
            client = subprocess.Popen(["wine", str(ROOT / "build/windows/waybridge-client.exe"), "127.0.0.1", str(port), token], env=wine_env, stdout=log, stderr=log)
            processes.append(client)
            app = subprocess.Popen([str(ROOT / "target/debug/waybridge"), "--socket", "windows-chrome-0", "--launch", "--", "google-chrome-stable", f"--user-data-dir={runtime}/profile", "--password-store=basic", "--no-first-run", "--no-default-browser-check", "--disable-background-networking", "data:text/html,<title>Waybridge tab A</title><h1>Tab A</h1>", "data:text/html,<title>Waybridge tab B</title><h1>Tab B</h1>"], env=env, stdout=app_log, stderr=app_log)
            processes.append(app)
            wait(lambda: probe("ready", False).returncode == 0, seconds=25)
            for action in ("split", "merge", "cancel"):
                result = probe(action, False)
                assert result.returncode == 0, (action, result.returncode, result.stdout, result.stderr)
                print(result.stdout.strip(), flush=True)
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            subprocess.run(["wineserver", "-k"], env=wine_env, stdout=log, stderr=log)
            xvfb.terminate()
            xvfb.wait(timeout=5)


if __name__ == "__main__":
    main()
