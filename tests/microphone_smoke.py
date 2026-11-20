#!/usr/bin/env python3
"""Exercise microphone permission, wire framing and the real Pulse/PipeWire source."""
import json
import math
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from audio_smoke import control_message, packet, send
from smoke import ROOT


def wait(check, seconds=8):
    deadline = time.monotonic() + seconds
    while not check():
        assert time.monotonic() < deadline, "microphone fixture timed out"
        time.sleep(.02)


def closed(connection):
    try:
        return connection.recv(1) == b""
    except ConnectionResetError:
        return True


def main():
    logs = ROOT / "build/tests"
    logs.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="wb-mic-") as directory, (logs / "microphone.log").open("w+") as log:
        root = Path(directory)
        token = "microphone-fixture-" + "a" * 42
        secret = root / "token"
        secret.write_text(token)
        secret.chmod(0o600)
        name = root / "source"
        with socket.socket() as free:
            free.bind(("127.0.0.1", 0))
            address = free.getsockname()
        pulse_server = os.environ.get("PULSE_SERVER") or f"unix:{os.environ['XDG_RUNTIME_DIR']}/pulse/native"
        env = {**os.environ, "PULSE_SERVER": pulse_server, "XDG_RUNTIME_DIR": directory}
        host = subprocess.Popen([
            ROOT/"target/debug/waybridge", "--audio", "--no-xwayland", "--codec", "png",
            "--socket", "microphone-test", "--listen", f"127.0.0.1:{address[1]}", "--token-file", secret,
            "--", sys.executable, "-c",
            'import os,sys; from pathlib import Path; Path(sys.argv[1]).write_text(os.environ["PULSE_SOURCE"])',
            name,
        ], env=env, stdout=log, stderr=log)
        children = [host]
        streams = []

        def connect(channel=None, request=1, offered_key=None, offered_token=token):
            connection = socket.create_connection(address, timeout=4)
            streams.append(connection)
            if channel:
                send(connection, type="hello", version=3, token=offered_token, channel=channel,
                     media_key=hello["media_key"] if offered_key is None else offered_key, request_id=request)
            else:
                send(connection, type="hello", version=3, token=offered_token, microphone_v1=True, audio_pcm=True)
            return connection

        def configure(request, enabled):
            send(control, type="microphone_enabled", request_id=request, enabled=enabled)
            result = control_message(control, "microphone_state")
            assert result["request_id"] == request
            return result["allowed"]

        def read_only(value, request):
            send(control, type="stream_config", request_id=request, fps=30, bitrate=30000,
                 codec="png", read_only=value)
            result = control_message(control, "stream_config")
            assert result["ok"] and result["read_only"] == value

        def samples(connection, sequence, pcm):
            body = b"\3" + struct.pack(">IHHIQQ", 48000, 1, 16, 480, sequence, sequence * 10000) + pcm
            connection.sendall(struct.pack(">I", len(body)) + body)
            assert packet(connection) == (3, struct.pack(">Q", sequence))

        def list_sources():
            return json.loads(subprocess.check_output(["pactl", "--format=json", "list", "sources"], env=env))

        source = None
        try:
            wait(lambda: name.exists() or host.poll() is not None)
            assert host.poll() is None, (logs / "microphone.log").read_text()
            source = name.read_text()
            assert source.startswith("waybridge-microphone-")
            assert any(entry["name"] == source for entry in list_sources())
            control = connect()
            _, hello = packet(control)
            assert hello["microphone_v1"] and hello["audio_pcm"]
            assert closed(connect("microphone")), "microphone opened without permission"
            assert configure(1, True)
            assert closed(connect("microphone", offered_token="wrong"))
            assert closed(connect("microphone", offered_key="wrong"))
            assert closed(connect("microphone", request=2))
            microphone = connect("microphone")
            assert packet(microphone) == (0, {"type": "microphone_ready"})
            assert closed(connect("microphone")), "two sockets for one microphone lease"

            capture_path = root / "capture.pcm"
            with capture_path.open("wb") as destination:
                capture = subprocess.Popen([
                    "parec", "--raw", "--format=s16le", "--rate=48000", "--channels=1",
                    "--latency-msec=10", "--device=" + source,
                ], env=env, stdout=destination, stderr=log)
                children.append(capture)
                time.sleep(.12)
                pcm = struct.pack("<480h", *(int(12000 * math.sin(2 * math.pi * 500 * i / 48000)) for i in range(480)))
                start = time.monotonic()
                for sequence in range(1, 81):
                    time.sleep(max(0, start + (sequence - 1) * .01 - time.monotonic()))
                    samples(microphone, sequence, pcm)
                capture.terminate()
                capture.wait(timeout=3)
            raw = capture_path.read_bytes()
            peak = max(map(abs, struct.unpack(f"<{len(raw)//2}h", raw))) if raw else 0
            assert peak >= 5000, (peak, source, len(raw))

            send(control, type="ping", nonce=10)
            assert control_message(control, "pong")["nonce"] == 10
            read_only(True, 2)
            assert closed(microphone), "read-only must revoke an open microphone"
            assert not configure(2, True)
            assert closed(connect("microphone", request=2))
            read_only(False, 3)
            assert configure(3, True)
            assert closed(connect("microphone", request=1)), "a revoked lease reattached"
            resumed = connect("microphone", request=3)
            assert packet(resumed)[1]["type"] == "microphone_ready"
            samples(resumed, 1, pcm)
            resumed.sendall(struct.pack(">I", 72 * 1024 * 1024))
            assert closed(resumed), "microphone framing accepted an oversized packet"
            resumed = connect("microphone", request=3)
            assert packet(resumed)[1]["type"] == "microphone_ready"
            assert not configure(4, False)
            assert closed(resumed)
            assert closed(connect("microphone", request=3))
            assert configure(5, True)
            resumed = connect("microphone", request=5)
            assert packet(resumed)[1]["type"] == "microphone_ready"
            control.close()
            assert closed(resumed), "control disconnect must revoke the microphone"
            print(f"PASS: real Pulse microphone (peak {peak}), permission and socket isolation, stale lease rejection, read-only/off/disconnect revocation")
        finally:
            for connection in streams:
                connection.close()
            for process in reversed(children):
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=10)
            if source is not None:
                assert all(entry["name"] != source for entry in list_sources()), "microphone source leaked"


if __name__ == "__main__":
    main()
