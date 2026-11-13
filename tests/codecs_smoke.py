#!/usr/bin/env python3
"""Actual multi-codec NVENC streams and compositor protocol; needs NVIDIA NVENC."""
import io
import json
import os
import re
import select
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path
from PIL import Image
from hardware_smoke import Viewer, decoded, ROOT

CODECS = {2: ("h264", "h264_alpha", "h264-nvenc"), 4: ("hevc", "hevc_alpha", "hevc-nvenc"), 5: ("obu", "av1_alpha", "av1-nvenc")}


def verify_packet(data, kind, key):
    if kind != 5:
        nals = [data[m.end()] for m in re.finditer(rb'\x00\x00\x00?\x01', data) if m.end() < len(data)]
        types = [n & 31 if kind == 2 else (n >> 1) & 63 for n in nals]
        assert types and types[0] == (9 if kind == 2 else 35), types
        assert (12 if kind == 2 else 38) not in types, types
        if key:
            assert ({7, 8, 5} if kind == 2 else {32, 33, 34}).issubset(types), types
            if kind == 4:
                assert any(t in (19, 20) for t in types), types
    else:
        offset, types = 0, []
        while offset < len(data):
            header = data[offset]
            offset += 1
            assert not header & 0x81 and header & 2, hex(header)
            types.append((header >> 3) & 15)
            if header & 4:
                offset += 1
            size = 0
            for shift in range(0, 56, 7):
                byte = data[offset]
                offset += 1
                size |= (byte & 127) << shift
                if not byte & 128:
                    break
            else:
                raise AssertionError("invalid OBU size")
            offset += size
            assert offset <= len(data)
        assert types and types[0] == 2 and 15 not in types, types
        assert any(t in (3, 6) for t in types), types
        if key:
            assert 1 in types, types


def native():
    flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "libavcodec", "libavutil"], text=True).split()
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "tests/generate_codec_stream.c", "native/nvenc.c", "-o", "build/tests/generate-codec-stream", *flags], cwd=ROOT, check=True)
    for kind, (extension, _, _) in CODECS.items():
        path = ROOT / f"build/tests/native-codec.{extension}"
        result = subprocess.check_output([str(ROOT / "build/tests/generate-codec-stream"), str(kind), "640", "480", str(path)], text=True)
        rows = [list(map(int, row.split())) for row in result.splitlines()]
        offset, chunks = 0, []
        for sequence, length, key in rows:
            chunk = path.read_bytes()[offset:offset+length]
            offset += length
            verify_packet(chunk, kind, key)
            chunks.append(chunk)
        assert offset == path.stat().st_size and [row[2] for row in rows] == [1, 0, 0, 1, 0, 0], rows
        for segment in (chunks[:3], chunks[3:]):
            output = subprocess.check_output(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", extension, "-i", "pipe:0", "-f", "rawvideo", "-pix_fmt", "rgba", "pipe:1"], input=b"".join(segment))
            assert len(output) == 3 * 640 * 480 * 4
            assert Image.frombytes("RGBA", (640, 480), output[:640*480*4]).getpixel((320, 240))[1] > 225
        path.with_suffix(path.suffix + ".sizes").write_text("\n".join(str(row[1]) for row in rows) + "\n")
    print("PASS: native H264/HEVC/AV1 C adapters, original wrapper retained, codec-specific framing/no filler, independent forced keyframes on the same encoder and intact inter-frame references")


def check_alpha(frames):
    image = decoded(frames)
    w, h = image.size
    red, green, clear = (image.getpixel((w*x//6, h//2)) for x in (1, 3, 5))
    assert red[0] > 225 and red[1] < 25 and red[3] == 255, red
    assert green[1] > 225 and green[0] < 25 and green[3] == 128, green
    assert clear[3] == 0, clear


def wait_socket(host, path):
    end = time.monotonic() + 10
    while not path.exists():
        assert host.poll() is None and time.monotonic() < end
        time.sleep(.02)


def compositor(kind):
    extension, capability, cli = CODECS[kind]
    with tempfile.TemporaryDirectory(prefix="waybridge-codecs-") as runtime, open(ROOT / f"build/tests/codecs-{kind}.log", "w") as log:
        env = {**os.environ, "XDG_RUNTIME_DIR": runtime, "WAYLAND_DISPLAY": "codec-0", "WAYBRIDGE_FIXTURE_ALPHA": "1"}
        with socket.socket() as free:
            free.bind(("127.0.0.1", 0)); port = free.getsockname()[1]
        token = "codec-test-" + "b" * 40
        token_file = Path(runtime) / "token"
        token_file.write_text(token); token_file.chmod(0o600)
        host = subprocess.Popen([str(ROOT / "target/debug/waybridge"), "--listen", f"127.0.0.1:{port}", "--socket", "codec-0", "--token-file", str(token_file), "--codec", cli, "--stats"]+(["--gpu-input", os.environ["WAYBRIDGE_GPU_INPUT"]] if os.getenv("WAYBRIDGE_GPU_INPUT") else []), env=env, stdout=log, stderr=log)
        app = viewer = None
        try:
            wait_socket(host, Path(runtime) / "codec-0")
            viewer = Viewer(port, token, capability, kind)
            app = subprocess.Popen([str(ROOT / "build/tests/test-app")], env=env, stdout=log, stderr=log)
            first = viewer.video()
            assert first['key'] == 1
            frames = [first]
            for _ in range(4):
                viewer.send(type='refresh'); frames.append(viewer.video())
            assert len({f['epoch'] for f in frames}) == 1 and any(not f['key'] for f in frames[1:])
            for frame in frames:
                verify_packet(frame['color'], kind, frame['key'])
            check_alpha(frames)
            (ROOT / f"build/tests/server-codec.{extension}").write_bytes(b''.join(f['color'] for f in frames))
            (ROOT / f"build/tests/server-codec.{extension}.sizes").write_text('\n'.join(str(len(f['color'])) for f in frames) + '\n')
            viewer.send(type='refresh'); a = viewer.video(ack=False)
            viewer.send(type='refresh'); b = viewer.video(ack=False)
            viewer.send(type='refresh'); time.sleep(.1)
            assert not select.select([viewer.media], [], [], .15)[0], 'two credits must stop each codec'
            viewer.send(type='ping', nonce=kind)
            viewer.until(lambda k, v: k == 0 and v.get('type') == 'pong' and v['nonce'] == kind)
            viewer.send(type='frame_ack', id=first['id'], sequence=b['seq'])
            c = viewer.video()
            check_alpha(frames + [a, b, c])
            # A delayed fallback from a different codec must not disable this stream.
            wrong = 'av1_alpha' if kind != 5 else 'hevc_alpha'
            viewer.send(type='codec_fallback', id=first['id'], codec=wrong)
            viewer.send(type='refresh'); assert viewer.video()['epoch'] == first['epoch']
            previous = first['epoch']
            for width, height in ((641, 481), (960, 640), (641, 481), (960, 640)):
                viewer.send(type='resize', id=first['id'], width=width, height=height, dpi=96)
                frame = viewer.video()
                while (frame['w'], frame['h']) != (width, height):
                    frame = viewer.video()
                assert frame['key'] == 1 and frame['epoch'] != previous
                assert (frame['cw'], frame['ch']) == ((width+1)&~1, (height+1)&~1)
                verify_packet(frame['color'], kind, True)
                viewer.send(type='refresh'); following = viewer.video()
                assert following['epoch'] == frame['epoch'] and not following['key']
                check_alpha([frame, following])
                previous = frame['epoch']
            viewer.send(type='codec_fallback', id=first['id'], codec=capability)
            png = viewer.until(lambda k, v: k == 1)
            assert Image.open(io.BytesIO(png[36:])).convert('RGBA').getpixel((480, 320)) == (0, 255, 0, 128)
            viewer.send(type='frame_ack', id=first['id'], sequence=struct.unpack('>Q', png[12:20])[0])
            viewer.media.close(); viewer.control.close(); time.sleep(.2)
            # An old viewer omitting the selected new capability must receive only PNG.
            viewer = Viewer(port, token, [], kind)
            viewer.until(lambda k, v: k == 1)
            viewer.media.close(); viewer.control.close(); time.sleep(.2)
            # Session fallback cannot poison a newly authenticated viewer.
            viewer = Viewer(port, token, capability, kind)
            recovered = viewer.video()
            assert recovered['key'] == 1 and recovered['epoch'] != previous
            check_alpha([recovered])
            if os.getenv('WAYBRIDGE_REQUIRE_GPU') == '1':
                trace = (ROOT / f"build/tests/codecs-{kind}.log").read_text()
                assert 'input="cuda"' in trace and 'CUDA GL capture failed' not in trace, trace
            print(f"PASS: {capability} real compositor IDR/key + inter references, exact alpha, 2-credit pause/resume, codec-bound fallback, odd size and cached resize epochs, capability gating and reconnect")
        finally:
            if viewer:
                viewer.media.close(); viewer.control.close()
            if app:
                app.terminate(); app.wait(timeout=5)
            host.terminate(); host.wait(timeout=5)


def auto():
    from smoke import Viewer as PngViewer
    with tempfile.TemporaryDirectory(prefix='waybridge-auto-') as runtime, open(ROOT/'build/tests/codecs-auto.log', 'w') as log:
        env = {**os.environ, 'XDG_RUNTIME_DIR': runtime, 'WAYLAND_DISPLAY': 'auto-0'}
        with socket.socket() as free:
            free.bind(('127.0.0.1', 0)); port = free.getsockname()[1]
        token = 'auto-codec-' + 'c'*40
        file = Path(runtime)/'token'; file.write_text(token); file.chmod(0o600)
        host = subprocess.Popen([str(ROOT/'target/debug/waybridge'), '--listen', f'127.0.0.1:{port}', '--socket', 'auto-0', '--token-file', str(file)]+(['--gpu-input',os.environ['WAYBRIDGE_GPU_INPUT']] if os.getenv('WAYBRIDGE_GPU_INPUT') else []), env=env, stdout=log, stderr=log)
        app = viewer = None
        try:
            wait_socket(host, Path(runtime)/'auto-0')
            viewer = Viewer(port, token, ['h264_alpha', 'hevc_alpha', 'av1_alpha'])
            app = subprocess.Popen([str(ROOT/'build/tests/test-app')], env=env, stdout=log, stderr=log)
            assert viewer.video()['kind'] == 2
            viewer.media.close(); viewer.control.close(); time.sleep(.2)
            viewer = Viewer(port, token, ['hevc_alpha', 'av1_alpha'])
            viewer.until(lambda k, v: k == 1)
            print('PASS: default auto remains H264 even when HEVC/AV1 are advertised; no H264 capability selects PNG')
        finally:
            if viewer:
                viewer.media.close(); viewer.control.close()
            if app:
                app.terminate(); app.wait(timeout=5)
            host.terminate(); host.wait(timeout=5)


def main():
    subprocess.run(['bash', 'tests/build-fixture.sh'], cwd=ROOT, check=True)
    native()
    for kind in CODECS:
        compositor(kind)
    auto()


if __name__ == '__main__':
    main()
