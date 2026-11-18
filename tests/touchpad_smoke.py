#!/usr/bin/env python3
"""Negotiated touchpad pan/pinch into an actual Wayland pointer subscriber."""
import os
from pathlib import Path
import shlex
import socket
import subprocess
import tempfile
import time

from smoke import ROOT, Viewer
from touch_smoke import wait


def make_fixture():
    subprocess.run(['bash', 'tests/build-fixture.sh'], cwd=ROOT, check=True)
    build = ROOT / 'build/tests'
    protocols = subprocess.check_output(
        ['pkg-config', '--variable=pkgdatadir', 'wayland-protocols'], text=True).strip()
    xml = Path(protocols) / 'unstable/pointer-gestures/pointer-gestures-unstable-v1.xml'
    header = build / 'pointer-gestures-client-protocol.h'
    implementation = build / 'pointer-gestures-protocol.c'
    subprocess.run(['wayland-scanner', 'client-header', xml, header], check=True)
    subprocess.run(['wayland-scanner', 'private-code', xml, implementation], check=True)
    flags = shlex.split(subprocess.check_output(
        ['pkg-config', '--cflags', '--libs', 'wayland-client'], text=True))
    app = build / 'touchpad-app'
    supporting = ('xdg-shell', 'xdg-decoration', 'cursor-shape', 'tablet', 'xdg-dialog', 'xdg-activation')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', build,
                    ROOT / 'tests/touchpad_app.c', implementation,
                    *(build / f'{name}-protocol.c' for name in supporting), '-o', app, *flags], check=True)
    return app


class TouchpadViewer(Viewer):
    def __init__(self, port, token, enabled):
        self.geometry = {}
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=6)
        self.send(type='hello', version=3, token=token, window_semantics_v1=True, touchpad_v1=enabled)
        assert self.receive()[1]['touchpad_v1'] is enabled

    def focus(self, window):
        self.send(type='focus', id=window, focused=True)
        self.send(type='pointer', id=window, x=35, y=50)

    def gesture(self, window, number, phase, **fields):
        self.send(type='touchpad', id=window, gesture=number, phase=phase, **fields)

    def barrier(self):
        token = time.monotonic_ns()
        self.send(type='ping', nonce=token)
        self.until(lambda k, v: k == 'control' and v.get('type') == 'pong' and v['nonce'] == token)
        time.sleep(.08)


def main():
    fixture = make_fixture()
    with tempfile.TemporaryDirectory(prefix='wb-touchpad-') as directory:
        root = Path(directory)
        env = {**os.environ, 'XDG_RUNTIME_DIR': directory, 'WAYLAND_DISPLAY': 'touchpad-0'}
        token = 'touchpad-fixture-' + 'c' * 40
        secret = root / 'token'
        secret.write_text(token)
        secret.chmod(0o600)
        with socket.socket() as free:
            free.bind(('127.0.0.1', 0))
            port = free.getsockname()[1]
        host_log = ROOT / 'build/tests/touchpad-host.log'
        app_log = ROOT / 'build/tests/touchpad-app.log'
        host = app = viewer = None
        try:
            with host_log.open('w') as log:
                host = subprocess.Popen([ROOT / 'target/debug/waybridge', '--codec', 'png', '--no-audio',
                                         '--no-xwayland', '--listen', f'127.0.0.1:{port}', '--socket',
                                         'touchpad-0', '--token-file', secret], env=env, stdout=log, stderr=log)
            wait(lambda: (root / 'touchpad-0').exists() or host.poll() is not None)
            assert host.poll() is None, host_log.read_text()
            with app_log.open('w') as log:
                app = subprocess.Popen([fixture], env=env, stdout=log, stderr=log)
            viewer = TouchpadViewer(port, token, False)
            first = viewer.until(lambda k, v: k == 'control' and v.get('title') == 'Touchpad first')['id']
            viewer.focus(first)
            viewer.send(type='hello', touchpad_v1=True)
            viewer.gesture(first, 1, 'begin', kind='pinch')
            viewer.barrier()
            assert 'PINCH' not in app_log.read_text()
            viewer.sock.close()
            time.sleep(.2)

            viewer = TouchpadViewer(port, token, True)
            second = viewer.until(lambda k, v: k == 'control' and v.get('title') == 'Touchpad second')['id']
            viewer.focus(first)
            viewer.gesture(first, 1, 'begin', kind='scroll')
            viewer.gesture(first, 1, 'update', dx=12.5, dy=-20)
            viewer.gesture(first, 1, 'end')
            viewer.gesture(first, 2, 'begin', kind='pinch')
            viewer.gesture(first, 2, 'update', scale=1.25)
            viewer.gesture(first, 2, 'end')
            wait(lambda: 'PINCH_END 0' in app_log.read_text())
            text = app_log.read_text()
            assert all(expected in text for expected in (
                'SCROLL_SOURCE 1', 'SCROLL_UPDATE 1 12.50', 'SCROLL_UPDATE 0 -20.00',
                'SCROLL_STOP 0', 'SCROLL_STOP 1', 'PINCH_BEGIN 1 2', 'PINCH_UPDATE 1.25')), text

            viewer.gesture(first, 3, 'begin', kind='pinch')
            viewer.gesture(first, 2, 'update', scale=4)
            viewer.send(type='leave', id=first)
            viewer.gesture(first, 3, 'update', scale=5)
            viewer.focus(second)
            viewer.gesture(second, 4, 'begin', kind='pinch')
            viewer.gesture(second, 4, 'update', scale=.75)
            viewer.send(type='scroll', id=second, x=0, y=15)
            viewer.gesture(second, 4, 'end')
            viewer.barrier()
            text = app_log.read_text()
            assert 'PINCH_END 1' in text and 'PINCH_BEGIN 2 2' in text and 'PINCH_UPDATE 0.75' in text, text
            assert 'PINCH_UPDATE 4.00' not in text and 'PINCH_UPDATE 5.00' not in text, text
            assert 'SCROLL_SOURCE 0' in text and 'SCROLL_UPDATE 0 15.00' in text, text

            viewer.gesture(second, 5, 'begin', kind='pinch')
            viewer.gesture(second, 5, 'update', scale=1.5)
            viewer.send(type='stream_config', read_only=True, fps=30, bitrate=12000, codec='png')
            assert viewer.until(lambda k, v: k == 'control' and v.get('type') == 'stream_config')['ok']
            viewer.gesture(second, 6, 'begin', kind='pinch')
            viewer.gesture(second, 6, 'update', scale=2)
            viewer.barrier()
            text = app_log.read_text()
            assert text.count('PINCH_END 1') == 2 and 'PINCH_UPDATE 1.50' in text, text
            assert 'PINCH_UPDATE 2.00' not in text, text

            viewer.send(type='stream_config', read_only=False, fps=30, bitrate=12000, codec='png')
            viewer.focus(second)
            viewer.gesture(second, 7, 'begin', kind='scroll')
            viewer.gesture(second, 7, 'update', dx=0, dy=28)
            wait(lambda: 'SCROLL_UPDATE 0 28.00' in app_log.read_text())
            viewer.sock.close()
            wait(lambda: app_log.read_text().count('SCROLL_STOP 0') == 2)
            print('PASS finger-source pan/stop, two-finger pinch, focus/read-only/disconnect cancellation, stale and legacy rejection, wheel protocol')
        finally:
            if viewer: viewer.sock.close()
            for child in (app, host):
                if child and child.poll() is None:
                    child.terminate()
                    child.wait(timeout=6)


if __name__ == '__main__':
    main()
