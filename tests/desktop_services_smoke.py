#!/usr/bin/env python3
"""Real shared user wallet/portal services, rendered and operated through Waybridge.

Only temporary XDG directories and a fixture wallet request are used. No host
wallet APIs, credentials, desktop activation environment or profiles are read.
"""
import base64
import json
import os
import queue
from pathlib import Path
import selectors
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import unquote, urlparse

from smoke import ROOT, Viewer
from session_fixture import SessionFixture


def client():
    import gi
    gi.require_version('Gio', '2.0')
    from gi.repository import Gio, GLib
    connection = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    write_lock = threading.Lock()

    def emit(value):
        with write_lock:
            print(json.dumps(value), flush=True)

    def received(conn, sender, path, interface, member, parameters, unused):
        emit(dict(event=member, path=path, value=parameters.unpack()))

    for interface, member in [('org.freedesktop.portal.Request', 'Response'),
                              ('org.freedesktop.Secret.Prompt', 'Completed')]:
        connection.signal_subscribe(None, interface, member, None, None,
                                    Gio.DBusSignalFlags.NONE, received, None)
    loop = GLib.MainLoop()
    threading.Thread(target=loop.run, daemon=True).start()

    def call(destination, path, interface, method, parameters):
        return connection.call_sync(destination, path, interface, method, parameters,
                                    None, Gio.DBusCallFlags.NONE, 15000, None)

    emit(dict(event='environment', values={key: os.environ.get(key) for key in
              ['XDG_CURRENT_DESKTOP', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME',
               'KDE_SESSION_VERSION', 'DBUS_SESSION_BUS_ADDRESS',
               'WAYBRIDGE_DESKTOP_SERVICES']}))
    for line in sys.stdin:
        request = json.loads(line)
        if request['op'] == 'quit':
            break
        if request['op'] == 'file':
            result = call('org.freedesktop.portal.Desktop', '/org/freedesktop/portal/desktop',
                          'org.freedesktop.portal.FileChooser', 'OpenFile',
                          GLib.Variant('(ssa{sv})', ('', 'Waybridge fixture file picker',
                              {'modal': GLib.Variant('b', False),
                               'handle_token': GLib.Variant('s', 'waybridge_fixture')})))
            emit(dict(event='file', path=result.unpack()[0]))
        elif request['op'] == 'wallet':
            result = call('org.freedesktop.secrets', '/org/freedesktop/secrets',
                          'org.freedesktop.Secret.Service', 'CreateCollection',
                          GLib.Variant('(a{sv}s)',
                              ({'org.freedesktop.Secret.Collection.Label':
                                GLib.Variant('s', 'Waybridge fixture wallet')}, '')))
            collection, prompt = result.unpack()
            assert prompt != '/', 'empty fixture wallet should require native interaction'
            call('org.freedesktop.secrets', prompt, 'org.freedesktop.Secret.Prompt',
                 'Prompt', GLib.Variant('(s)', ('',)))
            emit(dict(event='wallet', path=prompt, collection=collection))
        elif request['op'] == 'names':
            names = call('org.freedesktop.DBus', '/org/freedesktop/DBus',
                         'org.freedesktop.DBus', 'ListNames', None).unpack()[0]
            emit(dict(event='names', names=names))
        else:
            raise AssertionError(request)
    loop.quit()
    connection.close_sync(None)


def wait(predicate, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.03)
    raise AssertionError('desktop services timed out')


def main():
    if '--client' in sys.argv:
        client()
        return
    (ROOT / 'build/tests').mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='wb-desktop-') as directory, SessionFixture(Path(directory) / ("user % quote's $" if '--special-runtime' in sys.argv else 'user')) as user:
        fixture_root = Path(directory)
        runtime = Path(user.env['XDG_RUNTIME_DIR'])
        socket_name = f'desktop-fixture-{os.getpid()}'
        app_data = Path(user.env['XDG_DATA_HOME'])
        config = Path(user.env['XDG_CONFIG_HOME'])
        # Fixture-only preferences avoid the first-use wizard; real user config
        # is never edited by Waybridge.
        (config / 'kwalletrc').write_text('[Wallet]\nEnabled=true\nFirst Use=false\n[KSecretD]\nEnabled=true\n')
        # Explicit fixture component selection, using the same static Portal
        # configuration and standard D-Bus service format a user can install.
        portal = config / 'xdg-desktop-portal'
        portal.mkdir()
        (portal / 'waybridge-portals.conf').write_bytes((ROOT / 'integration/xdg-desktop-portal/waybridge-portals.conf').read_bytes())
        catalog = app_data / 'dbus-1/services'
        catalog.mkdir(parents=True, exist_ok=True)
        (catalog / 'org.freedesktop.secrets.service').write_text('[D-BUS Service]\nName=org.freedesktop.secrets\nExec=/usr/bin/ksecretd\n')
        subprocess.run(['gdbus', 'call', '--address', user.env['DBUS_SESSION_BUS_ADDRESS'], '--dest',
                        'org.freedesktop.DBus', '--object-path', '/org/freedesktop/DBus',
                        '--method', 'org.freedesktop.DBus.ReloadConfig'], check=True, capture_output=True)
        env = {**user.env, 'KDE_SESSION_VERSION': '999'}
        token = 'desktop-test-' + '4' * 40
        token_path = fixture_root / 'token'
        token_path.write_text(token)
        token_path.chmod(0o600)
        fixture = fixture_root / '测试 文档.txt'
        fixture.write_text('Waybridge native portal fixture\n')
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        log_name = 'desktop-services-special-runtime.log' if '--special-runtime' in sys.argv else 'desktop-services-host-runtime.log' if '--host-runtime' in sys.argv else 'desktop-services.log'
        log_path = ROOT / 'build/tests' / log_name
        with log_path.open('w') as log:
            host = subprocess.Popen([str(ROOT / 'target/debug/waybridge'), '--socket', socket_name,
                '--listen', f'127.0.0.1:{port}', '--token-file', str(token_path),
                '--codec', 'png', '--no-audio', '--no-xwayland'],
                env=env, stdout=log, stderr=log)
            helper = viewer = second = uncooperative = None
            try:
                descriptor = runtime / f'{socket_name}.session.json'
                wait(lambda: descriptor.exists() or host.poll() is not None)
                assert host.poll() is None, log_path
                saved = json.loads(descriptor.read_text())
                status_file = Path(saved['environment']['WAYBRIDGE_DESKTOP_SERVICES'])

                status = json.loads(status_file.read_text())
                assert status['version'] == 3 and status['activation_environment']['dbus_unset'] is False
                assert all(item['status'] != 'ready' for item in status['services']
                           if item['name'] in ('screencast', 'flatpak-documents'))
                assert saved['environment'].get('XDG_CONFIG_DIRS') is None, 'runtime must not inject Portal config search paths'
                viewer = Viewer(port, token, dpi=168)
                helper = subprocess.Popen([str(ROOT / 'target/debug/waybridge'), '--socket',
                    socket_name, '--launch', '--', sys.executable, str(Path(__file__).resolve()),
                    '--client'], env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                    stderr=log, text=True, bufsize=1)
                pending = []
                output_queue = queue.Queue()
                def read_output():
                    for line in helper.stdout:
                        output_queue.put(json.loads(line))
                    output_queue.put(None)
                threading.Thread(target=read_output, daemon=True).start()

                def message(event, seconds=20):
                    deadline = time.monotonic() + seconds
                    while time.monotonic() < deadline:
                        for index, item in enumerate(pending):
                            if item['event'] == event:
                                return pending.pop(index)
                        try:
                            item = output_queue.get(timeout=max(.01, deadline-time.monotonic()))
                        except queue.Empty:
                            continue
                        assert item is not None, f'portal client exited: {helper.poll()}; see {log_path}'
                        if item['event'] == event:
                            return item
                        pending.append(item)
                    raise AssertionError(f'missing client event {event}; see {log_path}')

                def request(op):
                    helper.stdin.write(json.dumps(dict(op=op)) + '\n')
                    helper.stdin.flush()

                inherited = message('environment')['values']
                assert inherited['XDG_CURRENT_DESKTOP'] == 'Waybridge'
                assert inherited['KDE_SESSION_VERSION'] is None
                assert inherited['XDG_CONFIG_HOME'] == str(config)
                assert inherited['XDG_DATA_HOME'] == str(app_data)
                assert inherited['DBUS_SESSION_BUS_ADDRESS'] == env['DBUS_SESSION_BUS_ADDRESS']
                request('names')
                names = message('names')['names']
                assert 'org.freedesktop.secrets' not in names, 'core proactively activated a wallet'
                assert 'org.kde.KWin' not in names and 'org.kde.plasmashell' not in names

                request('file')
                picker_request = message('file')['path']
                picker = viewer.until(lambda kind, value: kind == 'control' and
                    value.get('type') == 'window' and 'fixture file picker' in value.get('title', ''))
                frame = viewer.until(lambda kind, value: kind == 'frame' and value[0] == picker['id'])
                def settle(name):
                    nonlocal frame
                    deadline = time.monotonic() + .6
                    while time.monotonic() < deadline:
                        selector = selectors.DefaultSelector()
                        selector.register(viewer.sock, selectors.EVENT_READ)
                        readable = selector.select(.05)
                        selector.close()
                        if readable:
                            kind, value = viewer.receive()
                            if kind == 'frame' and value[0] == picker['id']:
                                frame = value
                    frame[-1].save(ROOT / f'build/tests/desktop-picker-{name}.png')
                settle('initial')
                viewer.send(type='focus', id=picker['id'], focused=True)

                def key(code, down):
                    viewer.send(type='key', id=picker['id'], key=code, down=down)

                key(29, True); key(38, True); key(38, False); key(29, False)
                settle('location')
                viewer.send(type='clipboard', items={'text/plain;charset=utf-8':
                    base64.b64encode(str(fixture).encode()).decode()})
                time.sleep(.15)
                key(29, True); key(47, True); key(47, False); key(29, False)
                settle('pasted')
                key(28, True); key(28, False)
                response = message('Response')
                assert response['path'] == picker_request, response
                assert response['value'][0] == 0, response
                uris = response['value'][1]['uris']
                assert unquote(urlparse(uris[0]).path) == str(fixture), uris
                print('PASS: native GTK Portal picker paints remotely and returns selected Unicode fixture through actual viewer keyboard/clipboard input')

                for attempt in range(2):
                    if attempt:
                        address = inherited['DBUS_SESSION_BUS_ADDRESS']
                        pid_text = subprocess.check_output(['gdbus', 'call', '--address', address,
                            '--dest', 'org.freedesktop.DBus', '--object-path', '/org/freedesktop/DBus',
                            '--method', 'org.freedesktop.DBus.GetConnectionUnixProcessID',
                            'org.freedesktop.secrets'], text=True)
                        service_pid = int(pid_text.strip().removeprefix('(uint32 ').removesuffix(',)'))
                        os.kill(service_pid, signal.SIGTERM)
                        def owner_gone():
                            result = subprocess.run(['gdbus', 'call', '--address', address,
                                '--dest', 'org.freedesktop.DBus', '--object-path', '/org/freedesktop/DBus',
                                '--method', 'org.freedesktop.DBus.GetNameOwner', 'org.freedesktop.secrets'],
                                capture_output=True)
                            return result.returncode != 0
                        wait(owner_gone)
                        # The next standard interface call activates the fixture's explicitly selected provider.
                    request('wallet')
                    wallet_request = message('wallet')['path']
                    wallet = viewer.until(lambda kind, value: kind == 'control' and
                        value.get('type') == 'window' and ('wallet' in
                        (value.get('app_id', '') + value.get('title', '')).lower() or
                        'ksecretd' in value.get('app_id', '').lower()))
                    frame = viewer.until(lambda kind, value: kind == 'frame' and value[0] == wallet['id'])
                    frame[-1].save(ROOT / f'build/tests/desktop-wallet-{attempt}.png')
                    viewer.send(type='close', id=wallet['id'])
                    cancelled = message('Completed')
                    assert cancelled['path'] == wallet_request and cancelled['value'][0], cancelled
                print('PASS: native fixture-wallet prompts paint and cancel remotely; standard user activation restarts the wallet after provider termination')
                owner = subprocess.check_output(['gdbus', 'call', '--address', inherited['DBUS_SESSION_BUS_ADDRESS'],
                    '--dest', 'org.freedesktop.DBus', '--object-path', '/org/freedesktop/DBus',
                    '--method', 'org.freedesktop.DBus.GetConnectionUnixProcessID', 'org.freedesktop.secrets'], text=True)
                wallet_pid = int(owner.strip().removeprefix('(uint32 ').removesuffix(',)'))
                wallet_environment = dict(item.split(b'=', 1) for item in Path(f'/proc/{wallet_pid}/environ').read_bytes().split(b'\0') if b'=' in item)
                assert wallet_environment[b'XDG_DATA_HOME'].decode() == str(app_data)
                assert wallet_environment[b'XDG_CONFIG_HOME'].decode() == str(config)
                uncooperative = subprocess.Popen(['sleep', '60'], start_new_session=True)
                request('quit')
                helper.wait(timeout=5)
                assert helper.returncode == 0
                host.terminate(); host.wait(timeout=8)
                assert uncooperative.poll() is None, 'unrelated user process was terminated'
                assert not descriptor.exists() and not status_file.exists()
                assert not (app_data / 'waybridge/sessions').exists()
                assert json.loads(user.manager.read_text()) == {'KEEP_ME': 'unchanged'}
                print('PASS: wallet uses normal user paths/bus, Portal selection renders remotely, stale display environment removed, unrelated background process preserved')
            finally:
                if helper is not None and helper.poll() is None:
                    helper.terminate(); helper.wait(timeout=5)
                if viewer is not None:
                    viewer.sock.close()
                if host.poll() is None:
                    host.terminate(); host.wait(timeout=8)
                if second is not None and second.poll() is None:
                    second.terminate(); second.wait(timeout=8)
                if uncooperative is not None and uncooperative.poll() is None:
                    uncooperative.kill(); uncooperative.wait(timeout=3)


if __name__ == '__main__':
    main()
