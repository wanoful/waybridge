#!/usr/bin/env python3
"""Regression for real CSD geometry, decoration ownership and title-bar actions."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

from smoke import ROOT, Viewer


def main():
    subprocess.run(["bash", "tests/build-fixture.sh"], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="waybridge-decoration-") as runtime:
        env = {**os.environ, "XDG_RUNTIME_DIR": runtime, "WAYLAND_DISPLAY": "decoration-0"}
        with socket.socket() as free:
            free.bind(("127.0.0.1", 0))
            port = free.getsockname()[1]
        token = "decoration-test-" + "5" * 40
        token_path = Path(runtime) / "token"
        token_path.write_text(token)
        token_path.chmod(0o600)
        processes = []
        with open(ROOT / "build/tests/decoration-host.log", "w") as log, open(ROOT / "build/tests/decoration-app.log", "w") as app_log:
            try:
                host = subprocess.Popen([str(ROOT / "target/debug/waybridge"), "--socket", "decoration-0", "--listen", f"127.0.0.1:{port}", "--token-file", str(token_path)], env=env, stdout=log, stderr=log)
                processes.append(host)
                deadline = time.monotonic() + 10
                while not (Path(runtime) / "decoration-0").exists():
                    assert host.poll() is None and time.monotonic() < deadline
                    time.sleep(0.05)
                viewer = Viewer(port, token)
                app = subprocess.Popen([str(ROOT / "build/tests/test-app")], env={**env,"WAYBRIDGE_FIXTURE_CSD":"1"}, stdout=app_log, stderr=app_log)
                processes.append(app)
                info = viewer.until(lambda k,v:k == "control" and v["type"] == "window")
                assert info["decoration"] == "client", info
                id = info["id"]
                frame = viewer.until(lambda k,v:k == "frame" and v[0] == id)
                assert frame[1:3] == (960,640), "invisible 12px CSD shadows must not be streamed"
                image = frame[-1]
                assert all(abs(c-40)<5 for c in image.getpixel((100,16))[:3])
                assert image.getpixel((0,80))[0] > 220
                assert image.getpixel((959,639))[2] > 220
                image.save(ROOT / "build/tests/csd-window.png")
                viewer.send(type="resize",id=id,width=640,height=480)
                viewer.until(lambda k,v:k=="frame" and v[0:3]==(id,640,480))
                viewer.send(type="pointer",id=id,x=120,y=80)
                viewer.send(type="pointer",id=id,x=120,y=16)
                viewer.send(type="button",id=id,button=272,down=True)
                viewer.until(lambda k,v:k=="control" and v["type"]=="window_action" and v["action"]=="move")
                viewer.send(type="cancel_input",id=id)
                for x,expected in [(540,"minimize"),(580,"maximize"),(580,"restore")]:
                    viewer.send(type="pointer",id=id,x=x,y=16)
                    viewer.send(type="button",id=id,button=272,down=True)
                    viewer.send(type="button",id=id,button=272,down=False)
                    viewer.until(lambda k,v:k=="control" and v["type"]=="window_action" and v["action"]==expected)
                contents=Path(app_log.name).read_text()
                assert "ENTER 132.0 92.0" in contents or "MOTION 132.0 92.0" in contents, contents
                viewer.send(type="pointer",id=id,x=620,y=16)
                viewer.send(type="button",id=id,button=272,down=True)
                viewer.send(type="button",id=id,button=272,down=False)
                viewer.until(lambda k,v:k=="control" and v["type"]=="window_closed" and v["id"]==id)
                # A negotiated client must instead receive a native Windows frame.
                plain_env = env.copy()
                plain_env.pop("WAYBRIDGE_FIXTURE_CSD",None)
                app = subprocess.Popen([str(ROOT / "build/tests/test-app")], env=plain_env, stdout=app_log, stderr=app_log)
                processes.append(app)
                viewer.until(lambda k,v:k=="control" and v["type"]=="window" and v["decoration"]=="server")
                viewer.sock.close()
                print("PASS: CSD header retained once; shadows cropped; input origin and resize match cropped geometry; move/minimize/maximize/restore/close forwarded; xdg-decoration negotiates native frame")
            finally:
                for p in reversed(processes):
                    if p.poll() is None:
                        p.terminate()
                        try:p.wait(timeout=3)
                        except subprocess.TimeoutExpired:p.kill();p.wait()


if __name__ == "__main__":
    main()
