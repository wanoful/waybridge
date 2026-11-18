#!/usr/bin/env python3
"""Exercise the resize preview against separately timed decoded-frame sizes."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import zlib

ROOT = Path(__file__).resolve().parents[1]
FILL = (40, 40, 40)


def wait(check):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if check():
            return
        time.sleep(.03)
    raise AssertionError('resize preview fixture timed out')


def png(width, height, color):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

    # A narrow colored stripe reveals either a change in scale or center anchoring.
    base = bytes((*color, 255))
    stripe = bytes((0, 255, 0, 255))
    row = b'\0' + base * 16 + stripe * 16 + base * (width - 32)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(row * height)) + chunk(b'IEND', b''))


def exercise(worker, probe, env, log):
    window_id = 3000 + os.getpid() % 2000
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        listener.listen()
        listener.settimeout(15)
        viewer = subprocess.Popen([*worker, '127.0.0.1', str(listener.getsockname()[1]), 'resize-fixture'],
                                  env=env, stdout=log, stderr=log)
        try:
            connection, _ = listener.accept()
            with connection, connection.makefile('rb') as events:
                connection.settimeout(10)
                history = []

                def send(packet):
                    connection.sendall(struct.pack('>I', len(packet)) + packet)

                def control(**value):
                    send(b'\0' + json.dumps(value).encode())

                def expect(predicate):
                    if any(predicate(event) for event in history):
                        return
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline:
                        header = events.read(4)
                        assert len(header) == 4, 'worker disconnected'
                        size, = struct.unpack('>I', header)
                        assert 0 < size <= 1024 * 1024
                        packet = events.read(size)
                        assert packet[0] == 0
                        event = json.loads(packet[1:])
                        history.append(event)
                        if predicate(event):
                            return
                    raise AssertionError('expected viewer event missing')

                def native(action, *args, check=True):
                    return subprocess.run([*probe, action, str(window_id), *map(str, args)], env=env,
                                          capture_output=True, text=True, timeout=15, check=check)

                def pixel(x, y):
                    return tuple(map(int, native('pixel', x, y).stdout.split()))

                def frame(sequence, width, height, color):
                    header = struct.pack('>BIIIQiiii', 1, window_id, width, height, sequence, width, height, 0, 0)
                    send(header + png(width, height, color))
                    expect(lambda m: m['type'] == 'frame_ack' and m['sequence'] == sequence)

                control(type='hello', version=3, codec='mixed', window_semantics_v1=True)
                control(type='window', id=window_id, title=f'Waybridge semantics {window_id}',
                        width=320, height=200, decoration='client')
                red, blue, purple = (255, 0, 0), (0, 0, 255), (192, 0, 192)
                stripe = (0, 255, 0)
                frame(1, 320, 200, red)
                wait(lambda: native('exists', check=False).returncode == 0)
                wait(lambda: pixel(24, 30) == stripe)

                native('size-begin')
                native('resize', 480, 140)
                assert pixel(24, 30) == stripe
                assert pixel(10, 130) == red
                assert pixel(400, 40) == FILL
                assert native('hit', 400, 40).stdout.strip() == '1'
                frame(2, 400, 160, blue)
                assert pixel(10, 30) == red and pixel(400, 40) == FILL
                native('size-end')
                expect(lambda m: m['type'] == 'resize' and (m['width'], m['height']) == (480, 140))
                frame(3, 480, 140, blue)
                wait(lambda: pixel(400, 40) == blue)

                native('size-begin')
                native('resize', 240, 210)
                assert pixel(24, 30) == stripe
                assert pixel(10, 180) == FILL
                frame(4, 230, 200, purple)
                assert pixel(10, 30) == blue
                native('size-end')
                wait(lambda: pixel(130, 40) == purple)
                print('PASS: unscaled resize preview, right/bottom fill, obsolete frame ACK, exact frame and bounded fallback')
        finally:
            if viewer.poll() is None:
                viewer.terminate()
                try:
                    viewer.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    viewer.kill()
                    viewer.wait()


def main():
    env = {**os.environ, 'WAYBRIDGE_DPI': '96', 'WAYBRIDGE_PNG': '1'}

    output = ROOT / 'build/tests'
    output.mkdir(parents=True, exist_ok=True)
    probe = output / 'windows-resize-preview-probe.exe'
    subprocess.run(['x86_64-w64-mingw32-g++', '-std=c++17', '-static', '-O2', '-Iclient',
                    '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00', 'tests/windows_semantics_probe.cpp',
                    '-o', str(probe), '-lgdi32'], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix='wb-resize-preview-') as tmp, (output / 'windows-resize-preview.log').open('w') as log:
        read, write = os.pipe()
        xvfb = subprocess.Popen(['Xvfb', '-displayfd', str(write), '-screen', '0', '1280x1024x24', '-nolisten', 'tcp'],
                                pass_fds=[write], stdout=log, stderr=log)
        os.close(write)
        with os.fdopen(read) as pipe:
            display = ':' + pipe.readline().strip()
        env.update(DISPLAY=display, WINEPREFIX=str(Path(tmp) / 'wine'), WINEDEBUG='-all', WAYBRIDGE_GDI='1',
                   WINEDLLOVERRIDES='winemenubuilder.exe=d;mscoree,mshtml=')
        try:
            subprocess.run(['wineboot', '-u'], env=env, stdout=log, stderr=log, timeout=60, check=True)
            exercise(['wine', str(ROOT / 'build/windows/waybridge-client.exe')], ['wine', str(probe)], env, log)
        finally:
            subprocess.run(['wineserver', '-k'], env=env, stdout=log, stderr=log, timeout=10)
            xvfb.terminate()
            xvfb.wait(timeout=5)


if __name__ == '__main__':
    main()
