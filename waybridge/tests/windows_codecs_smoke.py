#!/usr/bin/env python3
"""Actual PE capability gating and safe unexpected-codec fallback in private Wine/Xvfb."""
import io
import json
import os
import socket
import struct
import subprocess
import tempfile
import time
from PIL import Image
from hardware_smoke import Viewer, ROOT


def main():
    (ROOT/"build/tests").mkdir(parents=True,exist_ok=True)
    subprocess.run(['x86_64-w64-mingw32-g++', '-municode', '-static', '-O2', 'tests/windows_probe.cpp', '-o', 'build/tests/windows-codec-probe.exe', '-lgdi32', '-lshell32', '-lole32', '-luuid'], cwd=ROOT, check=True)
    processes = []
    prefix = ROOT/'build/wine-codecs'; prefix.mkdir(mode=0o700, parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-wine-codecs-') as temporary, open(ROOT/'build/tests/windows-codecs.log', 'w+') as log:
        read_fd, write_fd = os.pipe()
        xvfb = subprocess.Popen(['Xvfb', '-displayfd', str(write_fd), '-screen', '0', '1280x1024x24', '-nolisten', 'tcp'], pass_fds=[write_fd], stdout=log, stderr=log)
        processes.append(xvfb); os.close(write_fd)
        with os.fdopen(read_fd) as pipe:
            display = ':' + pipe.readline().strip()
        env = {**os.environ, 'DISPLAY': display, 'WINEPREFIX': str(prefix), 'WINEDEBUG': '-all', 'WINEDLLOVERRIDES': 'winemenubuilder.exe=d;mscoree,mshtml=', 'WAYBRIDGE_GDI': '1'}

        def packet(sock):
            size, = struct.unpack('>I', Viewer.exact(sock, 4))
            body = Viewer.exact(sock, size)
            assert body[0] == 0
            return json.loads(body[1:])

        def send(sock, kind, body):
            sock.sendall(struct.pack('>I', len(body)+1) + bytes([kind]) + body)

        def control(sock, **value):
            send(sock, 0, json.dumps(value).encode())

        try:
            subprocess.run(['wineboot', '-u'], env=env, stdout=log, stderr=log, timeout=60, check=True)
            result = subprocess.run(['wine', str(ROOT/'build/windows/waybridge-client.exe'), '--check-codecs'], env=env, text=True, capture_output=True, timeout=20, check=True)
            hdr = subprocess.run(['wine', str(ROOT/'build/windows/waybridge-client.exe'), '--check-hdr'], env=env, text=True, capture_output=True, timeout=20, check=True)
            report = json.loads(hdr.stdout)
            assert report['streaming_hdr'] is False and report['current_decoder_format'] == 'NV12' and report['current_swapchain_format'] == 'BGRA8', report
            assert 'HEVC capability=unavailable'  in result.stdout and 'AV1 capability=unavailable' in result.stdout, result.stdout
            for kind, capability, name in ((4,'hevc_alpha','HEVC'),(5,'av1_alpha','AV1'),(7,'hevc_main10_pq_alpha16','HEVC Main10'),(8,'av1_main10_pq_alpha16','AV1 Main10')):
                with socket.socket() as listener:
                    listener.bind(('127.0.0.1', 0)); listener.listen(); listener.settimeout(15)
                    port = listener.getsockname()[1]
                    client = subprocess.Popen(['wine', str(ROOT/'build/windows/waybridge-client.exe'), '127.0.0.1', str(port), 'codec-fallback-test-token'], env=env, stdout=log, stderr=log)
                    processes.append(client)
                    sock, _ = listener.accept(); sock.settimeout(12)
                    with sock:
                        hello = packet(sock)
                        assert hello['codecs'] == ['png_rgba'] and hello['hdr'] is False, hello
                        control(sock, type='hello', version=3, codec='mixed', split_media=False, codecs=['png_rgba', 'h264_alpha', 'hevc_alpha', 'av1_alpha'])
                        title = f'Waybridge {name} fallback'
                        control(sock, type='window', id=1, title=title, app_id='waybridge.test', width=400, height=300, scale=1, decoration='server')
                        # Unexpected/unadvertised video must be rejected without breaking PNG.
                        # A complete native NVENC key access unit is included, not a fake codec payload.
                        import re
                        header = (ROOT/('client/hdr_capability_samples.h' if kind>=7 else 'client/video_capability_samples.h')).read_text()
                        label = {4:'Hevc',5:'Av1',7:'HevcHdr',8:'Av1Hdr'}[kind]
                        array = re.search(label + r'CapabilitySample\[\] = \{(.*?)\};', header, re.S).group(1)
                        data = bytes(int(value,16) for value in re.findall(r'0x([0-9a-f]{2})', array))
                        video = struct.pack('>IIIQiiiiIIIIII', 1,256,256,1,400,300,0,0,1,256,256,1,len(data),0) + data
                        send(sock, kind, video)
                        deadline = time.monotonic()+12
                        while time.monotonic()<deadline:
                            value = packet(sock)
                            if value['type'] == 'codec_fallback':
                                assert value['id'] == 1 and value['codec'] == capability, value
                                break
                        else:
                            raise AssertionError('no codec-specific fallback')
                        image = Image.new('RGBA', (400,300), (255,0,0,255))
                        image.paste((0,0,255,255), (0,150,400,300))
                        png = io.BytesIO(); image.save(png, format='PNG')
                        send(sock, 1, struct.pack('>IIIQiiii', 1,400,300,2,400,300,0,0) + png.getvalue())
                        probe_env = {**env, 'WAYBRIDGE_TEST_WINDOW': title}
                        deadline = time.monotonic()+12
                        while time.monotonic()<deadline:
                            result = subprocess.run(['wine', str(ROOT/'build/tests/windows-codec-probe.exe'), 'inspect'], env=probe_env, text=True, capture_output=True, timeout=15)
                            if result.returncode == 0 and ('TOP 255 0 0' in result.stdout or 'TOP 254 0 0' in result.stdout):
                                break
                            time.sleep(.1)
                        else:
                            raise AssertionError(result.stdout + result.stderr)
                    client.wait(timeout=10)
                    assert client.returncode == 0
            print('PASS: actual PE refuses HEVC/AV1 SDR and Main10 HDR capability without a GPU path, rejects unadvertised native streams with codec-bound fallback, then presents exact PNG pixels; no actual Windows HEVC/AV1 hardware decode claim')
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate(); process.wait(timeout=5)


if __name__ == '__main__':
    main()
