#!/usr/bin/env python3
"""Actual NVENC startup/live auto policy must retain the same HDR base codec."""
import os
from pathlib import Path
import signal
import struct
import subprocess
import tempfile
import time
import select
from gui_session_smoke import Rpc
from hardware_smoke import ROOT, Viewer


def main():
    subprocess.run(["bash", "tests/build-fixture.sh"], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="waybridge-auto-hdr-") as directory:
        runtime = Path(directory)
        env = {**os.environ, "XDG_RUNTIME_DIR": directory, "XDG_DATA_HOME": str(runtime / "data")}
        rpc = Rpc(env)
        pids = []
        pidfds = []
        viewer = None
        try:
            session = rpc.ok(op="ensure", session="auto-hdr-0", binary=str(ROOT / "target/debug/waybridge"),
                             port=0, codec="auto", hdr=True, audio=False, xwayland=False)
            pids.append(session["pid"])
            pidfds.append(os.pidfd_open(session["pid"]))
            viewer = Viewer(session["port"], session["token"], ["h264_alpha", "hevc_alpha"])
            pids.append(rpc.ok(op="launch", session="auto-hdr-0", binary=str(ROOT / "target/debug/waybridge"),
                               argv=[str(ROOT / "build/tests/test-app")])["pid"])

            def receive(check):
                end = time.monotonic() + 15
                while time.monotonic() < end:
                    kind, value = viewer.receive()
                    if kind in (1, 2, 4, 5):
                        window, = struct.unpack(">I", value[:4])
                        sequence, = struct.unpack(">Q", value[12:20])
                        viewer.send(type="frame_ack", id=window, sequence=sequence)
                    if check(kind, value):
                        return kind, value
                raise AssertionError("configuration did not produce expected actual media")

            # No Main10/HDR capability is advertised: HEVC remains an SDR stream,
            # while the shared auto preference agrees with startup's base codec.
            receive(lambda kind, _: kind == 4)
            for request, (hdr, expected, kind) in enumerate([(True, "hevc", 4), (False, "h264", 2), (True, "hevc", 4)], 1):
                viewer.send(type="stream_config", request_id=request, codec="auto", hdr=hdr,
                            fps=60, bitrate=30000, read_only=False)
                _, ack = receive(lambda k, v: k == 0 and v.get("type") == "stream_config" and v.get("request_id") == request)
                assert ack["ok"] and ack["codec"] == expected and ack["requested_codec"] == "auto", ack
                assert ack["hdr_requested"] == hdr, ack
                viewer.send(type="refresh")
                receive(lambda k, _: k == kind)
            print("PASS: actual HDR+auto startup and live settings retain HEVC; SDR auto selects H264; HDR requires separate negotiated capability")
        finally:
            if viewer:
                viewer.control.close()
                viewer.media.close()
            rpc.close()
            for pid in reversed(pids):
                try:
                    os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            for fd in pidfds:
                assert select.select([fd], [], [], 10)[0], "compositor failed to stop"
                os.close(fd)
            log = runtime / "waybridge-control-center/auto-hdr-0.log"
            if log.exists():
                (ROOT / "build/tests/stream-config-hdr-host.log").write_text(log.read_text())


if __name__ == "__main__":
    main()
