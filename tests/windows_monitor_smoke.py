#!/usr/bin/env python3
"""Check viewer monitor metadata and interactive placement on private Wine/Xvfb."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
from smoke import ROOT


def main():
    probe = ROOT/'build/tests/windows-viewport-probe.exe'
    probe.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['x86_64-w64-mingw32-g++', '-std=c++17', '-static', '-O2', '-DUNICODE', '-D_UNICODE',
                    '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00', '-Iclient',
                    'tests/windows_semantics_probe.cpp', '-o', str(probe), '-lgdi32'], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-monitor-') as directory, (ROOT/'build/tests/windows-monitor.log').open('w') as log:
        env = {**os.environ, 'WINEPREFIX': str(Path(directory)/'wine'), 'WINEDEBUG': '-all',
               'WINEDLLOVERRIDES': 'winemenubuilder.exe=d;mscoree,mshtml=',
               'WAYBRIDGE_GDI': '1', 'WAYBRIDGE_PNG': '1', 'WAYBRIDGE_DPI': '168'}
        read_fd, write_fd = os.pipe()
        xvfb = subprocess.Popen(['Xvfb', '-displayfd', str(write_fd), '-screen', '0', '2560x1600x24', '-nolisten', 'tcp'],
                                pass_fds=[write_fd], stdout=log, stderr=log)
        os.close(write_fd)
        with os.fdopen(read_fd) as pipe:
            env['DISPLAY'] = ':' + pipe.readline().strip()
        viewer = None
        try:
            subprocess.run(['wineboot', '-u'], env=env, stdout=log, stderr=log, check=True, timeout=60)
            with socket.socket() as listener:
                listener.bind(('127.0.0.1', 0)); listener.listen(); listener.settimeout(20)
                viewer = subprocess.Popen(['wine', str(ROOT/'build/windows/waybridge-client.exe'), '127.0.0.1',
                                           str(listener.getsockname()[1]), 'monitor-fixture-'+'a'*32], env=env, stdout=log, stderr=log)
                with listener.accept()[0] as connection:
                    connection.settimeout(10)
                    def exact(size):
                        result = b''
                        while len(result) < size:
                            part = connection.recv(size-len(result))
                            assert part, 'viewer disconnected'
                            result += part
                        return result
                    def receive():
                        size, = struct.unpack('>I', exact(4))
                        body = exact(size)
                        assert body[0] == 0
                        return json.loads(body[1:])
                    def send(**message):
                        body = b'\0'+json.dumps(message).encode()
                        connection.sendall(struct.pack('>I', len(body))+body)
                    def resize(fullscreen, epoch):
                        deadline = time.monotonic()+10
                        while time.monotonic() < deadline:
                            message = receive()
                            if message.get('type') == 'resize' and message.get('fullscreen') is fullscreen and message.get('fullscreen_epoch') == epoch:
                                assert (message.get('monitor_width'), message.get('monitor_height')) == (2560,1600), message
                                assert message['dpi'] == 168, message
                                return message
                        raise AssertionError('missing resize')
                    hello = receive()
                    assert hello['type'] == 'hello' and (hello['monitor_width'],hello['monitor_height']) == (2560,1600)
                    send(type='hello', version=3, codec='mixed', window_semantics_v1=True)
                    send(type='window', id=1, title='Waybridge semantics 1', width=500, height=300,
                         decoration='client', fullscreen=False, fullscreen_epoch=0)
                    normal = resize(False,0)
                    paused, released = json.loads(subprocess.check_output(
                        ['wine', str(probe), 'move-burst', '1'], env=env, text=True, timeout=10))
                    dpi = normal['dpi']
                    def work_offset(client_origin):
                        # Xvfb's work area starts at (0, 0). MulDiv rounds to nearest.
                        return tuple(-((coordinate * 96 + dpi // 2) // dpi) for coordinate in client_origin)
                    final = work_offset(released)
                    moves = []
                    deadline = time.monotonic()+5
                    while time.monotonic() < deadline:
                        message = receive()
                        if message['type'] != 'viewport':
                            continue
                        xy = message['work_x'], message['work_y']
                        moves.append(xy)
                        if xy == final:
                            break
                    assert moves and moves[-1] == final, moves
                    assert work_offset(paused) in moves, moves
                    assert len(moves) < 24, f'49 drag positions emitted {len(moves)} viewport updates'
                    print(f'PASS: 49 placements coalesced into {len(moves)} viewport updates, including pause and release')
                    send(type='window_action', id=1, action='fullscreen', fullscreen_epoch=1)
                    full = resize(True,1)
                    assert (full['width'],full['height']) == (1463,914), full
                    send(type='window_action', id=1, action='unfullscreen', fullscreen_epoch=2)
                    restored = resize(False,2)
                    assert (restored['width'],restored['height']) == (normal['width'],normal['height']), restored
            print('PASS: actual viewer sends full 2560x1600 monitor metadata in normal/fullscreen/restored resize replies at 175% DPI')
        finally:
            if viewer and viewer.poll() is None:
                viewer.terminate(); viewer.wait(timeout=10)
            subprocess.run(['wineserver','-k'],env=env,stdout=log,stderr=log,timeout=10)
            xvfb.terminate(); xvfb.wait(timeout=10)


if __name__ == '__main__':
    main()
