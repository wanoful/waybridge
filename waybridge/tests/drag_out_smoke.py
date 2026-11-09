#!/usr/bin/env python3
"""Real Wayland source/sink lifecycle and lazy bounded range reads; no desktop."""
import base64
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from smoke import Viewer, ROOT


def wait_for(check, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.02)
    raise AssertionError("timeout")


def main():
    subprocess.run(["bash", "tests/build-fixture.sh"], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="waybridge-drag-") as directory:
        root = Path(directory)
        payload = os.urandom(180_000)
        files = [root / "图片 data.bin", root / "empty.txt"]
        files[0].write_bytes(payload)
        files[1].write_bytes(b"")
        uris = root / "uris"
        uris.write_text("".join(p.as_uri() + "\r\n" for p in files))
        token = root / "token"
        token.write_text("drag-smoke-token-" + "3" * 40)
        token.chmod(0o600)
        env = {**os.environ, "XDG_RUNTIME_DIR": directory, "WAYLAND_DISPLAY": "drag-0", "WAYBRIDGE_TEST_DRAG_URIS": str(uris)}
        with socket.socket() as free:
            free.bind(("127.0.0.1", 0))
            port = free.getsockname()[1]
        with open(root / "host.log", "w+") as host_log, open(root / "app.log", "w+") as app_log:
            host = subprocess.Popen([str(ROOT / "target/debug/waybridge"), "--socket", "drag-0", "--listen", f"127.0.0.1:{port}", "--token-file", str(token), "--codec", "png"], env=env, stdout=host_log, stderr=host_log)
            app = None
            try:
                wait_for(lambda: (root / "drag-0").exists())
                viewer = Viewer(port, token.read_text())
                viewer.send(type="hello", file_drop_out=True)
                app = subprocess.Popen([str(ROOT / "build/tests/test-app")], env=env, stdout=app_log, stderr=app_log)
                window = viewer.until(lambda k, m: k == "control" and m["type"] == "window")["id"]
                viewer.until(lambda k, m: k == "frame" and m[0] == window)

                def begin():
                    viewer.send(type="pointer", id=window, x=50, y=70)
                    viewer.send(type="button", id=window, button=272, down=True)
                    return viewer.until(lambda k, m: k == "control" and m["type"] == "drag_offer")

                offer = begin()
                assert [f["size"] for f in offer["files"]] == [len(payload), 0]
                assert offer["files"][0]["name"] == files[0].name
                drag = offer["drag"]
                viewer.send(type="key", id=window, key=29, down=True)
                viewer.send(type="drag_claim", drag=drag)
                viewer.until(lambda k, m: k == "control" and m["type"] == "drag_ready")
                assert "SOURCE_CANCELLED" not in (root / "app.log").read_text()
                wait_for(lambda: "KEY 29 0" in (root / "app.log").read_text())
                viewer.send(type="focus", id=window, focused=False)
                viewer.send(type="drag_drop", drag=drag)
                wait_for(lambda: "SOURCE_DROPPED" in (root / "app.log").read_text())
                assert "SOURCE_FINISHED" not in (root / "app.log").read_text()
                for request, (offset, length) in enumerate([(65531, 65536), (0, 65536), (179990, 10), (0, 1)], 1):
                    viewer.send(type="drag_read", drag=drag, request=request, file=0, offset=offset, length=length)
                    data = viewer.until(lambda k, m: k == "control" and m["type"] == "drag_data")
                    assert data["request"] == request and base64.b64decode(data["data"]) == payload[offset:offset + length]
                viewer.send(type="drag_read", drag=drag, request=99, file=0, offset=0, length=65537)
                assert "error" in viewer.until(lambda k, m: k == "control" and m["type"] == "drag_data")
                viewer.send(type="drag_result", drag=drag, success=True)
                viewer.until(lambda k, m: k == "control" and m["type"] == "drag_end" and m["drag"] == drag)
                wait_for(lambda: "SOURCE_FINISHED" in (root / "app.log").read_text())
                assert "SOURCE_CANCELLED" not in (root / "app.log").read_text()

                # An early release must cancel, never create a late native drag.
                offer = begin()
                viewer.send(type="button", id=window, button=272, down=False)
                viewer.until(lambda k, m: k == "control" and m["type"] == "drag_end" and m["drag"] == offer["drag"])
                wait_for(lambda: "SOURCE_CANCELLED" in (root / "app.log").read_text())

                # A native rejection after handoff cancels exactly once.
                offer = begin()
                viewer.send(type="drag_claim", drag=offer["drag"])
                viewer.until(lambda k, m: k == "control" and m["type"] == "drag_ready")
                viewer.send(type="drag_result", drag=offer["drag"], success=False)
                wait_for(lambda: (root / "app.log").read_text().count("SOURCE_CANCELLED") == 2)
                assert (root / "app.log").read_text().count("SOURCE_FINISHED") == 1

                # Even an equal-length in-place rewrite must fail the read.
                offer = begin()
                viewer.send(type="drag_claim", drag=offer["drag"])
                viewer.until(lambda k, m: k == "control" and m["type"] == "drag_ready")
                files[0].write_bytes(b"changed" + payload[7:])
                viewer.send(type="drag_read", drag=offer["drag"], request=1, file=0, offset=0, length=64)
                assert "error" in viewer.until(lambda k, m: k == "control" and m["type"] == "drag_data")
                viewer.send(type="drag_result", drag=offer["drag"], success=False)
                wait_for(lambda: (root / "app.log").read_text().count("SOURCE_CANCELLED") == 3)
                files[0].write_bytes(payload)

                # Release during the claim/ready handshake; queued private
                # offers must not resurrect an already cancelled source.
                offer = begin()
                viewer.send(type="drag_claim", drag=offer["drag"])
                viewer.send(type="drag_result", drag=offer["drag"], success=False)
                wait_for(lambda: (root / "app.log").read_text().count("SOURCE_CANCELLED") == 4)
                assert (root / "app.log").read_text().count("SOURCE_FINISHED") == 1

                # Disconnect while the private target owns the grab.
                offer = begin()
                viewer.send(type="drag_claim", drag=offer["drag"])
                viewer.until(lambda k, m: k == "control" and m["type"] == "drag_ready")
                viewer.sock.close()
                wait_for(lambda: (root / "app.log").read_text().count("SOURCE_CANCELLED") == 5)
                assert files[0].read_bytes() == payload and files[1].exists()
                assert host.poll() is None and app.poll() is None
                # A new viewer can drag after disconnect; closing the source
                # process invalidates its pending operation and opened streams.
                viewer = Viewer(port, token.read_text())
                viewer.send(type="hello", file_drop_out=True)
                viewer.until(lambda k, m: k == "control" and m["type"] == "window")
                viewer.until(lambda k, m: k == "frame" and m[0] == window)
                offer = begin()
                viewer.send(type="drag_claim", drag=offer["drag"])
                viewer.until(lambda k, m: k == "control" and m["type"] == "drag_ready")
                app.terminate()
                app.wait(timeout=5)
                end = viewer.until(lambda k, m: k == "control" and m["type"] == "drag_end" and m["drag"] == offer["drag"])
                assert end["success"] is False and host.poll() is None
                viewer.sock.close()
                print("PASS: real Wayland outbound COPY negotiation; lazy random reads, bounds, empty/Unicode files, source mutation; finished after result; early release, rejection and disconnect cancel exactly once; reconnect/source close; source files retained")
            except BaseException:
                print((root / "host.log").read_text())
                print((root / "app.log").read_text())
                raise
            finally:
                for process in (app, host):
                    if process and process.poll() is None:
                        process.terminate()
                        process.wait(timeout=5)


if __name__ == "__main__":
    main()
