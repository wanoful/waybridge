#!/usr/bin/env python3
"""Run the real Windows WASAPI capture worker with a synthetic default microphone."""
import argparse
import math
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from microphone_smoke import wait
from session_fixture import SessionFixture
from smoke import ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--viewer", type=Path, default=ROOT/"build/windows/waybridge-client.exe")
    options = parser.parse_args()
    for program in ("pipewire", "pipewire-pulse", "wireplumber", "wine", "Xvfb"):
        assert shutil.which(program), program
    build = ROOT / "build/tests"
    build.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="wb-win-mic-") as directory, \
            SessionFixture(Path(directory)/"fixture") as fixture, \
            (build/"windows-microphone.log").open("w+") as log:
        root = Path(directory)
        runtime = root / "pulse"
        runtime.mkdir(mode=0o700)
        env = {**fixture.env, "PIPEWIRE_RUNTIME_DIR": str(runtime),
               "XDG_RUNTIME_DIR": str(runtime), "PULSE_SERVER": f"unix:{runtime}/pulse/native"}
        processes = []
        wine_env = None

        def launch(*args, **options):
            process = subprocess.Popen(args, env=options.pop("env", env), stderr=log,
                                       stdout=options.pop("stdout", log), **options)
            processes.append(process)
            return process

        def pulse(*args):
            return subprocess.check_output(["pactl", *args], env=env, text=True).strip()

        try:
            launch("pipewire")
            wait(lambda: (runtime/"pipewire-0").exists())
            # Only policy is loaded. ALSA, Bluetooth and video monitors stay out
            # of the fixture, so its default can only resolve to generated PCM.
            launch("wireplumber", "--profile=policy")
            launch("pipewire-pulse")
            wait(lambda: (runtime/"pulse/native").exists())
            input_fifo = root/"test-input.fifo"
            pulse("load-module", "module-pipe-source", f"file={input_fifo}",
                  "source_name=waybridge-synthetic-mic", "format=s16le", "rate=48000", "channels=1")
            pulse("set-default-source", "waybridge-synthetic-mic")
            assert pulse("get-default-source") == "waybridge-synthetic-mic"

            read, write = os.pipe()
            launch("Xvfb", "-displayfd", str(write), "-screen", "0", "1280x1024x24",
                   "-nolisten", "tcp", pass_fds=(write,))
            os.close(write)
            with os.fdopen(read) as stream:
                display = ":" + stream.readline().strip()
            wine_env = {**env, "DISPLAY": display, "WINEPREFIX": str(root/"wine"),
                        "WAYBRIDGE_NO_AUDIO": "1", "WAYBRIDGE_GDI": "1", "WINEDEBUG": "-all",
                        "WINEDLLOVERRIDES": "winemenubuilder.exe=d;mscoree,mshtml="}
            subprocess.run(["wineboot", "-u"], env=wine_env, stdout=log, stderr=log, timeout=60, check=True)
            token = "windows-mic-fixture-" + "x" * 40
            secret = root/"token"
            secret.write_text(token)
            secret.chmod(0o600)
            name = root/"source"
            with socket.socket() as free:
                free.bind(("127.0.0.1", 0))
                port = free.getsockname()[1]
            server = launch(
                ROOT/"target/debug/waybridge", "--audio", "--no-xwayland", "--codec", "png",
                "--socket", "windows-microphone-test", "--listen", f"127.0.0.1:{port}", "--token-file", secret,
                "--", sys.executable, "-c",
                'import os,sys; from pathlib import Path; Path(sys.argv[1]).write_text(os.environ["PULSE_SOURCE"])', name,
            )
            wait(lambda: name.exists() or server.poll() is not None)
            assert server.poll() is None, "waybridge microphone server failed"
            target_source = name.read_text()
            assert target_source.startswith("waybridge-microphone-")
            assert pulse("get-default-source") == "waybridge-synthetic-mic"
            viewer = launch(
                "wine", options.viewer.resolve(), "127.0.0.1",
                str(port), token, "--microphone", env=wine_env,
            )
            raw_path = root/"recorded.pcm"
            tone = struct.pack("<480h", *(int(14000 * math.sin(2 * math.pi * 600 * i / 48000)) for i in range(480)))
            peak = 0
            with raw_path.open("wb") as raw:
                record = launch("parec", "--raw", "--format=s16le", "--rate=48000", "--channels=1",
                                "--latency-msec=10", f"--device={target_source}", stdout=raw)
                pipe = os.open(input_fifo, os.O_WRONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
                try:
                    start = time.monotonic()
                    for index in range(650):
                        assert viewer.poll() is None and record.poll() is None
                        time.sleep(max(0, start + index * .01 - time.monotonic()))
                        try:
                            os.write(pipe, tone)
                        except BlockingIOError:
                            pass
                        if index > 100 and index % 40 == 0:
                            pcm = raw_path.read_bytes()
                            peak = max(map(abs, struct.unpack(f"<{len(pcm)//2}h", pcm[:len(pcm)//2*2])), default=0)
                            if peak > 3000:
                                break
                finally:
                    os.close(pipe)
                record.terminate()
                record.wait(timeout=3)
            assert peak > 3000, (peak, raw_path.stat().st_size, "see build/tests/windows-microphone.log")
            print(f"PASS: production Windows PE/WASAPI captured default software mic and reached real Linux source (peak {peak})")
        finally:
            if wine_env is not None:
                subprocess.run(["wineserver", "-k"], env=wine_env, stdout=log, stderr=log, timeout=8)
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)


if __name__ == "__main__":
    main()
