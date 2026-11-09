#!/usr/bin/env python3
"""Real PE + SendInput routing against a recording peer, isolated Wine/Xvfb.

Composition boundaries are explicit IMM messages, not a Microsoft IME test.
The existing IME/Qt tests independently cover actual text-input-v3 delivery.
"""
import io
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import threading
import time
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


def wait_for(check):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if check(): return
        time.sleep(.05)
    raise AssertionError("IME key probe timed out")


def main():
    (ROOT / "build/tests").mkdir(parents=True, exist_ok=True)
    probe = ROOT / "build/tests/windows-ime-keys-probe.exe"
    subprocess.run(["x86_64-w64-mingw32-g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-municode", "-static", "-O2", "tests/windows_ime_keys_probe.cpp", "-o", str(probe)], cwd=ROOT, check=True)
    prefix = ROOT / "build/wine-ime-keys"
    prefix.mkdir(parents=True, mode=0o700, exist_ok=True)
    processes = []
    with tempfile.TemporaryDirectory(prefix="waybridge-ime-keys-") as directory, open(ROOT / "build/tests/ime-keys.log", "w+") as log:
        read, write = os.pipe()
        xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write), "-screen", "0", "1280x1024x24", "-nolisten", "tcp"], pass_fds=[write], stdout=log, stderr=log)
        processes.append(xvfb); os.close(write)
        with os.fdopen(read) as pipe: display = ":" + pipe.readline().strip()
        env = {**os.environ, "DISPLAY": display, "WINEPREFIX": str(prefix), "WINEDEBUG": "-all", "WINEDLLOVERRIDES": "winemenubuilder.exe=d;mscoree,mshtml=", "WAYBRIDGE_GDI": "1", "WAYBRIDGE_DPI": "96"}
        peer = None
        try:
            subprocess.run(["wineboot", "-u"], env=env, stdout=log, stderr=log, timeout=60, check=True)
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0)); listener.listen(); listener.settimeout(15)
                client = subprocess.Popen(["wine", str(ROOT / "build/windows/waybridge-client.exe"), "127.0.0.1", str(listener.getsockname()[1]), "ime-key-probe"], env=env, stdout=log, stderr=log)
                processes.append(client)
                peer, _ = listener.accept()
            messages = []
            lock = threading.Lock()

            def exact(n):
                data = bytearray()
                while len(data) < n:
                    part = peer.recv(n - len(data))
                    if not part: raise EOFError
                    data.extend(part)
                return data

            def receive():
                try:
                    while True:
                        length, = struct.unpack(">I", exact(4))
                        assert 0 < length < 1024 * 1024
                        packet = exact(length)
                        assert packet[0] == 0
                        with lock: messages.append(json.loads(packet[1:]))
                except (EOFError, OSError): pass

            receiver = threading.Thread(target=receive, daemon=True)
            receiver.start()

            def send(**value):
                packet = b"\0" + json.dumps(value).encode()
                peer.sendall(struct.pack(">I", len(packet)) + packet)

            def native(action, check=True):
                return subprocess.run(["wine", str(probe), action], env=env, capture_output=True, text=True, timeout=15, check=check)

            def snapshot():
                with lock: return list(messages)

            send(type="hello", version=3, codec="mixed", split_media=False)
            send(type="window", id=1, width=320, height=200, title="IME key routing probe")
            image = io.BytesIO(); Image.new("RGBA", (320, 200), (255, 0, 0, 255)).save(image, format="PNG")
            packet = struct.pack(">BIIIQiiii", 1, 1, 320, 200, 1, 320, 200, 0, 0) + image.getvalue()
            peer.sendall(struct.pack(">I", len(packet)) + packet)
            wait_for(lambda: any(m["type"] == "focus" and m.get("focused") for m in snapshot()))
            send(type="ime_state", enabled=True, id=1, generation=42, rectangle=[10, 10, 1, 20], purpose=0)
            wait_for(lambda: native("ready", check=False).returncode == 0)

            def run(action):
                start = len(snapshot())
                native(action)
                time.sleep(.15)
                result = [m for m in snapshot()[start:] if m["type"] in ("key", "ime")]
                print(action, json.dumps(result, ensure_ascii=False), flush=True)
                return result

            def keys(result):
                return [(m["key"], m["down"]) for m in result if m["type"] == "key"]

            def text(result):
                return "".join(m.get("commit", "") for m in result if m["type"] == "ime")

            space = run("shortcut")
            assert keys(space) == [(29, True), (57, True), (57, False), (29, False)], space
            assert not text(space), space
            late = run("late-character")
            assert keys(late) == keys(space) and not text(late), late
            select = run("select")
            assert keys(select) == [(29, True), (30, True), (30, False), (29, False)] and not text(select), select
            alt = run("right-alt-shortcut")
            assert keys(alt) == [(29, True), (100, True), (16, True), (16, False), (100, False), (29, False)] and not text(alt), alt
            for action, modifier, key in (("composition", 29, 46), ("cancel", 29, 46), ("shift", 42, 105)):
                result = run(action)
                assert keys(result) == [(modifier, True), (modifier, False), (modifier, True), (key, True), (key, False), (modifier, False)], result
                assert not text(result), result
            released = run("released")
            assert keys(released) == [(29, True), (29, False)] and text(released) == "c", released
            unicode = run("unicode")
            assert not keys(unicode) and text(unicode) == "你🙂", unicode
            typed = run("type")
            assert not keys(typed) and text(typed) == "a", typed
            started = run("generation-start")
            assert keys(started) == [(29, True), (29, False)], started
            start = len(snapshot())
            send(type="ime_state", enabled=True, id=1, generation=43, rectangle=[30, 10, 1, 20], purpose=0)
            wait_for(lambda: native("ready-new", check=False).returncode == 0)
            native("old-end")
            time.sleep(.15)
            changed = snapshot()[start:]
            assert not keys(changed), changed
            copied = run("copy-release")
            assert keys(copied) == [(29, True), (46, True), (46, False), (29, False)], copied
            start = len(snapshot())
            lost = run("focus-lost-end")
            assert keys(lost) == [(29, True), (29, False)], lost
            assert any(m["type"] == "focus" and not m["focused"] for m in snapshot()[start:])
            assert client.poll() is None
            print("PASS: actual SendInput Ctrl+Space/Ctrl+A route once; held Ctrl/Shift restored after controlled composition end/cancel; released modifiers stay released; generation cancellation/old END restore no stale keys; Unicode/emoji and English commit once (Wine; no Microsoft IME engine)")
        except BaseException:
            print(Path(log.name).read_text())
            raise
        finally:
            if peer:
                try: peer.shutdown(socket.SHUT_RDWR)
                except OSError: pass
                peer.close()
            subprocess.run(["wineserver", "-k"], env=env, stdout=log, stderr=log, timeout=10)
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate(); process.wait(timeout=5)


if __name__ == "__main__": main()
