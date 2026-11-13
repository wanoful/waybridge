#!/usr/bin/env python3
"""Real Chrome CSD and tab detach/merge on an isolated profile, via viewer input."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
from smoke import ROOT, Viewer


def wait(check, seconds=15):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        value = check()
        if value:
            return value
        time.sleep(.025)
    raise AssertionError("Chrome drag timeout; see build/tests/chrome-drag-*.log")


def main():
    processes = []
    with tempfile.TemporaryDirectory(prefix="waybridge-chrome-drag-") as runtime, \
            open(ROOT / "build/tests/chrome-drag-host.log", "w") as log, \
            open(ROOT / "build/tests/chrome-drag-app.log", "w") as app_log:
        env = {**os.environ, "XDG_RUNTIME_DIR": runtime, "WAYLAND_DEBUG": "1"}
        token = "chrome-drag-" + "a" * 40
        token_path = Path(runtime) / "token"
        token_path.write_text(token)
        token_path.chmod(0o600)
        with socket.socket() as free:
            free.bind(("127.0.0.1", 0))
            port = free.getsockname()[1]
        viewer = None
        try:
            host = subprocess.Popen([str(ROOT / "target/debug/waybridge"), "--socket", "chrome-drag-0", "--listen", f"127.0.0.1:{port}", "--token-file", str(token_path), "--no-audio", "--codec", "png"], env=env, stdout=log, stderr=log)
            processes.append(host)
            wait(lambda: (Path(runtime) / "chrome-drag-0.session.json").exists())
            viewer = Viewer(port, token)
            viewer.send(type="hello", toplevel_drag_v1=True, dpi=96)
            viewer.send(type="ping", nonce=789)
            viewer.until(lambda k, v: k == "control" and v.get("type") == "pong")
            windows, frames, events, errors = {}, {}, [], []

            def receive():
                try:
                    while True:
                        kind, value = viewer.receive()
                        if kind == "frame":
                            frames[value[0]] = value
                        else:
                            events.append(value)
                            if value["type"] == "window":
                                windows[value["id"]] = value
                            elif value["type"] == "window_closed":
                                windows.pop(value["id"], None)
                except Exception as error:
                    errors.append(repr(error))
            thread = threading.Thread(target=receive, daemon=True)
            thread.start()
            # Fixture-only throwaway key store: never initialize/read a host wallet.
            app = subprocess.Popen([str(ROOT / "target/debug/waybridge"), "--socket", "chrome-drag-0", "--launch", "--", "google-chrome-stable", f"--user-data-dir={runtime}/profile", "--password-store=basic", "--no-first-run", "--no-default-browser-check", "--disable-background-networking", "data:text/html,<title>Waybridge tab A</title><h1>Tab A</h1>", "data:text/html,<title>Waybridge tab B</title><h1>Tab B</h1>"], env=env, stdout=app_log, stderr=app_log)
            processes.append(app)
            first = wait(lambda: next((id for id, w in list(windows.items()) if w.get("decoration") == "client" and id in frames), None))
            frames[first][-1].save(ROOT / "build/tests/chrome-drag-initial.png")
            time.sleep(.5)

            def motion(id, x, y):
                viewer.send(type="pointer", id=id, x=x, y=y)
                time.sleep(.08)

            viewer.send(type="focus", id=first, focused=True)
            motion(first, 120, 18)
            viewer.send(type="button", id=first, button=272, down=True)
            for x, y in [(145, 18), (160, 45), (170, 100), (200, 170)]:
                motion(first, x, y)
            motion(0, 0, 0)
            second = wait(lambda: next((id for id in list(windows) if id != first and id in frames), None))
            wait(lambda: any(e.get("type") == "toplevel_drag" and e.get("id") == second for e in events))
            viewer.send(type="button", id=0, button=272, down=False)
            wait(lambda: any(e.get("type") == "toplevel_drag" and e.get("active") is False for e in events))
            time.sleep(.4)
            assert len(windows) == 2, (windows, errors)
            frames[second][-1].save(ROOT / "build/tests/chrome-drag-detached.png")
            print("PASS: Chrome requests client decoration, tab detaches and survives drop outside all windows", flush=True)
            events.clear()
            viewer.send(type="focus", id=second, focused=True)
            motion(second, 120, 18)
            viewer.send(type="button", id=second, button=272, down=True)
            motion(second, 150, 18)
            motion(0, 0, 0)
            wait(lambda: any(e.get("type") == "toplevel_drag" and e.get("active") for e in events))
            for x in (140, 180, 220):
                motion(first, x, 18)
            wait(lambda: second not in windows)
            viewer.send(type="button", id=first, button=272, down=False)
            wait(lambda: any(e.get("type") == "toplevel_drag" and e.get("active") is False for e in events))
            assert len(windows) == 1, windows
            assert "xdg_toplevel_drag_v1" in Path(app_log.name).read_text()
            frames[first][-1].save(ROOT / "build/tests/chrome-drag-merged.png")
            print("PASS: Chrome tab reattaches to another window and the empty detached window closes")
        finally:
            if viewer:
                viewer.sock.close()
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=6)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


if __name__ == "__main__":
    main()
