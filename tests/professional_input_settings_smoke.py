#!/usr/bin/env python3
"""Real hosted PE worker: pen/gamepad settings, status and old-peer compatibility.

Uses a private Wine/Xvfb desktop, disposable compositor/user bus, generated
protocols and copied PE worker. No pen or controller samples are injected.
"""
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
from session_fixture import SessionFixture
from udp_flow_smoke import DelayProxy, read_packet
from smoke import ROOT

BUILD = ROOT/'build/tests/professional-input-settings'


def wait(check, timeout=15, message='professional input settings timed out'):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(.025)
    raise AssertionError(message)


class InputRelay(DelayProxy):
    def __init__(self, address, mode):
        self.mode = mode
        self.hello = None
        self.commands = []
        self.states = []
        super().__init__(address)

    def forward(self, source, target, upstream):
        try:
            while not self.closed.is_set():
                body = read_packet(source)
                if body[0] == 0:
                    value = json.loads(body[1:])
                    kind = value.get('type', '')
                    if upstream:
                        if kind == 'hello' and not value.get('channel'):
                            self.hello = {key: value.get(key) for key in
                                          ('pen_v1', 'pen_enabled', 'gamepad_v1', 'gamepad_enabled')}
                            if self.mode == 'old-protocol':
                                for key in ('pen_v1', 'pen_enabled', 'gamepad_v1', 'gamepad_enabled'):
                                    value.pop(key, None)
                                body = b'\0' + json.dumps(value).encode()
                        if kind == 'pen_enabled' or kind.startswith('gamepad') or kind == 'pen':
                            self.commands.append(value)
                    else:
                        if kind in ('pen_state', 'gamepad_state'):
                            self.states.append(value)
                        if kind == 'hello' and value.get('codec') == 'mixed':
                            if self.mode in ('old-live', 'old-protocol'):
                                value.pop('pen_settings_v1', None)
                                value.pop('gamepad_settings_v1', None)
                            if self.mode == 'old-protocol':
                                value.pop('pen_v1', None)
                                value.pop('gamepad_v1', None)
                            body = b'\0' + json.dumps(value).encode()
                    target.sendall(struct.pack('>I', len(body)) + body)
        except (EOFError, OSError):
            pass
        except Exception as error:
            self.errors.append(error)
        finally:
            try:
                target.shutdown(socket.SHUT_WR)
            except OSError:
                pass


class HostedWorker:
    def __init__(self, session, wine, mode, label, log):
        self.relay = InputRelay(('127.0.0.1', session['port']), mode)
        self.path = BUILD/f'{label}-worker.log'
        self.events, self.errors = [], []
        self.worker = subprocess.Popen(['wine', str(BUILD/'pipe-host.exe'), 'Z:'+str(BUILD/'worker.exe'),
                                        'Z:'+str(self.path)], env=wine, stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=log, text=True)
        def read():
            try:
                for line in self.worker.stdout:
                    value = json.loads(line)
                    self.events.append(value)
            except Exception as error:
                self.errors.append(repr(error))
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()
        self.session = session
        self.label = label

    def send(self, **value):
        self.worker.stdin.write(json.dumps(value, allow_nan=False)+'\n')
        self.worker.stdin.flush()

    def until(self, check, after=0, timeout=15):
        def found():
            assert not self.errors, self.errors
            return next((value for value in self.events[after:] if check(value)), None)
        return wait(found, timeout, f'{self.label}: expected hosted event missing; events={self.events[-8:]}')

    def start(self, settings):
        self.send(type='start', version=1, host='127.0.0.1', port=self.relay.port,
                  token=self.session['token'], settings=settings)
        self.until(lambda value:value['type']=='connected')
        return self.until(lambda value:value['type']=='status' and value.get('windows'))

    def apply(self, settings):
        after = len(self.events)
        self.send(type='settings', settings=settings)
        value = self.until(lambda value:value['type']=='local_settings_applied', after)
        assert value['settings'] == settings, value
        return value

    def requested(self, pen, gamepads, after=0):
        def ready():
            states = self.relay.states[after:]
            return (any(value['type']=='pen_state' and value.get('requested') is pen for value in states) and
                    any(value['type']=='gamepad_state' and value.get('requested') is gamepads for value in states))
        wait(ready, message=f'{self.label}: server requested state missing: {self.relay.states[-8:]}')

    def close(self):
        try:
            if self.worker.poll() is None:
                self.send(type='disconnect')
                self.worker.stdin.close()
                self.worker.wait(timeout=15)
                assert self.worker.returncode == 0, self.path.read_text(errors='replace')
        finally:
            if self.worker.poll() is None:
                self.worker.terminate()
                try:
                    self.worker.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    self.worker.kill(); self.worker.wait(timeout=5)
            self.reader.join(timeout=2)
            self.relay.close()
            (BUILD/f'{self.label}-events.json').write_text(json.dumps(self.events, indent=2)+'\n')
            (BUILD/f'{self.label}-wire.json').write_text(json.dumps(dict(hello=self.relay.hello,
                commands=self.relay.commands, states=self.relay.states), indent=2)+'\n')
        assert not self.relay.errors and not self.errors, (self.relay.errors, self.errors)
        text = self.path.read_text(errors='replace') if self.path.exists() else ''
        assert self.session['token'] not in text, 'session credential leaked to worker log'


def status_shape(status):
    for field in ('pen_supported', 'pen_enabled', 'pen_native_active', 'pen_hardware_available',
                  'pen_live_supported', 'gamepad_supported', 'gamepad_enabled', 'gamepad_active',
                  'gamepad_live_supported', 'rumble_supported'):
        assert isinstance(status[field], bool), (field, status)
    assert isinstance(status['gamepad_devices'], list), status
    assert isinstance(status['pen_error'], str) and isinstance(status['gamepad_error'], str), status


def modern(session, wine, initial, log):
    label = 'default-off' if not initial else 'explicit-on'
    host = HostedWorker(session, wine, 'modern', label, log)
    try:
        settings = dict(codec='png', fps=30, bitrate=12000, audio=False, files=False, ime=False,
                        read_only=True, clipboard='off')
        if initial:
            settings.update(pen=True, pen_pressure_gamma=1.75, gamepads=True,
                            gamepad_rumble=False, gamepad_deadzone=27)
        status = host.start(settings)
        status_shape(status)
        assert status['pen_supported'] and status['pen_live_supported'] and status['gamepad_live_supported'], status
        assert status['pen_enabled'] is initial and status['gamepad_enabled'] is initial, status
        # The ordinary SHM fixture never subscribes to tablet-v2. Read-only also
        # forbids a foreground gamepad lease, independent from requested opt-in.
        assert status['pen_native_active'] is False and status['gamepad_active'] is False, status
        wait(lambda:host.relay.hello is not None)
        assert host.relay.hello == dict(pen_v1=True, pen_enabled=initial, gamepad_v1=True, gamepad_enabled=initial)
        host.requested(initial, initial)
        sequence = (False, True) if initial else (True, False, True)
        for index, enabled in enumerate(sequence):
            before = len(host.relay.states)
            settings.update(pen=enabled, gamepads=enabled, pen_pressure_gamma=(.25 if index==0 else 4),
                            gamepad_deadzone=(0 if index==0 else 50), gamepad_rumble=bool(index%2))
            applied = host.apply(settings)
            status_shape(applied)
            assert applied['pen_enabled'] is enabled and applied['gamepad_enabled'] is enabled, applied
            assert applied['gamepad_active'] is False and applied['pen_native_active'] is False, applied
            assert applied['settings']['ime'] is False and applied['settings']['read_only'] is True and applied['settings']['files'] is False
            host.requested(enabled, enabled, before)
        toggles = [value['enabled'] for value in host.relay.commands if value['type']=='pen_enabled']
        assert toggles == list(sequence), host.relay.commands
        assert [value['enabled'] for value in host.relay.commands if value['type']=='gamepad_enabled'] == toggles
        # Rejected settings must have no partial outbound toggle or local apply.
        malformed = [dict(pen='yes'), dict(gamepads=1), dict(gamepad_rumble='false'),
                     dict(pen_pressure_gamma='1'), dict(pen_pressure_gamma=.249), dict(pen_pressure_gamma=4.001),
                     dict(gamepad_deadzone=-1), dict(gamepad_deadzone=51), dict(gamepad_deadzone=1.5),
                     dict(gamepad_deadzone=2**32+15), dict(gamepad_deadzone=-(2**32)+15),
                     dict(gamepad_deadzone=2**64-1)]
        for changes in malformed:
            before = len(host.events)
            command_count = len([value for value in host.relay.commands if value['type'] in ('pen_enabled','gamepad_enabled')])
            # Flip the booleans too, so a validation-after-mutation bug is visible.
            invalid = {**settings, 'pen':False, 'gamepads':False, **changes}
            host.send(type='settings', settings=invalid)
            error = host.until(lambda value:value['type']=='error', before)
            assert any(word in error['message'].lower() for word in ('boolean','numeric','integer','range')), error
            host.send(type='status')
            unchanged = host.until(lambda value:value['type']=='status', before)
            assert unchanged['pen_enabled'] is True and unchanged['gamepad_enabled'] is True, unchanged
            assert not any(value['type']=='local_settings_applied' for value in host.events[before:]), host.events[before:]
            assert len([value for value in host.relay.commands if value['type'] in ('pen_enabled','gamepad_enabled')]) == command_count
        assert not any(value['type']=='pen' or (value['type']=='gamepad' and value.get('connected')) for value in host.relay.commands), host.relay.commands
    finally:
        host.close()
    print(f'PASS: hosted worker {label}, calibrated live pen/gamepad ON/OFF, requested-vs-active, independent read-only policy, atomic malformed setting rejection and no physical samples')


def old_peer(session, wine, mode, log):
    host = HostedWorker(session, wine, mode, mode, log)
    try:
        settings = dict(codec='png', fps=30, bitrate=12000, audio=False, files=False, read_only=True)
        status = host.start(settings)
        status_shape(status)
        assert not status['pen_enabled'] and not status['gamepad_enabled'], status
        assert not status['pen_live_supported'] and not status['gamepad_live_supported'], status
        if mode == 'old-protocol':
            assert not status['pen_supported'] and not status['gamepad_supported'], status
        before = len(host.events)
        settings.update(pen=True, gamepads=True, pen_pressure_gamma=1.5, gamepad_deadzone=21, gamepad_rumble=False)
        applied = host.apply(settings)
        assert applied['settings']['pen'] and applied['settings']['gamepads'], applied
        assert not applied['pen_enabled'] and not applied['gamepad_enabled'], applied
        host.until(lambda value:value['type']=='settings_warning' and '笔' in value.get('message',''), before)
        host.until(lambda value:value['type']=='settings_warning' and '手柄' in value.get('message',''), before)
        warnings = [value for value in host.events[before:] if value['type']=='settings_warning']
        assert all('重新连接' in value['message'] for value in warnings), warnings
        assert not any(value['type'] in ('pen_enabled','gamepad_enabled','pen','gamepad') for value in host.relay.commands), host.relay.commands
        if mode == 'old-protocol':
            assert host.relay.commands == [], host.relay.commands
    finally:
        host.close()
    print(f'PASS: hosted worker {mode} capability stripping retains saved opt-in, reports effective OFF and reconnect warnings, sends no unsupported input/toggle')


def build():
    BUILD.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT/'build/windows/waybridge-client.exe', BUILD/'worker.exe')
    protocols = {
        'xdg-shell': 'stable/xdg-shell/xdg-shell.xml',
        'xdg-decoration': 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
        'cursor-shape': 'staging/cursor-shape/cursor-shape-v1.xml',
        'tablet': 'stable/tablet/tablet-v2.xml',
        'xdg-dialog': 'staging/xdg-dialog/xdg-dialog-v1.xml',
        'xdg-activation': 'staging/xdg-activation/xdg-activation-v1.xml'}
    for name, source in protocols.items():
        xml = '/usr/share/wayland-protocols/' + source
        subprocess.run(['wayland-scanner','client-header',xml,str(BUILD/f'{name}-client-protocol.h')],check=True)
        subprocess.run(['wayland-scanner','private-code',xml,str(BUILD/f'{name}-protocol.c')],check=True)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(BUILD),'tests/test_app.c',
                    *[str(BUILD/f'{name}-protocol.c') for name in protocols], '-o', str(BUILD/'app'),
                    *subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client'],text=True).split()],cwd=ROOT,check=True)
    subprocess.run(['x86_64-w64-mingw32-g++','-municode','-static','-O2','tests/windows_control_probe.cpp',
                    '-o',str(BUILD/'pipe-host.exe')],cwd=ROOT,check=True)


def main():
    build()
    with tempfile.TemporaryDirectory(prefix='waybridge-professional-settings-') as directory, \
            SessionFixture(Path(directory)/'bus') as fixture, (BUILD/'professional-input-settings.log').open('w') as log:
        read_fd, write_fd = os.pipe()
        xvfb = subprocess.Popen(['Xvfb','-displayfd',str(write_fd),'-screen','0','1280x1024x24','-nolisten','tcp'],
                                pass_fds=[write_fd],stdout=log,stderr=log)
        os.close(write_fd)
        with os.fdopen(read_fd) as pipe:
            display = ':' + pipe.readline().strip()
        wine = {**fixture.env,'DISPLAY':display,'WINEPREFIX':str(Path(directory)/'wine'),'WINEDEBUG':'-all',
                'WINEDLLOVERRIDES':'winemenubuilder.exe=d;mscoree,mshtml=','WAYBRIDGE_GDI':'1'}
        server = app = None
        try:
            subprocess.run(['wineboot','-u'],env=wine,stdout=log,stderr=log,check=True,timeout=60)
            runtime = Path(fixture.env['XDG_RUNTIME_DIR'])
            token = 'professional-settings-' + os.urandom(24).hex()
            token_file = runtime/'token'; token_file.write_text(token); token_file.chmod(0o600)
            with socket.socket() as free:
                free.bind(('127.0.0.1',0)); port=free.getsockname()[1]
            env = {**fixture.env,'WAYLAND_DISPLAY':'professional-settings-0'}
            server = subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--socket','professional-settings-0',
                '--listen',f'127.0.0.1:{port}','--token-file',str(token_file),'--codec','png','--no-audio','--no-xwayland'],
                env=env,stdout=log,stderr=log)
            wait(lambda:(runtime/'professional-settings-0').exists() or server.poll() is not None)
            assert server.poll() is None, 'private compositor failed; inspect ' + str(log.name)
            app = subprocess.Popen([str(BUILD/'app')],env=env,stdout=log,stderr=log)
            session = dict(port=port,token=token)
            for initial in (False,True):
                modern(session,wine,initial,log)
                time.sleep(.2)
            for mode in ('old-live','old-protocol'):
                old_peer(session,wine,mode,log)
                time.sleep(.2)
            assert server.poll() is None and app.poll() is None
            print('PASS: compositor and fixture remain alive across all four hosted-worker reconnects; evidence in build/tests/professional-input-settings/')
        finally:
            for child in (app,server):
                if child and child.poll() is None:
                    child.terminate()
                    try: child.wait(timeout=8)
                    except subprocess.TimeoutExpired: child.kill(); child.wait()
            subprocess.run(['wineserver','-k'],env=wine,stdout=log,stderr=log,timeout=10)
            if xvfb.poll() is None:
                xvfb.terminate();xvfb.wait(timeout=5)


if __name__ == '__main__':
    main()
