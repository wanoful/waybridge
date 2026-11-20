#!/usr/bin/env python3
"""Per-application capture policy with simultaneous PNG/H.264/HEVC on NVENC."""
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

from hardware_smoke import ROOT, Viewer
from udp_flow_smoke import stop


def main():
    subprocess.run(["bash", "tests/build-fixture.sh"], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="waybridge-app-streams-") as runtime, \
            (ROOT / "build/tests/app-stream-modes.log").open("w+") as log:
        port_socket = socket.socket()
        with port_socket:
            port_socket.bind(("127.0.0.1", 0))
            port = port_socket.getsockname()[1]
        token = "app-streams-" + "w" * 40
        token_file = Path(runtime) / "token"
        token_file.write_text(token)
        token_file.chmod(0o600)
        env = {**os.environ, "XDG_RUNTIME_DIR": runtime, "WAYLAND_DISPLAY": "stream-modes-0"}
        host = subprocess.Popen([
            str(ROOT / "target/debug/waybridge"), "--socket", env["WAYLAND_DISPLAY"],
            "--listen", f"127.0.0.1:{port}", "--token-file", str(token_file), "--codec", "hevc-nvenc",
            "--no-audio", "--no-xwayland"], env=env, stdout=log, stderr=log)
        viewer = None
        applications = []
        windows = {}
        received = {}

        def event():
            kind, value = viewer.receive()
            if kind == 0 and value.get("type") == "window":
                windows[value["app_id"]] = value["id"]
            if kind != 0:
                window, _, _, sequence = struct.unpack_from(">IIIQ", value)
                received[window, kind] = value
                viewer.send(type="frame_ack", id=window, sequence=sequence)
            return kind, value

        def receive(check):
            end = time.monotonic() + 12
            while time.monotonic() < end:
                kind, value = event()
                if check(kind, value):
                    return value
            raise AssertionError("missing expected per-application media; see app-stream-modes.log")

        def frame(window, kind):
            if (window, kind) not in received:
                receive(lambda _kind, _value: (window, kind) in received)
            return received.pop((window, kind))

        def modes(request, rules):
            received.clear()
            viewer.send(type="stream_config", request_id=request, codec="hevc", hdr=False, fps=60,
                        bitrate=30000, read_only=False, app_modes=rules)
            reply = receive(lambda k, value: k == 0 and value.get("type") == "stream_config"
                            and value.get("request_id") == request)
            assert reply["ok"] and reply["app_modes"] == len(rules), reply
            viewer.send(type="refresh")

        try:
            viewer = Viewer(port, token, ["h264_alpha", "hevc_alpha"])
            for app in ["tests.ClearEditor", "tests.Game"]:
                applications.append(subprocess.Popen([str(ROOT / "build/tests/test-app")],
                    env={**env, "WAYBRIDGE_TEST_APP_ID": app}, stdout=log, stderr=log))
            receive(lambda k, v: k == 0 and len(windows) == 2)
            editor, game = windows["tests.ClearEditor"], windows["tests.Game"]
            frame(editor, 4)
            old_game = frame(game, 4)
            old_game_epoch = struct.unpack_from(">I", old_game, 36)[0]

            modes(1, {"tests.ClearEditor.desktop": "clear"})
            frame(editor, 1)
            assert struct.unpack_from(">I", frame(game, 4), 36)[0] == old_game_epoch
            modes(2, {"tests.ClearEditor.desktop": "clear", "tests.Game.desktop": "low_latency"})
            frame(game, 2)
            frame(editor, 1)
            modes(3, {})
            frame(editor, 4)
            frame(game, 4)
            print("PASS: per-app clear and low-latency differ concurrently and restore defaults; unchanged window keeps its HEVC epoch")
        finally:
            if viewer is not None:
                viewer.control.close()
                viewer.media.close()
            for app in reversed(applications):
                stop(app)
            stop(host)


if __name__ == "__main__":
    main()
