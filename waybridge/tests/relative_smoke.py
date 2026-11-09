#!/usr/bin/env python3
"""Real isolated Wayland clients: locked-pointer handshake, motion and cleanup."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from smoke import Viewer, ROOT


class RelativeViewer(Viewer):
    def __init__(self, port, token, capability=True):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=8)
        self.geometry = {}
        self.send(type="hello", version=3, token=token, pointer_lock=capability)
        assert self.receive()[1]["type"] == "hello"

    def lock(self, active=True):
        return self.until(lambda k, v: k == "control" and v["type"] == "pointer_lock" and v["active"] == active)

    def ack(self, packet, active=True, **changes):
        self.send(**{**packet, "type": "pointer_lock_ack", "active": active, **changes})

    def barrier(self, no_lock=False):
        nonce = time.monotonic_ns()
        self.send(type="ping", nonce=nonce)
        while True:
            kind, value = self.receive()
            if no_lock:
                assert not (kind == "control" and value["type"] == "pointer_lock" and value["active"]), value
            if kind == "control" and value["type"] == "pong" and value["nonce"] == nonce:
                break
        time.sleep(.08)


def wait_for(check, seconds=8):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if check():
            return
        time.sleep(.025)
    raise AssertionError("timed out waiting for real fixture event")


def main():
    subprocess.run(["bash", "tests/build-relative-fixture.sh"], cwd=ROOT, check=True)
    processes = []
    with tempfile.TemporaryDirectory(prefix="waybridge-relative-") as runtime:
        env = {**os.environ, "XDG_RUNTIME_DIR": runtime, "WAYLAND_DISPLAY": "relative-0"}
        with socket.socket() as free:
            free.bind(("127.0.0.1", 0))
            port = free.getsockname()[1]
        token = "relative-test-" + "9" * 40
        token_file = Path(runtime) / "token"
        token_file.write_text(token)
        token_file.chmod(0o600)
        with open(ROOT / "build/tests/relative-host.log", "w+") as host_log, open(ROOT / "build/tests/relative-app.log", "w+") as app_log:
            def log():
                return Path(app_log.name).read_text()

            def command(text):
                count = log().count("COMMAND\n")
                app.stdin.write(text + "\n")
                app.stdin.flush()
                wait_for(lambda: log().count("COMMAND\n") > count)

            def focus_pointer():
                viewer.send(type="focus", id=first, focused=False)
                viewer.send(type="focus", id=first, focused=True)
                viewer.send(type="pointer", id=first, x=120., y=80.)

            def activate():
                packet = viewer.lock()
                count = log().count("LOCKED\n") - log().count("UNLOCKED\n")
                viewer.ack(packet)
                wait_for(lambda: log().count("LOCKED\n") - log().count("UNLOCKED\n") > count)
                return packet

            try:
                host = subprocess.Popen([str(ROOT / "target/debug/waybridge"), "--codec", "png", "--socket", "relative-0", "--listen", f"127.0.0.1:{port}", "--token-file", str(token_file)], env=env, stdout=host_log, stderr=host_log)
                processes.append(host)
                wait_for(lambda: (Path(runtime) / "relative-0").exists())
                viewer = RelativeViewer(port, token, capability=False)
                app = subprocess.Popen([str(ROOT / "build/tests/relative-app")], stdin=subprocess.PIPE, text=True, env=env, stdout=app_log, stderr=app_log)
                processes.append(app)
                ids = {}
                while len(ids) < 2:
                    kind, value = viewer.receive()
                    if kind == "control" and value["type"] == "window":
                        ids[value["title"]] = value["id"]
                first, other = ids["Relative fixture lock"], ids["Relative fixture other"]
                viewer.until(lambda k, v: k == "frame" and v[0] == first)
                focus_pointer()
                viewer.barrier(no_lock=True)
                assert "LOCKED\n" not in log()
                # An older viewer gets ordinary pointer input but never a lock request.
                viewer.send(type="hello", pointer_lock=True)
                viewer.send(type="pointer", id=first, x=120., y=80.)
                packet = viewer.lock()
                viewer.ack(packet, id=other)
                viewer.ack(packet, generation=packet["generation"] + 99)
                viewer.barrier()
                assert "LOCKED\n" not in log(), log()
                viewer.ack(packet)
                wait_for(lambda: "LOCKED\n" in log())
                before = log().count("MOTION ")
                viewer.send(type="pointer", id=first, x=130., y=90.)
                viewer.send(type="relative_pointer", id=first, generation=packet["generation"], dx=12.5, dy=-7.25)
                wait_for(lambda: "12.500 -7.250 12.500 -7.250" in log())
                for changes in ({"id": other}, {"generation": packet["generation"] + 1}, {"dx": "NaN"}, {"dy": None}, {"dx": 1e20}):
                    viewer.send(**{"type": "relative_pointer", "id": first, "generation": packet["generation"], "dx": 1, "dy": 2, **changes})
                viewer.barrier()
                assert log().count("REL ") == 1, log()
                assert log().count("MOTION ") == before, log()
                viewer.send(type="focus", id=first, focused=False)
                assert viewer.lock(False)["generation"] == packet["generation"]
                wait_for(lambda: "UNLOCKED\n" in log())
                focus_pointer()
                second = activate()
                assert second["generation"] > packet["generation"]
                viewer.send(type="relative_pointer", id=first, generation=packet["generation"], dx=99, dy=99)
                viewer.send(type="key", id=first, key=30, down=True)
                viewer.send(type="button", id=first, button=272, down=True)
                viewer.ack(second, active=False)  # native failure, capture loss, or emergency release
                viewer.lock(False)
                viewer.barrier()
                assert log().count("REL ") == 1 and "KEY 30 0" in log() and "BUTTON 272 0" in log(), log()
                # Resource destruction/recreation creates a fresh generation.
                command("persistent")
                viewer.send(type="pointer", id=first, x=120., y=80.)
                third = activate()
                command("destroy")
                assert viewer.lock(False)["generation"] == third["generation"]
                command("persistent")
                viewer.send(type="pointer", id=first, x=120., y=80.)
                fourth = activate()
                command("region")
                assert viewer.lock(False)["generation"] == fourth["generation"]
                # The protocol manager exists but confine is intentionally never activated.
                command("confine")
                focus_pointer()
                viewer.barrier(no_lock=True)
                assert "CONFINED\n" not in log()
                command("oneshot")
                fifth = activate()
                viewer.ack(fifth, active=False)
                viewer.lock(False)
                focus_pointer()
                viewer.barrier(no_lock=True)
                locked_before = log().count("LOCKED\n")
                assert log().count("LOCKED\n") == locked_before
                command("persistent")
                timeout = viewer.lock()
                # Unacknowledged requests expire and cannot activate from a stale acknowledgement.
                assert viewer.lock(False)["generation"] == timeout["generation"]
                viewer.ack(timeout)
                viewer.barrier()
                assert log().count("LOCKED\n") == locked_before
                focus_pointer()
                activate()
                command("unmap")
                viewer.lock(False)
                command("remap")
                focus_pointer()
                active = activate()
                viewer.sock.close()
                wait_for(lambda: log().count("UNLOCKED\n") >= 6)
                time.sleep(.15)
                viewer = RelativeViewer(port, token)
                viewer.until(lambda k, v: k == "control" and v["type"] == "window" and v["id"] == first)
                focus_pointer()
                reconnected = activate()
                assert reconnected["generation"] > active["generation"]
                # Invalid JSON numeric infinity is rejected by the transport and releases input.
                import struct
                body = b'\x00{"type":"relative_pointer","dx":1e999,"dy":0}'
                viewer.sock.sendall(struct.pack(">I", len(body)) + body)
                wait_for(lambda: log().count("UNLOCKED\n") >= 7)
                viewer.sock.close()
                time.sleep(.15)
                viewer = RelativeViewer(port, token)
                viewer.until(lambda k, v: k == "control" and v["type"] == "window" and v["id"] == first)
                focus_pointer()
                final = activate()
                command("destroy_surface")
                assert viewer.lock(False)["generation"] == final["generation"]
                viewer.until(lambda k, v: k == "control" and v["type"] == "window_closed" and v["id"] == first)
                assert host.poll() is None and app.poll() is None, log()
                viewer.sock.close()
                print("PASS: real Wayland locked-pointer handshake, relative counts, target/generation/value rejection, no absolute motion, focus/native release, recreation, region, oneshot, timeout, unmap, reconnect, invalid JSON and surface destruction; confine remains inactive")
            finally:
                for process in reversed(processes):
                    if process.poll() is None:
                        process.terminate()
                        process.wait(timeout=5)


if __name__ == "__main__":
    main()
