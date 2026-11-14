#!/usr/bin/env python3
"""Real systemd user-manager lifecycle, in a disposable delegated scope.

The outer manager only allocates a test scope. Its units, environment and desktop
services are untouched. The inner manager has a private bus, runtime and unit
search path; no host desktop targets are available to it.
"""
import ctypes
import ctypes.util
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

from session_fixture import ROOT, SessionFixture


def wait(predicate, timeout=25):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.03)
    raise AssertionError('real user-manager lifecycle timed out')


def component(output, status_failure=False):
    wayland = ctypes.CDLL(ctypes.util.find_library('wayland-client'))
    wayland.wl_display_connect.argtypes = [ctypes.c_char_p]
    wayland.wl_display_connect.restype = ctypes.c_void_p
    wayland.wl_display_roundtrip.argtypes = [ctypes.c_void_p]
    wayland.wl_display_disconnect.argtypes = [ctypes.c_void_p]
    def roundtrip():
        display = wayland.wl_display_connect(None)
        assert display, 'display disappeared before declared component stopped'
        assert wayland.wl_display_roundtrip(display) >= 0, 'compositor did not dispatch lifecycle roundtrip'
        wayland.wl_display_disconnect(display)
    def record(event):
        with Path(output).open('a') as file:
            file.write(json.dumps(dict(event=event, environment={key: os.environ.get(key) for key in (
                'WAYLAND_DISPLAY', 'DISPLAY', 'QT_QPA_PLATFORM', 'GDK_BACKEND', 'WAYBRIDGE_SESSION_ID')})) + '\n')
    roundtrip()
    record('ready')
    if status_failure:
        # Block both the worker's ready report and every later stop report.
        status = Path(os.environ['WAYBRIDGE_SESSION_ID']) / 'status.json'
        status.unlink()
        status.mkdir()
    address = os.environ['NOTIFY_SOCKET']
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as notification:
        notification.connect('\0' + address[1:] if address.startswith('@') else address)
        notification.sendall(b'READY=1')
    stopped = False
    def stop(*unused):
        nonlocal stopped
        stopped = True
    signal.signal(signal.SIGTERM, stop)
    while not stopped:
        time.sleep(.02)
    # A new connection in shutdown verifies the accept loop is still pumping.
    roundtrip()
    record('stopped-with-display')


def run_inner():
    with tempfile.TemporaryDirectory(prefix='wb-real-systemd-') as directory, SessionFixture(directory, real_manager=True) as fixture:
        root = Path(directory)
        units = root / 'units'
        units.mkdir()
        for source in (ROOT / 'integration/systemd/user').glob('*.target'):
            shutil.copy2(source, units / source.name)
        for name in ('default.target', 'basic.target', 'shutdown.target'):
            (units / name).write_text('[Unit]\nDescription=Isolated test ' + name + '\nDefaultDependencies=no\n')
        # The real inner manager owns its real user-bus daemon/socket. The
        # fixture's first private bus is retained solely as fake system/logind.
        user_address = 'unix:path=' + str(Path(fixture.env['XDG_RUNTIME_DIR']) / 'bus')
        fixture.env['DBUS_SESSION_BUS_ADDRESS'] = user_address
        (units / 'dbus.socket').write_text('[Unit]\nDescription=Isolated user bus socket\n[Socket]\nListenStream=%t/bus\nSocketMode=0600\n')
        (units / 'dbus.service').write_text('[Unit]\nDescription=Real isolated user bus\nRequires=dbus.socket\nAfter=dbus.socket\n[Service]\nType=notify\nNotifyAccess=main\nExecStart=/usr/bin/dbus-daemon --session --address=systemd: --nofork --nopidfile --systemd-activation\n')
        (units / 'default.target').write_text('[Unit]\nDescription=Isolated manager default target\nWants=dbus.service\n')
        record = root / 'component.jsonl'
        script = Path(__file__).resolve()
        (units / 'fixture-owned.service').write_text(
            '[Unit]\nDescription=Explicitly Waybridge-owned roundtrip fixture\n'
            'PartOf=waybridge-session.target\nBefore=waybridge-session.target\nAfter=waybridge-session-pre.target\n'
            '[Service]\nType=notify\nNotifyAccess=main\nTimeoutStartSec=10\nTimeoutStopSec=8\n'
            f'ExecStart=/usr/bin/python3 "{script}" --component "{record}"\n')
        (units / 'fixture-unrelated.service').write_text('[Unit]\nDescription=Unrelated test background task\n[Service]\nExecStart=/usr/bin/sleep 90\n')
        wants = units / 'waybridge-session.target.wants'
        wants.mkdir()
        (wants / 'fixture-owned.service').symlink_to('../fixture-owned.service')
        env = {**fixture.env, 'SYSTEMD_UNIT_PATH': str(units), 'SYSTEMD_LOG_TARGET': 'console', 'SYSTEMD_LOG_LEVEL': 'info'}
        # The private manager's inherited environment also omits toolkit defaults.
        for key in ('QT_QPA_PLATFORM', 'GDK_BACKEND', 'DISPLAY', 'WAYLAND_DISPLAY'):
            env.pop(key, None)
        log_root = ROOT / 'build/tests'
        log_root.mkdir(parents=True, exist_ok=True)
        with (log_root / 'real-systemd-manager.log').open('w') as manager_log, (log_root / 'real-systemd-compositor.log').open('w') as compositor_log:
            manager = subprocess.Popen(['/usr/lib/systemd/systemd', '--user'], env=env, stdout=manager_log, stderr=manager_log)
            host = None
            def ctl(*args, check=True):
                result = subprocess.run(['systemctl', '--user', *args], env=env, capture_output=True, text=True)
                if check:
                    assert result.returncode == 0, (args, result.stderr, (log_root / 'real-systemd-manager.log').read_text())
                return result
            try:
                wait(lambda: ctl('show-environment', check=False).returncode == 0 or manager.poll() is not None)
                assert manager.poll() is None, (log_root / 'real-systemd-manager.log').read_text()
                ctl('start', 'dbus.service')
                assert ctl('show', '-p', 'Version', '--value').stdout.strip(), 'not connected to a real systemd manager'
                # Deliberate empty and stale values test genuine unset semantics.
                ctl('set-environment', 'WAYLAND_DISPLAY=stale-nonexistent', 'DISPLAY=', 'KEEP_ME=untouched')
                ctl('start', 'fixture-unrelated.service')
                token = root / 'token'
                token.write_text('lifecycle-' + 'a' * 40)
                token.chmod(0o600)
                with socket.socket() as reservation:
                    reservation.bind(('127.0.0.1', 0))
                    port = reservation.getsockname()[1]
                def start():
                    return subprocess.Popen([str(ROOT / 'target/debug/waybridge'), '--socket', 'lifecycle',
                        '--listen', f'127.0.0.1:{port}', '--token-file', str(token), '--codec', 'png',
                        '--no-audio', '--no-xwayland'], env=env, stdout=compositor_log, stderr=compositor_log)
                host = start()
                descriptor = Path(env['XDG_RUNTIME_DIR']) / 'lifecycle.session.json'
                wait(lambda: descriptor.exists() or host.poll() is not None)
                assert host.poll() is None, (log_root / 'real-systemd-compositor.log').read_text()
                saved = json.loads(descriptor.read_text())
                status = Path(saved['environment']['WAYBRIDGE_DESKTOP_SERVICES'])
                wait(lambda: any(state['name'] == 'session-lifecycle' and state['status'] == 'ready'
                                 for state in json.loads(status.read_text())['services']))
                events = [json.loads(line) for line in record.read_text().splitlines()]
                assert events[-1]['event'] == 'ready', events
                assert events[-1]['environment']['WAYLAND_DISPLAY'] == 'lifecycle'
                assert all(events[-1]['environment'][key] is None for key in ('DISPLAY', 'QT_QPA_PLATFORM', 'GDK_BACKEND'))
                assert ctl('is-active', 'waybridge-session.target').stdout.strip() == 'active'
                # A pre-existing target must not be taken over or stopped by a
                # failed second start (the normal file lock also protects it).
                duplicate = subprocess.run([str(ROOT / 'target/debug/waybridge'), '--no-audio'], env=env, capture_output=True, text=True)
                assert duplicate.returncode and ctl('is-active', 'fixture-owned.service').stdout.strip() == 'active'
                host.terminate()
                host.wait(timeout=20)
                assert host.returncode == 0, (log_root / 'real-systemd-compositor.log').read_text()
                events = [json.loads(line) for line in record.read_text().splitlines()]
                assert [event['event'] for event in events] == ['ready', 'stopped-with-display'], events
                assert ctl('is-active', 'fixture-owned.service', check=False).stdout.strip() == 'inactive'
                assert ctl('is-active', 'fixture-unrelated.service').stdout.strip() == 'active'
                activation = ctl('show-environment').stdout.splitlines()
                assert not any(value.startswith(('WAYLAND_DISPLAY=', 'WAYBRIDGE_SESSION_ID=')) for value in activation), activation
                assert 'DISPLAY=' in activation, 'an explicitly empty prior DISPLAY must remain distinct from unset'
                assert 'KEEP_ME=untouched' in activation
                assert not descriptor.exists() and not status.exists()
                # Diagnostics are optional even after services have started.
                # Fail the ready report inside the component before READY=1,
                # and leave the failure in place during shutdown.
                owned_unit = units / 'fixture-owned.service'
                original_unit = owned_unit.read_text()
                owned_unit.write_text(original_unit.replace(f'--component "{record}"', f'--component "{record}" --status-failure'))
                ctl('daemon-reload')
                host = start()
                wait(lambda: descriptor.exists() or host.poll() is not None)
                assert host.poll() is None, (log_root / 'real-systemd-compositor.log').read_text()
                status = Path(json.loads(descriptor.read_text())['environment']['WAYBRIDGE_DESKTOP_SERVICES'])
                wait(lambda: ctl('is-active', 'waybridge-session.target', check=False).stdout.strip() == 'active')
                assert status.is_dir(), 'component did not block status persistence'
                # Ensure the ready diagnostic has actually failed before stop.
                wait(lambda: 'lifecycle=ready' in (log_root / 'real-systemd-compositor.log').read_text())
                host.terminate(); host.wait(timeout=20)
                assert host.returncode == 0, (log_root / 'real-systemd-compositor.log').read_text()
                events = [json.loads(line) for line in record.read_text().splitlines()]
                assert [event['event'] for event in events[-2:]] == ['ready', 'stopped-with-display'], events
                assert ctl('is-active', 'fixture-owned.service', check=False).stdout.strip() == 'inactive'
                assert ctl('show', 'fixture-owned.service', '-p', 'Result', '--value').stdout.strip() == 'success'
                assert ctl('is-active', 'fixture-unrelated.service').stdout.strip() == 'active'
                activation = ctl('show-environment').stdout.splitlines()
                assert not any(value.startswith(('WAYLAND_DISPLAY=', 'WAYBRIDGE_SESSION_ID=')) for value in activation), activation
                assert 'DISPLAY=' in activation and 'KEEP_ME=untouched' in activation
                assert not descriptor.exists() and not status.exists()
                owned_unit.write_text(original_unit)
                ctl('daemon-reload')
                # A subsequent desktop may replace activation state; ending
                # Waybridge must stop its own services but preserve that state.
                host = start()
                wait(lambda: descriptor.exists() or host.poll() is not None)
                assert host.poll() is None
                wait(lambda: ctl('is-active', 'waybridge-session.target', check=False).stdout.strip() == 'active')
                ctl('set-environment', 'WAYLAND_DISPLAY=later-desktop', 'DISPLAY=:567', 'XDG_CURRENT_DESKTOP=LaterDesktop')
                before = ctl('show-environment').stdout
                host.terminate(); host.wait(timeout=20)
                assert ctl('show-environment').stdout == before
                assert ctl('is-active', 'fixture-owned.service', check=False).stdout.strip() == 'inactive'
                # Validate old display addresses independently: a valid old
                # Wayland socket does not make an expired X11 DISPLAY valid.
                previous_socket = Path(env['XDG_RUNTIME_DIR']) / 'previous-wayland'
                with socket.socket(socket.AF_UNIX) as previous_display:
                    previous_display.bind(str(previous_socket))
                    previous_display.listen()
                    ctl('set-environment', 'WAYLAND_DISPLAY=' + str(previous_socket), 'DISPLAY=:987654321')
                    host = start()
                    wait(lambda: descriptor.exists() or host.poll() is not None)
                    assert host.poll() is None
                    wait(lambda: ctl('is-active', 'waybridge-session.target', check=False).stdout.strip() == 'active')
                    host.terminate(); host.wait(timeout=20)
                    activation = ctl('show-environment').stdout.splitlines()
                    assert 'WAYLAND_DISPLAY=' + str(previous_socket) in activation
                    assert not any(value.startswith('DISPLAY=') for value in activation), activation
                # A target activated by another owner must be rejected before
                # any environment mutation, even when no compositor lock exists.
                (wants / 'fixture-owned.service').unlink()
                ctl('daemon-reload')
                ctl('start', 'waybridge-session.target')
                ctl('set-environment', 'WAYBRIDGE_SESSION_ID=external-owner')
                external = ctl('show-environment').stdout
                rejected = start()
                rejected.wait(timeout=10)
                assert rejected.returncode != 0
                assert ctl('show-environment').stdout == external
                assert ctl('is-active', 'waybridge-session.target').stdout.strip() == 'active'
                ctl('stop', 'waybridge-session.target')
                # An inactive target with a pending notify dependency still
                # belongs to the earlier StartUnit transaction.
                (units / 'fixture-pending.service').write_text('[Unit]\nPartOf=waybridge-session.target\nBefore=waybridge-session.target\n[Service]\nType=notify\nExecStart=/usr/bin/sleep 30\nTimeoutStartSec=20\n')
                (wants / 'fixture-pending.service').symlink_to('../fixture-pending.service')
                ctl('daemon-reload')
                ctl('--no-block', 'start', 'waybridge-session.target')
                wait(lambda: ctl('show', 'waybridge-session.target', '-p', 'Job', '--value').stdout.strip() != '0')
                pending_job = ctl('show', 'waybridge-session.target', '-p', 'Job', '--value').stdout
                assert ctl('is-active', 'waybridge-session.target', check=False).stdout.strip() == 'inactive'
                external = ctl('show-environment').stdout
                rejected = start()
                rejected.wait(timeout=10)
                assert rejected.returncode != 0
                assert ctl('show-environment').stdout == external
                assert ctl('show', 'waybridge-session.target', '-p', 'Job', '--value').stdout == pending_job
                ctl('stop', 'waybridge-session.target')
                ctl('stop', 'fixture-pending.service')
                print('PASS: real isolated systemd user manager, notify readiness after Wayland roundtrip, declared service stop with live display despite status-write failures, no toolkit import, genuine unset, newer desktop preservation and unrelated service survival')
            finally:
                if host is not None and host.poll() is None:
                    host.terminate(); host.wait(timeout=20)
                if manager.poll() is None:
                    ctl('stop', 'fixture-owned.service', 'fixture-unrelated.service', 'fixture-pending.service', 'dbus.service', 'dbus.socket', check=False)
                    ctl('--force', 'exit', check=False)
                    try:
                        manager.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        manager.kill(); manager.wait(timeout=3)


def main():
    if '--component' in sys.argv:
        component(sys.argv[sys.argv.index('--component') + 1], '--status-failure' in sys.argv)
    elif '--inner' in sys.argv:
        run_inner()
    else:
        # Allocate only a private delegated scope from the live manager; all
        # actual service/env operations then target the new manager's bus.
        command = ['systemd-run', '--user', '--scope', '--quiet', '--property=Delegate=yes',
                   '--unit=waybridge-lifecycle-test-' + str(os.getpid()), sys.executable,
                   str(Path(__file__).resolve()), '--inner']
        raise SystemExit(subprocess.run(command).returncode)


if __name__ == '__main__':
    main()
