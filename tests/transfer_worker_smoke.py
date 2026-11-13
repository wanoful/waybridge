#!/usr/bin/env python3
"""Stall real receive-side disk I/O; input, detach and reconnect must progress."""
import base64
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from urllib.parse import unquote, urlparse
from smoke import ROOT, Viewer
from gui_session_smoke import wait


def main():
    subprocess.run(["bash", "tests/build-fixture.sh"], cwd=ROOT, check=True)
    shim = ROOT / "build/tests/slow-transfer-io.so"
    subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror", "-o", str(shim),
                    str(ROOT / "tests/slow_transfer_io.c"), "-ldl"], check=True)
    with tempfile.TemporaryDirectory(prefix="waybridge-transfer-worker-") as directory:
        runtime = Path(directory)
        entered, release = runtime / "entered", runtime / "release"
        env = {**os.environ, "XDG_RUNTIME_DIR": directory, "WAYLAND_DISPLAY": "transfer-worker-0",
               "XDG_CONFIG_HOME": str(runtime / "config"), "XDG_DATA_HOME": str(runtime / "data"),
               "XDG_STATE_HOME": str(runtime / "state"), "LD_PRELOAD": str(shim),
               "WAYBRIDGE_SLOW_TRANSFER_ROOT": str(runtime / "waybridge-inbox"),
               "WAYBRIDGE_SLOW_TRANSFER_ENTERED": str(entered), "WAYBRIDGE_SLOW_TRANSFER_RELEASE": str(release)}
        token = runtime / "token"
        token.write_text("transfer-worker-" + "7" * 40)
        token.chmod(0o600)
        with socket.socket() as free:
            free.bind(("127.0.0.1", 0))
            port = free.getsockname()[1]
        with (runtime / "host.log").open("w+") as log:
            host = subprocess.Popen([str(ROOT / "target/debug/waybridge"), "--listen", f"127.0.0.1:{port}",
                                     "--socket", "transfer-worker-0", "--codec", "png", "--no-audio", "--no-xwayland",
                                     "--token-file", str(token)], env=env, stdout=log, stderr=log)
            viewer = app = None
            try:
                wait(lambda: (runtime / "transfer-worker-0").exists())
                viewer = Viewer(port, token.read_text())
                app = subprocess.Popen([str(ROOT / "build/tests/test-app")], env=env, stdout=log, stderr=log)
                window = viewer.until(lambda k, v: k == "frame")[0]
                viewer.send(type="focus", id=window, focused=True)
                viewer.send(type="upload_begin", transfer=701, name="slow.bin", size=4)
                viewer.send(type="upload_chunk", transfer=701, data=base64.b64encode(b"data").decode())
                wait(entered.exists)
                started = time.monotonic()
                viewer.send(type="key", id=window, key=30, down=True)
                viewer.send(type="key", id=window, key=30, down=False)
                viewer.send(type="ping", nonce=701)
                viewer.until(lambda k, v: k == "control" and v.get("type") == "pong" and v.get("nonce") == 701)
                assert time.monotonic() - started < 1.5, "disk I/O blocked compositor dispatch"
                wait(lambda: "KEY 30 0" in (runtime / "host.log").read_text())
                viewer.send(type="upload_end", transfer=701)
                viewer.send(type="upload_commit", transfers=[701])
                viewer.sock.close()
                viewer = None
                # Still stalled: reconnect cannot wait for partial-file deletion.
                time.sleep(.1)
                viewer = Viewer(port, token.read_text())
                viewer.until(lambda k, v: k == "frame")
                release.touch()
                wait(lambda: not (runtime / "waybridge-inbox/701").exists())
                viewer.send(type="upload_begin", transfer=701, name="complete.bin", size=4)
                viewer.send(type="upload_chunk", transfer=701, data=base64.b64encode(b"done").decode())
                viewer.send(type="upload_end", transfer=701)
                viewer.send(type="upload_commit", transfers=[701])
                viewer.until(lambda k, v: k == "control" and v.get("type") == "upload_ready")
                pasted = subprocess.check_output(["wl-paste", "--no-newline", "--type", "text/uri-list"], env=env).decode().strip()
                received = Path(unquote(urlparse(pasted).path))
                assert received.read_bytes() == b"done"
                viewer.sock.close()
                viewer = None
                time.sleep(.1)
                assert received.read_bytes() == b"done", "committed files were removed on disconnect"
                print("PASS: stalled actual disk write leaves input/ping/reconnect responsive; old queued commits canceled; completed files retained")
            finally:
                release.touch()
                if viewer:
                    viewer.sock.close()
                for child in (app, host):
                    if child and child.poll() is None:
                        child.terminate()
                        child.wait(timeout=10)
                (ROOT / "build/tests/transfer-worker-host.log").write_text((runtime / "host.log").read_text())


if __name__ == "__main__":
    main()
