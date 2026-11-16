#!/usr/bin/env python3
"""Windows wire payload -> compositor -> private Xwayland -> real Qt drop target."""
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
    subprocess.run(['c++', '-std=c++17', '-fPIC', str(ROOT / 'tests/drag_widgets.cpp'), '-o', str(executable),
        *subprocess.check_output(['pkg-config', '--cflags', '--libs', 'Qt6Widgets'], text=True).split()], check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-xdnd-bridge-') as directory:
        root = Path(directory)
        (root / 'files/folder').mkdir(parents=True)
        (root / 'files/source.txt').write_text('local file')
        token = root / 'token'
        token.write_text('xdnd-bridge-' + '5' * 40)
        token.chmod(0o600)
        env = {**os.environ, 'XDG_RUNTIME_DIR': directory, 'WAYLAND_DISPLAY': 'xdnd-0',
               'QT_QPA_PLATFORM': 'xcb', 'GVFS_DISABLE_FUSE': '1', 'WAYBRIDGE_DND_FIXTURE_MOVE':'1', 'RUST_LOG': 'waybridge=debug'}
        with socket.socket() as free:
            free.bind(('127.0.0.1', 0))
            port = free.getsockname()[1]
        with open(root / 'app.log', 'w+') as app_log, open(root / 'host.log', 'w+') as log:
            host = subprocess.Popen([str(ROOT / 'target/debug/waybridge'), '--socket', 'xdnd-0', '--listen',
                f'127.0.0.1:{port}', '--token-file', str(token), '--codec', 'png', '--no-audio'], env=env, stdout=log, stderr=log)
            viewer = app = None
            try:
                wait_for(lambda: (root / 'xdnd-0.session.json').exists())
                viewer = Viewer(port, token.read_text(), data_drag_v1=True)
                app = subprocess.Popen([str(ROOT / 'target/debug/waybridge'), '--socket', 'xdnd-0', '--launch', '--',
                    str(executable), str(root / 'files')], env=env, stdout=app_log, stderr=app_log)
                ids = {}
                while len(ids) < 2:
                    window = viewer.until(lambda k, m: k == 'control' and m['type'] == 'window' and m['title'] not in ids)
                    ids[window['title']] = window['id']
                first, second = ids['Waybridge drag source'], ids['Waybridge drag target']
                for ident in (first, second):
                    viewer.until(lambda k, m: k == 'frame' and m[0] == ident)
                # Unicode, then INCR-sized text through the same generic MIME bridge.
                for drag, text in enumerate(('Windows 你好 λ', 'x' * 180000), 1):
                    before = (root / 'app.log').read_text().count('OTHER ')
                    viewer.send(type='text_drag_enter', drag=drag, id=second, x=100, y=120,
                        data=base64.b64encode(text.encode()).decode())
                    viewer.until(lambda k, m: k == 'control' and m['type'] == 'text_drag_status' and m['drag'] == drag and m['accepted'])
                    viewer.send(type='text_drag_drop', drag=drag, id=second, x=100, y=120)
                    wait_for(lambda: (root / 'app.log').read_text().count('OTHER ') > before, 8)
                    time.sleep(.1)
                assert 'Windows 你好 λ' in (root / 'app.log').read_text()
                wait_for(lambda: re.search(r'ITEM folder (\d+) (\d+)', (root / 'app.log').read_text()))
                tx, ty = map(int, re.search(r'ITEM folder (\d+) (\d+)', (root / 'app.log').read_text()).groups())
                payload = b'Windows original stays unchanged'
                viewer.send(type='upload_begin', transfer=101, name='delivered.txt', size=len(payload))
                viewer.send(type='upload_chunk', transfer=101, data=base64.b64encode(payload).decode())
                viewer.send(type='upload_end', transfer=101)
                viewer.send(type='upload_commit', transfers=[101], drop=True, id=first, x=tx, y=ty)
                viewer.until(lambda k, m: k == 'control' and m['type'] == 'upload_ready')
                saved = root / 'files/folder/delivered.txt'
                wait_for(saved.exists, 8)
                assert saved.read_bytes() == payload
                saved.write_bytes(b'user writable copy')
                print('PASS: integrated private Xwayland Unicode/INCR text and read-only URI delivery copied into a writable Qt folder')
            except Exception:
                print((root / 'app.log').read_text()[:2500])
                print((root / 'host.log').read_text()[-6000:])
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
