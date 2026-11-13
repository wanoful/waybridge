#!/usr/bin/env python3
"""Live GUI stream settings across actual NVENC codecs, reference epochs and PNG."""
import io
import os
from pathlib import Path
import signal
import struct
import tempfile
import time
from PIL import Image
from gui_session_smoke import Rpc
from hardware_smoke import Viewer, decoded, ROOT


def main():
    with tempfile.TemporaryDirectory(prefix="waybridge-gui-codec-") as directory:
        rpc = Rpc({**os.environ, "XDG_RUNTIME_DIR": directory, "WAYBRIDGE_FIXTURE_ALPHA": "1"})
        server_pid = app_pid = viewer = None
        try:
            session = rpc.ok(op="ensure", session="gui-codec-0", binary=str(ROOT/"target/debug/waybridge"),
                             port=0, codec="auto", audio=False, xwayland=False)
            server_pid = session["pid"]
            viewer = Viewer(session["port"], session["token"], ["h264_alpha", "hevc_alpha", "av1_alpha"])
            app_pid = rpc.ok(op="launch", session="gui-codec-0", binary=str(ROOT/"target/debug/waybridge"), argv=[str(ROOT/"build/tests/test-app")])["pid"]
            def receive(predicate):
                end = time.monotonic() + 15
                while time.monotonic() < end:
                    kind, value = viewer.receive()
                    if kind in (1, 2, 4, 5):
                        window, = struct.unpack(">I", value[:4])
                        sequence, = struct.unpack(">Q", value[12:20])
                        viewer.send(type="frame_ack", id=window, sequence=sequence)
                    if predicate(kind, value):
                        return value
                raise AssertionError("live codec update did not arrive")
            first = viewer.parse_video(receive(lambda k, _: k == 2), ack=False)
            previous = first["epoch"]
            for request, (codec, kind, fps, bitrate) in enumerate([
                    ("h264", 2, 90, 12000), ("png", 1, 30, 10000), ("hevc", 4, 60, 20000),
                    ("av1", 5, 60, 25000), ("h264", 2, 60, 30000)], 1):
                viewer.send(type="stream_config", request_id=request, codec=codec, fps=fps, bitrate=bitrate, read_only=False)
                ack = receive(lambda k, v: k == 0 and v.get("type") == "stream_config" and v["request_id"] == request)
                assert ack["ok"] and ack["fps"] == fps and ack["bitrate"] == bitrate
                if kind == 1:
                    png = receive(lambda k, _: k == 1)
                    image = Image.open(io.BytesIO(png[36:])).convert("RGBA")
                    assert image.getpixel((image.width//2, image.height//2))[3] == 128
                    continue
                viewer.kind = kind
                body = receive(lambda k, v: k == kind and struct.unpack(">I", v[36:40])[0] != previous)
                frame = viewer.parse_video(body, ack=False)
                assert frame["key"] and frame["epoch"] != previous
                viewer.send(type="refresh")
                following = viewer.parse_video(receive(lambda k, v: k == kind), ack=False)
                assert following["epoch"] == frame["epoch"]
                image = decoded([frame, following])
                color = image.getpixel((image.width//2, image.height//2))
                assert color[1] > 225 and color[0] < 25 and color[3] == 128, color
                previous = frame["epoch"]
            print("PASS: live FPS/bitrate changes and H264 → PNG → HEVC → AV1 → H264 on actual NVENC; fresh independently decodable epochs and exact alpha")
        finally:
            server_log = Path(directory)/"waybridge-control-center/gui-codec-0.log"
            if server_log.exists():
                (ROOT/"build/tests/gui-codec-host.log").write_text(server_log.read_text())
            if viewer:
                viewer.control.close(); viewer.media.close()
            rpc.close()
            for pid in (app_pid, server_pid):
                if pid:
                    try: os.kill(pid, signal.SIGTERM)
                    except ProcessLookupError: pass


if __name__ == "__main__":
    main()
