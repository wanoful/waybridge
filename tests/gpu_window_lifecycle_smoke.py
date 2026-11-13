#!/usr/bin/env python3
"""Real CUDA capture survives Wayland buffer detach/remap and other-window rendering."""
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

from codecs_smoke import wait_socket
from hardware_smoke import ROOT, Viewer, decoded


class LifecycleViewer(Viewer):
    def __init__(self, port, token):
        self.infos = {}
        self.visibility = {}
        self.frames = {}
        super().__init__(port, token)

    def send(self, **message):
        if message.get("type") == "hello":
            message["window_semantics_v1"] = True
        super().send(**message)

    def receive(self):
        kind, value = super().receive()
        if kind == 0 and value.get("type") == "window":
            self.infos[value["id"]] = value
        elif kind == 0 and value.get("type") == "window_visibility":
            self.visibility[value["id"]] = value
        elif kind == 2:
            frame = self.parse_video(value)
            chain = self.frames.setdefault(frame["id"], [])
            if frame["key"]:
                chain.clear()
            chain.append(frame)
        elif kind == 1:
            self.send(type="frame_ack", id=struct.unpack(">I", value[:4])[0],
                      sequence=struct.unpack(">Q", value[12:20])[0])
        return kind, value

    def wait(self, predicate):
        deadline = time.monotonic() + 15
        while not predicate():
            assert time.monotonic() < deadline, "GPU window lifecycle timed out"
            self.receive()


def main():
    subprocess.run(["bash", "tests/build-fixture.sh"], cwd=ROOT, check=True)
    log_path = ROOT / "build/tests/gpu-window-lifecycle.log"
    with tempfile.TemporaryDirectory(prefix="waybridge-gpu-lifecycle-") as runtime, log_path.open("w") as log:
        env = {**os.environ, "XDG_RUNTIME_DIR": runtime, "WAYLAND_DISPLAY": "gpu-lifecycle-0",
               "WAYBRIDGE_TEST_SEMANTICS": "1"}
        env.pop("WAYBRIDGE_FIXTURE_ALPHA", None)
        with socket.socket() as free:
            free.bind(("127.0.0.1", 0))
            port = free.getsockname()[1]
        token = "gpu-lifecycle-" + "a" * 40
        token_path = Path(runtime) / "token"
        token_path.write_text(token)
        token_path.chmod(0o600)
        binary = os.environ.get("WAYBRIDGE_HOST_BINARY", str(ROOT / "target/debug/waybridge"))
        host = subprocess.Popen([binary, "--listen", f"127.0.0.1:{port}", "--socket", "gpu-lifecycle-0",
                                 "--token-file", str(token_path), "--codec", "h264-nvenc",
                                 "--gpu-input", "cuda", "--stats"], env=env, stdout=log, stderr=log)
        app = viewer = None
        try:
            wait_socket(host, Path(runtime) / "gpu-lifecycle-0")
            viewer = LifecycleViewer(port, token)
            app = subprocess.Popen([str(ROOT / "build/tests/test-app")], env=env, stdout=log, stderr=log)
            viewer.wait(lambda: len(viewer.infos) == 3 and len(viewer.frames) == 3)
            ids = {int(info["title"].split()[-1]): id for id, info in viewer.infos.items()}
            main_id, other_id, dialog_id = ids[1], ids[2], ids[3]
            initial = viewer.frames[main_id][-1]
            size = initial["w"], initial["h"]

            def key(code):
                viewer.send(type="focus", id=dialog_id, focused=True)
                viewer.send(type="key", id=dialog_id, key=code, down=True)
                viewer.send(type="key", id=dialog_id, key=code, down=False)

            for cycle in range(3):
                before = viewer.frames[main_id][-1]["seq"]
                key(24)  # Change actual source pixels, proving remap displays fresh content.
                expected = (0, 0, 255) if cycle % 2 == 0 else (255, 0, 0)

                def fresh_pixels():
                    chain = viewer.frames[main_id]
                    if chain[-1]["seq"] <= before:
                        return False
                    image = decoded(chain)
                    pixel = image.getpixel((image.width // 2, image.height // 4))
                    return max(abs(pixel[n] - expected[n]) for n in range(3)) < 30

                viewer.wait(fresh_pixels)
                before = viewer.frames[main_id][-1]["seq"]
                key(35)  # Real wl_surface.attach(NULL), rather than native minimize.
                viewer.wait(lambda: not viewer.visibility.get(main_id, {}).get("mapped", True))
                other_sequence = viewer.frames[other_id][-1]["seq"]
                other_width = viewer.frames[other_id][-1]["w"] + 1
                other_height = viewer.frames[other_id][-1]["h"]
                viewer.send(type="resize", id=other_id, width=other_width, height=other_height,
                            dpi=96, fullscreen=False,
                            fullscreen_epoch=viewer.infos[other_id]["fullscreen_epoch"])
                # Rendering another surface drains GL cleanup while the first is detached.
                viewer.wait(lambda: viewer.frames[other_id][-1]["seq"] > other_sequence and
                            viewer.frames[other_id][-1]["w"] == other_width)
                key(47)
                viewer.wait(lambda: viewer.visibility.get(main_id, {}).get("mapped", False))
                viewer.wait(fresh_pixels)
                assert (viewer.frames[main_id][-1]["w"], viewer.frames[main_id][-1]["h"]) == size
                assert host.poll() is None and app.poll() is None

            text = log_path.read_text()
            registrations = [line for line in text.splitlines()
                             if "CUDA GL color input registered" in line and f"id={main_id} " in line]
            assert len(registrations) >= 4, text
            assert 'input="cuda"' in text and 'input="cpu"' not in text, text
            assert "CUDA GL capture failed" not in text and "GPU readback fallback failed" not in text, text
            print("PASS: real CUDA/NVENC same-size detach/remap repeated with other-window rendering; "
                  "fresh decoded pixels and references survive, CUDA re-registers without CPU fallback")
        finally:
            if viewer:
                viewer.media.close()
                viewer.control.close()
            if app:
                app.terminate()
                app.wait(timeout=5)
            host.terminate()
            try:
                host.wait(timeout=10)
            except subprocess.TimeoutExpired:
                host.kill()
                host.wait(timeout=5)


if __name__ == "__main__":
    main()
