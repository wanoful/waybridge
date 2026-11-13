#!/usr/bin/env python3
"""Real compositor entrypoints against disposable login/user-service fixtures."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from urllib.parse import unquote
from session_fixture import ROOT, SessionFixture

BINARY = ROOT / 'target/debug/waybridge'


def wait(predicate):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.03)
    raise AssertionError('session admission timed out')


def main():
    with tempfile.TemporaryDirectory(prefix='wb-admission-') as directory, SessionFixture(directory) as fixture:
        def check():
            return json.loads(subprocess.check_output([BINARY, '--check-session'], env=fixture.env, text=True))
        for kind in ('wayland', 'x11'):
            for state in ('active', 'online'):
                fixture.set_sessions([dict(type=kind, state=state, **{'class': 'user'})])
                assert not check()['available']
                rejected = subprocess.run([BINARY, '--no-audio'], env=fixture.env, capture_output=True, text=True)
                assert rejected.returncode and 'Log out' in rejected.stderr
        for entry in [dict(type='tty'), dict(type='wayland', **{'class': 'greeter'}),
                      dict(type='unspecified', **{'class': 'manager'}), dict(type='wayland', state='closing'),
                      dict(type='wayland', uid=os.getuid() + 1)]:
            fixture.set_sessions([entry])
            assert check()['available'], entry
        fixture.set_sessions([])
        assert check()['available']
        runtime = Path(fixture.env['XDG_RUNTIME_DIR'])
        log = Path(directory) / 'compositor.log'
        token = runtime / 'token'
        token.write_text('admission-' + '9' * 40)
        token.chmod(0o600)
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            port = listener.getsockname()[1]
        with log.open('w') as output:
            host = subprocess.Popen([BINARY, '--socket', 'admission', '--listen', f'127.0.0.1:{port}',
                                     '--no-audio', '--no-xwayland', '--codec', 'png', '--token-file', token],
                                    env=fixture.env, stdout=output, stderr=output)
            try:
                descriptor = runtime / 'admission.session.json'
                wait(lambda: descriptor.exists() or host.poll() is not None)
                assert host.poll() is None, log.read_text()
                saved = json.loads(descriptor.read_text())
                assert saved['environment']['DBUS_SESSION_BUS_ADDRESS'] == fixture.env['DBUS_SESSION_BUS_ADDRESS']
                assert all(saved['environment'][key] == fixture.env[key] for key in ('XDG_DATA_HOME', 'XDG_CONFIG_HOME'))
                before_manager = json.loads(fixture.manager.read_text())
                assert before_manager['WAYLAND_DISPLAY'] == 'admission' and before_manager['KEEP_ME'] == 'unchanged'
                assert 'DISPLAY' not in before_manager, 'unset DISPLAY must remain genuinely unset'
                assert 'QT_QPA_PLATFORM' not in before_manager and 'GDK_BACKEND' not in before_manager
                assert 'PULSE_SINK' not in before_manager and 'XDG_CONFIG_HOME' not in before_manager
                assert not check()['available']
                duplicate = subprocess.run([BINARY, '--socket', 'other', '--no-audio'], env=fixture.env, capture_output=True, text=True)
                assert duplicate.returncode and 'already running or starting' in duplicate.stderr
                # A later local login does not interfere with reconnect/launch.
                fixture.set_sessions([dict(type='wayland')])
                snapshot = subprocess.check_output([BINARY, '--socket', 'admission', '--launch', '--',
                    sys.executable, '-c', 'import os;print(os.environ["WAYLAND_DISPLAY"])'], env=fixture.env, text=True)
                assert snapshot.strip() == 'admission'
                request = dict(op='ensure', session='admission', mode='attach')
                attached = json.loads(subprocess.check_output([sys.executable, ROOT / 'scripts/session-control.py'],
                    input=json.dumps(request) + '\n', env=fixture.env, text=True))
                assert attached['ok'] and not attached['result']['created'] and attached['result']['pid'] == host.pid
                request.update(session='another-name', mode='auto')
                attached = json.loads(subprocess.check_output([sys.executable, ROOT / 'scripts/session-control.py'],
                    input=json.dumps(request) + '\n', env=fixture.env, text=True))
                assert attached['ok'] and attached['result']['name'] == 'admission'
                request['mode'] = 'new'
                denied = json.loads(subprocess.check_output([sys.executable, ROOT / 'scripts/session-control.py'],
                    input=json.dumps(request) + '\n', env=fixture.env, text=True))
                assert not denied['ok']
            finally:
                host.terminate()
                host.wait(timeout=10)
            restored = json.loads(fixture.manager.read_text())
            assert restored == {'KEEP_ME': 'unchanged'}, restored
            assert not descriptor.exists()
            fixture.set_sessions([])
            assert check()['available'], 'lock was not released after exit'
            # A later desktop owns its environment; closing Waybridge must not
            # overwrite that environment with the pre-Waybridge snapshot.
            with log.open('a') as output:
                host = subprocess.Popen([BINARY, '--socket', 'admission', '--listen', f'127.0.0.1:{port}',
                    '--no-audio', '--no-xwayland', '--codec', 'png', '--token-file', token],
                    env=fixture.env, stdout=output, stderr=output)
                try:
                    wait(lambda: descriptor.exists() or host.poll() is not None)
                    assert host.poll() is None, log.read_text()
                    newer = json.loads(fixture.manager.read_text())
                    newer['WAYLAND_DISPLAY'] = 'later-desktop'
                    fixture.manager.write_text(json.dumps(newer))
                finally:
                    host.terminate()
                    host.wait(timeout=10)
                assert json.loads(fixture.manager.read_text()) == newer
            assert check()['available']
            # SSH need not export a bus address, and an absolute Wayland socket
            # may live outside the user's canonical runtime. The user bus still
            # comes from XDG_RUNTIME_DIR/bus, not the display's parent directory.
            address_path = fixture.env['DBUS_SESSION_BUS_ADDRESS'].split('path=', 1)[1].split(',', 1)[0]
            (runtime / 'bus').symlink_to(unquote(address_path))
            alternative = Path(directory) / 'display-runtime'
            alternative.mkdir(mode=0o700)
            absolute_socket = alternative / 'absolute'
            no_bus_address = dict(fixture.env)
            no_bus_address.pop('DBUS_SESSION_BUS_ADDRESS')
            with log.open('a') as output:
                host = subprocess.Popen([BINARY, '--socket', absolute_socket, '--listen', f'127.0.0.1:{port}',
                    '--no-audio', '--no-xwayland', '--codec', 'png', '--token-file', token],
                    env=no_bus_address, stdout=output, stderr=output)
                try:
                    absolute_descriptor = alternative / 'absolute.session.json'
                    wait(lambda: absolute_descriptor.exists() or host.poll() is not None)
                    assert host.poll() is None, log.read_text()
                    assert str(runtime) in unquote(json.loads(absolute_descriptor.read_text())['environment']['DBUS_SESSION_BUS_ADDRESS'])
                finally:
                    host.terminate()
                    host.wait(timeout=10)
            unavailable = {**fixture.env, 'DBUS_SYSTEM_BUS_ADDRESS': 'unix:path=/nonexistent-logind'}
            result = json.loads(subprocess.check_output([BINARY, '--check-session'], env=unavailable, text=True))
            assert not result['available'] and 'Cannot check' in result['error']
        print('PASS: desktop admission, background/locked sessions, ignored greeter/SSH/other UID, duplicate exclusion, attach bypass, shared user bus/data, environment restore and lock release')


if __name__ == '__main__':
    main()
