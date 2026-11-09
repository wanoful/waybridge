#!/usr/bin/env python3
"""Shared session: X11 + native Wayland, audio, UDP fallback and HDR policy.

The viewer only advertises PNG and never binds UDP, exercising safe media fallback
while all server subsystems are enabled. No sound reaches the default output.
"""
import io
import json
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time

from PIL import Image
from smoke import ROOT


def wait(check, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(.03)
    raise AssertionError("combined session timed out; see build/tests/session-integration.log")


def command(*args):
    return subprocess.check_output(args, text=True).strip()


def send(stream, **value):
    body = b"\0" + json.dumps(value).encode()
    stream.sendall(struct.pack(">I", len(body)) + body)


def exact(stream, count):
    data = bytearray()
    while len(data) < count:
        block = stream.recv(count - len(data))
        if not block:
            raise EOFError()
        data.extend(block)
    return data


def packet(stream):
    count, = struct.unpack(">I", exact(stream, 4))
    assert 0 < count <= 72 * 1024 * 1024
    data = exact(stream, count)
    return data[0], json.loads(data[1:]) if data[0] == 0 else bytes(data[1:])


def main():
    subprocess.run(["bash", "tests/build-fixture.sh"], cwd=ROOT, check=True)
    default_sink = command("pactl", "get-default-sink")
    pulse_server = next(line.split(": ", 1)[1] for line in command("pactl", "info").splitlines()
                        if line.startswith("Server String:"))
    with tempfile.TemporaryDirectory(prefix="waybridge-integrated-") as directory, \
            open(ROOT / "build/tests/session-integration.log", "w") as log:
        runtime = Path(directory)
        env = {**os.environ, "PULSE_SERVER": pulse_server, "XDG_RUNTIME_DIR": directory,
               "WAYLAND_DISPLAY": "integrated-0"}
        with socket.socket() as free:
            free.bind(("127.0.0.1", 0))
            port = free.getsockname()[1]
        token = "combined-test-" + "a" * 40
        token_file = runtime / "token"
        token_file.write_text(token)
        token_file.chmod(0o600)
        child_env = runtime / "child.json"
        helper = ('import os,json,sys;from pathlib import Path;'
                  'Path(sys.argv[1]).write_text(json.dumps({k:os.environ[k] for k in '
                  '["DISPLAY","PULSE_SINK"]}))')
        host = subprocess.Popen([
            str(ROOT / "target/debug/waybridge"), "--xwayland", "--audio",
            "--udp-listen", "127.0.0.1:0", "--gpu-input", "cuda",
            "--hdr", "experimental-scrgb", "--codec", "hevc-nvenc",
            "--socket", "integrated-0", "--listen", f"127.0.0.1:{port}",
            "--token-file", str(token_file), "--", sys.executable, "-c", helper, str(child_env),
        ], env=env, stdout=log, stderr=log)
        clients = []
        control = media = audio = None
        sink = x_socket = None
        try:
            wait(lambda: child_env.exists() or host.poll() is not None)
            assert host.poll() is None, "combined server failed startup"
            private = json.loads(child_env.read_text())
            sink = private["PULSE_SINK"]
            assert sink == f"waybridge-{host.pid}"
            number = int(private["DISPLAY"].lstrip(":"))
            x_socket = Path(f"/tmp/.X11-unix/X{number}")
            assert x_socket.stat().st_mode & 0o077 == 0
            control = socket.create_connection(("127.0.0.1", port), timeout=10)
            send(control, type="hello", version=3, token=token, split_media=True,
                 frame_ack=True, udp_fec_v1=True, audio_pcm=True, file_tree=True,
                 file_drop_out=True, cursor_v1=True, window_icon_v1=True,
                 window_semantics_v1=True, pointer_lock=True, codecs=["png_rgba"], dpi=168)
            kind, hello = packet(control)
            assert kind == 0 and hello["type"] == "hello"
            for feature in ("cursor_v1", "window_icon_v1", "window_semantics_v1", "audio_pcm"):
                assert hello[feature] is True, feature
            assert hello["udp_fec_v1"]["version"] == 1
            for channel in ("media", "audio"):
                stream = socket.create_connection(("127.0.0.1", port), timeout=10)
                send(stream, type="hello", version=3, token=token, channel=channel,
                     media_key=hello["media_key"])
                assert packet(stream)[1]["type"] == channel + "_ready"
                if channel == "media":
                    media = stream
                else:
                    audio = stream
            # Consume authenticated PCM; nothing is routed to a local playback endpoint.
            for _ in range(4):
                kind, body = packet(audio)
                assert kind == 3
                rate, channels, bits, frames, seq, _ = struct.unpack(">IHHIQQ", body[:28])
                assert (rate, channels, bits, frames) == (48000, 2, 16, 480)
                assert len(body) == 1948
                send(audio, type="audio_ack", sequence=seq)
            clients.append(subprocess.Popen([str(ROOT / "build/tests/test-app")],
                                             env=env, stdout=log, stderr=log))
            clients.append(subprocess.Popen(["glxgears", "-geometry", "320x240"],
                                             env={**env, "DISPLAY": private["DISPLAY"]},
                                             stdout=log, stderr=log))
            infos, seen, pong = {}, set(), False
            send(control, type="ping", nonce="combined-session")
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline and (len(seen) < 2 or not pong):
                ready, _, _ = select.select([control, media], [], [], 5)
                assert ready, "media fallback or control stalled"
                for stream in ready:
                    kind, value = packet(stream)
                    if kind == 0:
                        if value["type"] == "window":
                            infos[value["id"]] = value
                        elif value["type"] == "pong":
                            assert value["nonce"] == "combined-session"
                            pong = True
                        assert value["type"] != "udp_start", "UDP was never authenticated"
                    else:
                        assert kind == 1, "PNG-only peer must never receive a video codec"
                        window, width, height, seq = struct.unpack(">IIIQ", value[:20])
                        image = Image.open(io.BytesIO(value[36:])).convert("RGBA")
                        assert image.size == (width, height)
                        seen.add(window)
                        send(control, type="frame_ack", id=window, sequence=seq)
            assert len(seen) >= 2 and pong
            assert any(info.get("x11") for info in infos.values())
            assert any(not info.get("x11") for info in infos.values())
            assert command("pactl", "get-default-sink") == default_sink
            host.terminate()
            assert host.wait(timeout=10) == 0
            wait(lambda: not x_socket.exists())
            wait(lambda: all(item[1] != sink for line in command("pactl", "list", "short", "sinks").splitlines()
                             if len(item := line.split("\t")) > 1))
            assert command("pactl", "get-default-sink") == default_sink
            print("PASS: combined capabilities, audio auth/PCM, PNG fallback with unbound UDP and HDR policy, "
                  "native Wayland + X11, responsive control, shared SIGTERM sink/socket cleanup")
        finally:
            for stream in (audio, media, control):
                if stream:
                    stream.close()
            for client in reversed(clients):
                if client.poll() is None:
                    client.terminate()
                    try:
                        client.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        client.kill()
                        client.wait(timeout=5)
            if host.poll() is None:
                host.terminate()
                try:
                    host.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    host.kill()
                    host.wait(timeout=5)


if __name__ == "__main__":
    main()
