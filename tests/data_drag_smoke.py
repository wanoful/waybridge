#!/usr/bin/env python3
"""Real Qt/Wayland text, native MOVE, and OLE-return protocol regressions."""
import base64
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time

from smoke import ROOT, Viewer
from drag_out_smoke import wait_for


def main():
    executable = ROOT / 'build/tests/drag-widgets'
    executable.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['c++', '-std=c++17', '-fPIC', str(ROOT / 'tests/drag_widgets.cpp'),
                    '-o', str(executable), *subprocess.check_output(
                        ['pkg-config', '--cflags', '--libs', 'Qt6Widgets'], text=True).split()], check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-data-drag-') as directory:
        root = Path(directory)
        token = root / 'token'
        token.write_text('data-drag-test-' + '5' * 40)
        token.chmod(0o600)
        (root / 'files/folder').mkdir(parents=True)
        (root / 'files/source.txt').write_text('payload')
        env = {**os.environ, 'XDG_RUNTIME_DIR': directory, 'WAYLAND_DISPLAY': 'dnd-0',
               'QT_QPA_PLATFORM': 'wayland', 'QT_WAYLAND_DISABLE_WINDOWDECORATION': '1'}
        with socket.socket() as free:
            free.bind(('127.0.0.1', 0))
            port = free.getsockname()[1]
        with open(root / 'app.log', 'w+') as log, open(root / 'host.log', 'w+') as host_log:
            host = subprocess.Popen([str(ROOT / 'target/debug/waybridge'), '--socket', 'dnd-0',
                '--listen', f'127.0.0.1:{port}', '--token-file', str(token), '--codec', 'png',
                '--no-audio', '--no-xwayland'], env=env, stdout=host_log, stderr=host_log)
            app = viewer = None
            try:
                wait_for(lambda: (root / 'dnd-0').exists())
                viewer = Viewer(port, token.read_text(), file_drop_out=True, data_drag_v1=True)
                viewer.send(type='hello', file_drop_out=True, toplevel_drag_v1=True)
                app = subprocess.Popen([str(executable), str(root / 'files')], env=env, stdout=log, stderr=log)
                ids = {}
                for _ in range(2):
                    window = viewer.until(lambda k, m: k == 'control' and m['type'] == 'window' and m['title'] not in ids)
                    ids[window['title']] = window['id']
                first, second = ids['Waybridge drag source'], ids['Waybridge drag target']
                for ident in (first, second):
                    viewer.until(lambda k, m: k == 'frame' and m[0] == ident)

                def motion(ident, x, y):
                    viewer.send(type='pointer', id=ident, x=x, y=y)

                def button(ident, down):
                    viewer.send(type='button', id=ident, button=272, down=down)

                def begin_text():
                    time.sleep(.6)  # Avoid treating consecutive presses as a double click.
                    motion(first, 60, 40)
                    button(first, True)
                    motion(first, 100, 40)
                    return viewer.until(lambda k, m: k == 'control' and m['type'] == 'drag_offer')

                def expect_line(text):
                    wait_for(lambda: text in (root / 'app.log').read_text(), 6)

                offer = begin_text()
                assert base64.b64decode(offer['text']) == b'Waybridge drag text'
                assert offer['files'] == []
                motion(first, 100, 120)
                time.sleep(.2)
                button(first, False)
                expect_line('SAME Waybridge drag text')

                # Moving between streamed windows must preserve the original
                # Wayland source, offered MIME types and negotiated MOVE action.
                begin_text()
                motion(second, 100, 120)
                time.sleep(.2)
                button(second, False)
                expect_line('OTHER Waybridge drag text')

                # Once OLE has claimed a source, returning must resume that same
                # source rather than reuploading a file or accepting our own OLE object.
                offer = begin_text()
                viewer.send(type='drag_claim', drag=offer['drag'])
                viewer.until(lambda k, m: k == 'control' and m['type'] == 'drag_ready' and m['drag'] == offer['drag'])
                viewer.send(type='drag_return', drag=offer['drag'], return_token=1, id=second, x=100, y=120)
                viewer.until(lambda k, m: k == 'control' and m['type'] == 'drag_return_ready' and m['drag'] == offer['drag'] and m['accepted'])
                button(second, False)
                wait_for(lambda: any(len(line.removeprefix('OTHER ')) == 2 * len('Waybridge drag text')
                    for line in (root / 'app.log').read_text().splitlines() if line.startswith('OTHER ')), 6)

                # Generic QFileSystemModel move, same semantics used by file managers.
                coords = {name: (int(x), int(y)) for name, x, y in re.findall(
                    r'ITEM (source.txt|folder) (\d+) (\d+)', (root / 'app.log').read_text())}
                sx, sy = coords['source.txt']
                tx, ty = coords['folder']
                time.sleep(.6)
                motion(first, sx, sy)
                button(first, True)
                time.sleep(.1)
                motion(first, sx + 35, sy)
                time.sleep(.3)
                motion(first, tx, ty)
                time.sleep(.3)
                button(first, False)
                wait_for(lambda: (root / 'files/folder/source.txt').exists(), 6)
                assert not (root / 'files/source.txt').exists()
                assert (root / 'files/folder/source.txt').read_text() == 'payload'

                # External text uses MIME acceptance and COPY action before drop.
                viewer.send(type='text_drag_enter', drag=1, id=second, x=100, y=120,
                            data=base64.b64encode('Windows 你好'.encode()).decode())
                viewer.until(lambda k, m: k == 'control' and m['type'] == 'text_drag_status' and m['drag'] == 1 and m['accepted'])
                viewer.send(type='text_drag_drop', drag=1, id=second, x=100, y=120)
                expect_line('Windows 你好')
                assert 'protocol error' not in (root / 'host.log').read_text().lower()
                print('PASS: real Qt same-window/cross-window text, OLE-return source, file MOVE and external Unicode COPY')
            except Exception:
                print((root / 'app.log').read_text())
                print((root / 'host.log').read_text())
                raise
            finally:
                if viewer:
                    viewer.sock.close()
                if app:
                    app.terminate()
                    app.wait(timeout=5)
                host.terminate()
                host.wait(timeout=10)


if __name__ == '__main__':
    main()
