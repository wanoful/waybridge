#!/usr/bin/env python3
"""Real Linux session RPC: lifecycle, literal argv, live settings and input policy."""
import json
import os
from pathlib import Path
import queue
import select
import signal
import subprocess
import tempfile
import threading
import time
from smoke import ROOT, Viewer


def wait(check, timeout=15):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = check()
        if value:
            return value
        time.sleep(.025)
    raise AssertionError("GUI session fixture timed out")


def stop_process(pid, timeout=10):
    """Wait for owned detached fixtures, including private portal/FUSE teardown."""
    try:
        descriptor = os.pidfd_open(pid)
    except ProcessLookupError:
        return
    try:
        try:
            signal.pidfd_send_signal(descriptor, signal.SIGTERM)
        except ProcessLookupError:
            return
        if not select.select([descriptor], [], [], timeout)[0]:
            signal.pidfd_send_signal(descriptor, signal.SIGKILL)
            assert select.select([descriptor], [], [], 3)[0], "fixture failed to stop"
    finally:
        os.close(descriptor)


class Rpc:
    def __init__(self, env):
        self.child = subprocess.Popen(["python", str(ROOT / "scripts/session-control.py")], env=env,
                                      stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.messages = queue.Queue()
        self.id = 0
        threading.Thread(target=self.read, daemon=True).start()

    def read(self):
        for line in self.child.stdout:
            self.messages.put(json.loads(line))

    def call(self, **request):
        self.id += 1
        self.child.stdin.write(json.dumps(dict(request, id=self.id)) + "\n")
        self.child.stdin.flush()
        response = self.messages.get(timeout=35)
        assert response["id"] == self.id
        return response

    def ok(self, **request):
        response = self.call(**request)
        assert response["ok"], response.get("error")
        return response["result"]

    def close(self):
        self.child.terminate()
        self.child.wait(timeout=5)


def main():
    subprocess.run(["bash", "tests/build-fixture.sh"], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="waybridge-gui-session-") as directory:
        runtime = Path(directory)
        env = {**os.environ, "XDG_RUNTIME_DIR": directory}
        rpc = Rpc(env)
        session = "gui-session-0"
        server_pid = app_pid = None
        viewer = None
        try:
            assert rpc.ok(op="hello")["version"] == 1
            for invalid in ("../other", "/tmp/other", "a;echo"):
                assert not rpc.call(op="ensure", session=invalid)["ok"]
            result = rpc.ok(op="ensure", session=session, binary=str(ROOT / "target/debug/waybridge"),
                            mode="auto", port=0, codec="png", fps=60, bitrate=30000, audio=False, xwayland=False)
            server_pid = result["pid"]
            assert result["created"]
            assert len(result["token"]) >= 16
            entries = rpc.ok(op="list")
            assert entries[0]["name"] == session and "token" not in entries[0]
            again = rpc.ok(op="ensure", session=session, mode="attach")
            assert not again["created"] and again["pid"] == server_pid
            descriptor = runtime / (session + ".session.json")
            assert descriptor.stat().st_mode & 0o077 == 0
            assert rpc.ok(op="applications"), "installed applications missing"

            marker = runtime / "argv.json"
            literal = "spaces ' quotes ; $(not-a-shell) 你好"
            rpc.ok(op="launch", session=session, binary=str(ROOT / "target/debug/waybridge"),
                   argv=["python", "-c", "import json,os,sys;json.dump([sys.argv[2],os.environ['WAYLAND_DISPLAY']],open(sys.argv[1],'w'))", str(marker), literal])
            wait(marker.exists)
            assert json.loads(marker.read_text()) == [literal, session]
            viewer = Viewer(result["port"], result["token"])
            launched = rpc.ok(op="launch", session=session, binary=str(ROOT / "target/debug/waybridge"),
                              argv=[str(ROOT / "build/tests/test-app")])
            app_pid = launched["pid"]
            first = viewer.until(lambda k, v: k == "frame")[0]
            log = runtime / "waybridge-control-center" / (session + ".log")
            viewer.send(type="stream_config", request_id=1, fps=90, bitrate=12000, codec="png", read_only=True)
            applied = viewer.until(lambda k, v: k == "control" and v.get("type") == "stream_config")
            assert applied["ok"] and applied["fps"] == 90 and applied["read_only"]
            before = log.read_text().count("BUTTON")
            for down in (True, False):
                viewer.send(type="button", id=first, x=80, y=80, button=272, down=down)
            viewer.send(type="ping", nonce=1)
            viewer.until(lambda k, v: k == "control" and v.get("type") == "pong")
            assert log.read_text().count("BUTTON") == before
            viewer.send(type="stream_config", request_id=2, fps=99999, bitrate=12000, codec="png")
            assert not viewer.until(lambda k, v: k == "control" and v.get("type") == "stream_config")["ok"]
            viewer.send(type="stream_config", request_id=3, fps=30, bitrate=10000, codec="png", read_only=False)
            assert viewer.until(lambda k, v: k == "control" and v.get("type") == "stream_config")["ok"]
            viewer.send(type="focus", id=first, focused=True)
            viewer.send(type="pointer", id=first, x=80, y=80)
            for down in (True, False):
                viewer.send(type="button", id=first, button=272, down=down)
            wait(lambda: log.read_text().count("BUTTON") > before)
            viewer.sock.close()
            viewer = None

            rpc.close()
            rpc = Rpc(env)
            retained = rpc.ok(op="ensure", session=session, mode="attach")
            assert retained["pid"] == server_pid, "closing the SSH control process killed the session"
            stale = runtime / "gui-stale.session.json"
            data = json.loads(descriptor.read_text())
            data["pid"] = os.getpid()
            data["started"] = "invalid"
            stale.write_text(json.dumps(data))
            stale.chmod(0o600)
            assert not rpc.call(op="end", session="gui-stale")["ok"]
            assert len(rpc.ok(op="list")) == 1
            rpc.ok(op="end", session=session)
            wait(lambda: not descriptor.exists())
            server_pid = None
            print("PASS: actual session create/attach/list, token privacy, installed app catalog, literal Unicode argv/environment, live FPS/bitrate ack, read-only enforcement, detach persistence, stale-pid rejection and explicit cleanup")
        finally:
            if viewer:
                viewer.sock.close()
            rpc.close()
            for pid in (app_pid, server_pid):
                if pid:
                    try:
                        os.kill(pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass


if __name__ == "__main__":
    main()
