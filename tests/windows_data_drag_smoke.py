#!/usr/bin/env python3
"""Production viewer + real Qt + real OLE/SendInput in a private Wine desktop."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from smoke import ROOT
from drag_out_smoke import wait_for


def main():
    output = ROOT / 'build/tests/windows-data-drag-probe.exe'
    subprocess.run(['x86_64-w64-mingw32-g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
        '-Wno-misleading-indentation', '-D_WIN32_WINNT=0x0A00', '-municode', '-static', '-O1', 'tests/windows_data_drag_probe.cpp',
        '-o', str(output), '-lole32', '-lshell32', '-luuid'], cwd=ROOT, check=True)
    executable = ROOT / 'build/tests/drag-widgets'
    subprocess.run(['c++', '-std=c++17', '-fPIC', str(ROOT / 'tests/drag_widgets.cpp'), '-o', str(executable),
        *subprocess.check_output(['pkg-config', '--cflags', '--libs', 'Qt6Widgets'], text=True).split()], check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-native-data-drag-') as directory:
        root = Path(directory)
        children = []
        token = root / 'token'
        token.write_text('native-data-drag-' + '3' * 40)
        token.chmod(0o600)
        env = {**os.environ, 'XDG_RUNTIME_DIR': directory, 'WAYLAND_DISPLAY': 'native-data-0',
               'QT_QPA_PLATFORM': 'wayland', 'QT_WAYLAND_DISABLE_WINDOWDECORATION': '1'}
        with socket.socket() as free:
            free.bind(('127.0.0.1', 0))
            port = free.getsockname()[1]
        with open(root / 'run.log', 'w+') as log, open(root / 'app.log', 'w+') as app_log:
            def spawn(args, environment):
                child = subprocess.Popen(args, cwd=ROOT, env=environment, stdout=log, stderr=log)
                children.append(child)
                return child
            read, write = os.pipe()
            xvfb = subprocess.Popen(['Xvfb', '-displayfd', str(write), '-screen', '0', '1600x1000x24',
                '-nolisten', 'tcp'], pass_fds=[write], stdout=log, stderr=log)
            children.append(xvfb)
            os.close(write)
            with os.fdopen(read) as pipe:
                display = ':' + pipe.readline().strip()
            wine = {**os.environ, 'DISPLAY': display, 'WINEPREFIX': str(root / 'wine'), 'WINEDEBUG': '-all',
                'WINEDLLOVERRIDES': 'winemenubuilder.exe=d;mscoree,mshtml=', 'WAYBRIDGE_GDI': '1', 'WAYBRIDGE_DND_TRACE': '1'}
            try:
                subprocess.run(['wineboot', '-u'], env=wine, stdout=log, stderr=log, timeout=60, check=True)
                server = spawn([str(ROOT / 'target/debug/waybridge'), '--socket', 'native-data-0', '--listen',
                    f'127.0.0.1:{port}', '--token-file', str(token), '--codec', 'png', '--no-audio', '--no-xwayland'], env)
                wait_for(lambda: (root / 'native-data-0').exists())
                app = subprocess.Popen([str(executable)], env=env, stdout=app_log, stderr=app_log)
                children.append(app)
                client = spawn(['wine', str(ROOT / 'build/windows/waybridge-client.exe'), '127.0.0.1', str(port), token.read_text()], wine)
                # Probe exits3 until both actual RemoteWindow handles exist.
                for mode in ('internal', 'return', 'out', 'in'):
                    deadline = time.monotonic() + 30
                    while True:
                        before = (root / 'app.log').read_text().count('OTHER ')
                        result = subprocess.run(['wine', str(output), mode], env=wine, capture_output=True, text=True, timeout=25)
                        log.write(result.stdout + result.stderr)
                        log.flush()
                        if result.returncode != 3 or time.monotonic() >= deadline:
                            break
                        time.sleep(.1)
                    assert result.returncode == 0, (mode, result.stdout, result.stderr)
                    if mode in ('internal', 'return', 'in'):
                        wait_for(lambda: (root / 'app.log').read_text().count('OTHER ') > before, 5)
                    if mode == 'in':
                        assert 'Windows 你好' in (root / 'app.log').read_text()
                    assert server.poll() is None and app.poll() is None and client.poll() is None
                print('PASS: production viewer native capture between Qt windows, OLE-return, Unicode OLE out/in')
            except Exception:
                print((root / 'run.log').read_text())
                print((root / 'app.log').read_text())
                raise
            finally:
                subprocess.run(['wineserver', '-k'], env=wine, stdout=log, stderr=log, timeout=10)
                for child in reversed(children):
                    if child.poll() is None:
                        child.terminate()
                for child in children:
                    try:
                        child.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        child.kill()
                        child.wait()


if __name__ == '__main__':
    main()
