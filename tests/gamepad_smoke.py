#!/usr/bin/env python3
"""Authenticated compositor snapshots -> only our disposable uinput/evdev node."""
import errno
import fcntl
import os
from pathlib import Path
import re
import shlex
import socket
import struct
import subprocess
import tempfile
import time
from smoke import ROOT, Viewer
from touch_smoke import wait


class Unavailable(Exception):
    pass


def ioctl_read(kind, number, size):
    return (2 << 30) | (size << 16) | (ord(kind) << 8) | number


def build_app(directory):
    protocols = {
        'xdg-shell': 'stable/xdg-shell/xdg-shell.xml',
        'xdg-decoration': 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml',
        'cursor-shape': 'staging/cursor-shape/cursor-shape-v1.xml',
        'tablet-v2': 'stable/tablet/tablet-v2.xml',
        'xdg-dialog': 'staging/xdg-dialog/xdg-dialog-v1.xml',
        'xdg-activation': 'staging/xdg-activation/xdg-activation-v1.xml',
    }
    generated = []
    for name, xml in protocols.items():
        source = Path('/usr/share/wayland-protocols') / xml
        code = directory / (name + '-protocol.c')
        subprocess.run(['wayland-scanner', 'client-header', str(source),
                        str(directory / (name + '-client-protocol.h'))], check=True)
        subprocess.run(['wayland-scanner', 'private-code', str(source), str(code)], check=True)
        generated.append(str(code))
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs',
                                                'wayland-client'], text=True))
    executable = directory / 'gamepad-app'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(directory),
                    str(ROOT / 'tests/test_app.c'), *generated, '-o', str(executable), *flags],
                   check=True)
    return executable


class GamepadViewer(Viewer):
    def __init__(self, port, token, enabled=False):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=8)
        self.geometry, self.state, self.controls = {}, {}, []
        self.send(type='hello', version=3, token=token, gamepad_v1=True,
                  gamepad_enabled=enabled, gamepad_reconnect_v1=True, window_semantics_v1=True)
        hello = self.receive()[1]
        assert hello['type'] == 'hello' and hello['gamepad_v1'] and hello['gamepad_reconnect_v1'], hello

    def receive(self):
        kind, value = super().receive()
        if kind == 'control':
            self.controls.append(value)
            if value.get('type') == 'gamepad_state':
                self.state = value
        return kind, value

    def barrier(self):
        nonce = time.monotonic_ns()
        self.send(type='ping', nonce=nonce)
        self.until(lambda k, v: k == 'control' and v.get('type') == 'pong' and v['nonce'] == nonce)

    def wait_state(self, predicate):
        if not predicate(self.state):
            self.until(lambda k, v: k == 'control' and v.get('type') == 'gamepad_state' and predicate(v))
        return self.state

    def lease(self, window):
        self.send(type='gamepad_focus', id=window)
        self.send(type='focus', id=window, focused=True)
        self.barrier()
        return dict(self.wait_state(lambda v: v.get('enabled') and v.get('id') == window))

    def sample(self, lease, incarnation, sequence, **changes):
        value = dict(type='gamepad', id=lease['id'], generation=lease['generation'],
                     slot=0, connected=True, device_generation=incarnation, sequence=sequence,
                     buttons=0x1001, lx=12345, ly=32767, rx=-30000, ry=-32768, lt=80, rt=255)
        self.send(**{**value, **changes})

    def device(self, incarnation, slot=0):
        self.barrier()
        def ready():
            return next((d for d in self.state.get('devices', [])
                         if d['slot'] == slot and d['device_generation'] == incarnation), None)
        if not ready():
            self.until(lambda k, v: k == 'control' and v.get('type') == 'gamepad_state'
                       and any(d['slot'] == slot and d['device_generation'] == incarnation
                               for d in v.get('devices', [])))
        return ready()


def open_own(device, host_pid):
    sysname = device['sysname']
    assert re.fullmatch(r'input[0-9]+', sysname), device
    directory = Path('/sys/devices/virtual/input') / sysname
    assert directory.joinpath('name').read_text().strip() == device['name']
    assert device['name'].startswith(f'Waybridge Gamepad {device["slot"] + 1} [{host_pid}-'), device
    assert directory.joinpath('phys').read_text().startswith(f'waybridge/{host_pid}/'), device
    deadline = time.monotonic() + 3
    denied = False
    while time.monotonic() < deadline:
        for entry in directory.glob('event*'):
            try:
                return os.open('/dev/input/' + entry.name, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC), directory
            except PermissionError:
                # udev creates the node before applying its group/ACL. Retry
                # only this already-identified node, never change permissions.
                denied = True
            except FileNotFoundError:
                pass
        time.sleep(.01)
    raise Unavailable('no read access to own virtual evdev node' if denied else
                      'own virtual evdev node did not become available')


def axis(fd, code):
    data = bytearray(24)
    fcntl.ioctl(fd, ioctl_read('E', 0x40 + code, len(data)), data)
    return struct.unpack('=6i', data)[0]


def buttons(fd):
    data = bytearray(96)
    fcntl.ioctl(fd, ioctl_read('E', 0x18, len(data)), data)
    return data


def held(fd, code):
    data = buttons(fd)
    return bool(data[code // 8] & (1 << (code % 8)))


def drain(fd):
    event = struct.Struct('@llHHi')
    result = []
    while True:
        try:
            data = os.read(fd, event.size * 64)
        except BlockingIOError:
            return result
        assert len(data) % event.size == 0
        for offset in range(0, len(data), event.size):
            _, _, kind, code, value = event.unpack_from(data, offset)
            result.append((kind, code, value))


def removed(directory, fd):
    wait(lambda: not directory.exists())
    try:
        os.read(fd, 24)
    except OSError as error:
        assert error.errno == errno.ENODEV, error
    else:
        raise AssertionError('removed virtual evdev node remained readable')


def run():
    try:
        probe = os.open('/dev/uinput', os.O_RDWR | os.O_NONBLOCK | os.O_CLOEXEC)
        try:
            version = bytearray(4)
            fcntl.ioctl(probe, ioctl_read('U', 45, 4), version)
            if struct.unpack('=I', version)[0] < 5:
                raise Unavailable('uinput version < 5')
        finally:
            os.close(probe)
    except OSError as error:
        raise Unavailable(f'/dev/uinput unavailable: {error}') from error
    with tempfile.TemporaryDirectory(prefix='waybridge-gamepad-e2e-') as temporary:
        root = Path(temporary)
        fixture = build_app(root)
        runtime = root / 'runtime'
        runtime.mkdir(mode=0o700)
        env = {**os.environ, 'XDG_RUNTIME_DIR': str(runtime), 'WAYLAND_DISPLAY': 'gamepad-0'}
        token = 'gamepad-e2e-' + 'f' * 40
        secret = root / 'token'
        secret.write_text(token)
        secret.chmod(0o600)
        with socket.socket() as free:
            free.bind(('127.0.0.1', 0))
            port = free.getsockname()[1]
        with (ROOT / 'build/tests/gamepad-host.log').open('w') as host_log, \
                (ROOT / 'build/tests/gamepad-app.log').open('w') as app_log:
            host = subprocess.Popen([str(ROOT / 'target/debug/waybridge'), '--codec', 'png',
                                     '--no-audio', '--no-xwayland', '--socket', 'gamepad-0',
                                     '--listen', f'127.0.0.1:{port}', '--token-file', str(secret)],
                                    env=env, stdout=host_log, stderr=host_log)
            app = viewer = None
            readers = []
            try:
                wait(lambda: (runtime / 'gamepad-0').exists() or host.poll() is not None)
                assert host.poll() is None, Path(host_log.name).read_text()
                viewer = GamepadViewer(port, token)
                app = subprocess.Popen([str(fixture), 'two'], env=env, stdout=app_log, stderr=app_log)
                windows = {}
                while len(windows) < 2:
                    kind, value = viewer.receive()
                    if kind == 'control' and value.get('type') == 'window':
                        windows[value['title']] = value['id']
                first, other = windows['Waybridge fixture 1'], windows['Waybridge fixture 2']
                viewer.barrier()
                if not viewer.state.get('supported'):
                    raise Unavailable(viewer.state.get('error', 'server uinput backend unavailable'))
                assert viewer.state.get('requested') is False and not viewer.state['enabled']
                viewer.sample({**viewer.state, 'id': first}, 10, 1)
                viewer.barrier()
                assert viewer.state['devices'] == [], 'default-OFF snapshot created a device'
                # Even an enabled preference cannot bypass local/server focus matching.
                viewer.send(type='gamepad_enabled', enabled=True)
                viewer.send(type='gamepad_focus', id=first)
                viewer.send(type='focus', id=other, focused=True)
                viewer.barrier()
                assert not viewer.state['enabled'] and not viewer.state['devices'], viewer.state
                viewer.sample({**viewer.state, 'id': first}, 10, 2)
                viewer.barrier()
                assert viewer.state['devices'] == [], 'mismatched-focus snapshot created a device'
                lease = viewer.lease(first)
                viewer.sample(lease, 11, 1)
                first_device = viewer.device(11)
                fd, directory = open_own(first_device, host.pid)
                readers.append(fd)
                viewer.sample(lease, 21, 1, slot=1)
                other_device = viewer.device(21, 1)
                other_fd, other_directory = open_own(other_device, host.pid)
                readers.append(other_fd)
                assert held(fd, 0x130) and axis(fd, 0) == 12345 and axis(fd, 1) == -32767
                assert axis(fd, 4) == 32767 and axis(fd, 17) == -1
                drain(fd)
                viewer.sample(lease, 11, 2, buttons=0x2000, lx=-123)
                viewer.barrier()
                events = drain(fd)
                assert (1, 0x130, 0) in events and (1, 0x131, 1) in events and (3, 0, -123) in events, events
                assert events[-1] == (0, 0, 0), events
                for changes in (dict(generation=lease['generation'] + 99), dict(id=other),
                                dict(slot=4), dict(buttons=0x0400), dict(lt=256)):
                    viewer.sample(lease, 11, 3, **changes)
                viewer.sample(lease, 11, 1)
                viewer.barrier()
                assert drain(fd) == [] and held(fd, 0x131), 'invalid/stale input changed evdev'
                viewer.send(type='gamepad_focus', id=0)
                viewer.barrier()
                assert not viewer.state['enabled'] and viewer.state['devices'] == [first_device, other_device]
                assert not any(buttons(fd)) and all(axis(fd, code) == 0 for code in (0, 1, 2, 3, 4, 5, 16, 17))
                assert not any(buttons(other_fd)), 'the second slot retained a held button while blurred'
                assert any(v.get('type') == 'gamepad_rumble' and v.get('slot') == 0
                           and v.get('device_generation') == 11 and v.get('strong') == 0
                           and v.get('weak') == 0 for v in viewer.controls)
                viewer.sample(lease, 21, 2, slot=1, connected=False)
                viewer.barrier()
                removed(other_directory, other_fd)
                viewer.wait_state(lambda v: v['devices'] == [first_device])
                newer = viewer.lease(other)
                assert newer['generation'] > lease['generation']
                viewer.sample(lease, 11, 2, connected=False)
                viewer.sample(newer, 11, 2)
                viewer.sample(lease, 11, 3)
                viewer.barrier()
                assert viewer.device(11) == first_device and directory.exists(), 'focus switch changed device identity'
                assert not any(buttons(fd)), 'old or repeated input was applied after refocus'
                viewer.sample(newer, 11, 3)
                viewer.barrier()
                assert held(fd, 0x130)
                viewer.send(type='gamepad_focus', id=0)
                viewer.send(type='gamepad_enabled', enabled=False)
                viewer.barrier()
                removed(directory, fd)
                viewer.wait_state(lambda v: not v['requested'] and not v['enabled'] and not v['devices'])
                viewer.send(type='gamepad_enabled', enabled=True)
                restored = viewer.lease(first)
                viewer.sample(restored, 12, 1)
                fd, directory = open_own(viewer.device(12), host.pid)
                readers.append(fd)
                assert held(fd, 0x130)
                viewer.send(type='gamepad_focus', id=0)
                viewer.send(type='stream_config', request_id=81, codec='png', fps=30,
                            bitrate=12000, read_only=True)
                viewer.barrier()
                removed(directory, fd)
                viewer.wait_state(lambda v: v['requested'] and not v['enabled'] and not v['devices'])
                viewer.sample(restored, 12, 2)
                viewer.barrier()
                assert viewer.state['devices'] == [], 'read-only snapshot created a device'
                viewer.send(type='stream_config', request_id=82, codec='png', fps=30,
                            bitrate=12000, read_only=False)
                final = viewer.lease(first)
                viewer.sample(final, 13, 1)
                fd, directory = open_own(viewer.device(13), host.pid)
                readers.append(fd)
                assert held(fd, 0x130)
                viewer.send(type='gamepad_focus', id=0)
                viewer.barrier()
                assert directory.exists() and not any(buttons(fd))
                viewer.sock.close()
                removed(directory, fd)
                viewer = GamepadViewer(port, token, enabled=True)
                reconnected = viewer.lease(first)
                viewer.sample(reconnected, 13, 1)
                new_device = viewer.device(13)
                assert new_device['sysname'] != directory.name
                fd, directory = open_own(new_device, host.pid)
                readers.append(fd)
                viewer.sample(final, 13, 99, connected=False)
                viewer.barrier()
                assert directory.exists() and held(fd, 0x130), 'prior connection detached a new device'
                viewer.sample(reconnected, 31, 1, slot=1)
                other_device = viewer.device(31, 1)
                other_fd, other_directory = open_own(other_device, host.pid)
                readers.append(other_fd)
                viewer.sample(reconnected, 13, 2, connected=False, suspended=True)
                assert viewer.device(13)['reconnecting']
                assert directory.exists() and not any(buttons(fd))
                assert other_directory.exists() and held(other_fd, 0x130)
                viewer.sample(reconnected, 13, 3)
                viewer.barrier()
                assert not any(buttons(fd)), 'suspended generation still admitted input'
                viewer.sample(reconnected, 14, 1, reconnect_from=13)
                resumed = viewer.device(14)
                assert not resumed['reconnecting'] and resumed['sysname'] == directory.name
                assert held(fd, 0x130) and viewer.device(31, 1) == other_device
                viewer.sock.close()
                removed(directory, fd)
                removed(other_directory, other_fd)
                assert host.poll() is None and app.poll() is None
                print('PASS: authenticated compositor -> own evdev buttons/axes/hat; opt-in, '
                      'focus-neutral stable slots, stale rejection, zero rumble, blur unplug '
                      'short reconnect retaining dual handles, and cleanup on disable/read-only/disconnect')
            finally:
                if viewer:
                    viewer.sock.close()
                for fd in readers:
                    os.close(fd)
                if app and app.poll() is None:
                    app.terminate()
                    app.wait(timeout=5)
                if host.poll() is None:
                    host.terminate()
                    host.wait(timeout=5)


def main():
    try:
        run()
    except Unavailable as error:
        print('SKIP: gamepad protocol/uinput end-to-end: ' + str(error))


if __name__ == '__main__':
    main()
