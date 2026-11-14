#!/usr/bin/env python3
"""Error exits stop owned services while the private display still dispatches."""
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile

from session_fixture import ROOT, SessionFixture
from session_lifecycle_smoke import component, wait


def run_inner():
    binary = Path(os.environ.get('WAYBRIDGE_TEST_BINARY', ROOT / 'target/debug/waybridge'))
    shim = ROOT / 'build/tests/fail-wayland-accept.so'
    with tempfile.TemporaryDirectory(prefix='wb-error-shutdown-') as directory, SessionFixture(directory, real_manager=True) as fixture:
        root = Path(directory)
        units = root / 'units'
        units.mkdir()
        for source in (ROOT / 'integration/systemd/user').glob('*.target'):
            shutil.copy2(source, units / source.name)
        for name in ('default.target', 'basic.target', 'shutdown.target'):
            (units / name).write_text('[Unit]\nDefaultDependencies=no\n')
        fixture.env['DBUS_SESSION_BUS_ADDRESS'] = 'unix:path=' + str(Path(fixture.env['XDG_RUNTIME_DIR']) / 'bus')
        (units / 'dbus.socket').write_text('[Socket]\nListenStream=%t/bus\nSocketMode=0600\n')
        (units / 'dbus.service').write_text(
            '[Unit]\nRequires=dbus.socket\nAfter=dbus.socket\n[Service]\nType=notify\nNotifyAccess=main\n'
            'ExecStart=/usr/bin/dbus-daemon --session --address=systemd: --nofork --nopidfile --systemd-activation\n')
        (units / 'default.target').write_text('[Unit]\nWants=dbus.service\n')
        record = root / 'component.jsonl'
        (units / 'fixture-owned.service').write_text(
            '[Unit]\nPartOf=waybridge-session.target\nBefore=waybridge-session.target\nAfter=waybridge-session-pre.target\n'
            '[Service]\nType=notify\nNotifyAccess=main\nTimeoutStartSec=10\nTimeoutStopSec=8\n'
            f'ExecStart=/usr/bin/python3 "{Path(__file__).resolve()}" --component "{record}"\n')
        (units / 'fixture-unrelated.service').write_text('[Service]\nExecStart=/usr/bin/sleep 90\n')
        wants = units / 'waybridge-session.target.wants'
        wants.mkdir()
        (wants / 'fixture-owned.service').symlink_to('../fixture-owned.service')
        env = {**fixture.env, 'SYSTEMD_UNIT_PATH': str(units), 'SYSTEMD_LOG_TARGET': 'console', 'SYSTEMD_LOG_LEVEL': 'info',
               'GVFS_DISABLE_FUSE': '1'}
        for key in ('QT_QPA_PLATFORM', 'GDK_BACKEND', 'DISPLAY', 'WAYLAND_DISPLAY'):
            env.pop(key, None)
        logs = ROOT / 'build/tests'
        logs.mkdir(parents=True, exist_ok=True)
        with (logs / 'error-shutdown-manager.log').open('w') as manager_log, (logs / 'error-shutdown-compositor.log').open('w') as host_log:
            manager = subprocess.Popen(['/usr/lib/systemd/systemd', '--user'], env=env, stdout=manager_log, stderr=manager_log)
            host = None

            def ctl(*args, check=True):
                result = subprocess.run(['systemctl', '--user', *args], env=env, capture_output=True, text=True)
                if check:
                    assert result.returncode == 0, (args, result.stderr, (logs / 'error-shutdown-manager.log').read_text())
                return result

            try:
                wait(lambda: ctl('show-environment', check=False).returncode == 0 or manager.poll() is not None)
                assert manager.poll() is None, (logs / 'error-shutdown-manager.log').read_text()
                ctl('start', 'dbus.service', 'fixture-unrelated.service')
                ctl('set-environment', 'KEEP_ME=untouched')
                token = root / 'token'
                token.write_text('error-shutdown-' + 'b' * 40)
                token.chmod(0o600)
                runtime = Path(env['XDG_RUNTIME_DIR'])
                descriptor = runtime / 'error-session.session.json'
                trigger = root / 'fail-accept'
                with socket.socket() as reservation:
                    reservation.bind(('127.0.0.1', 0))
                    port = reservation.getsockname()[1]

                def start(*arguments):
                    return subprocess.Popen([str(binary), '--socket', 'error-session', '--listen', f'127.0.0.1:{port}',
                        '--token-file', str(token), '--codec', 'png', '--no-audio', '--no-xwayland', *arguments],
                        env={**env, 'LD_PRELOAD': str(shim), 'WAYBRIDGE_TEST_ACCEPT_PATH': str(runtime / 'error-session'),
                             'WAYBRIDGE_TEST_ACCEPT_TRIGGER': str(trigger)}, stdout=host_log, stderr=host_log)

                def cleaned():
                    assert ctl('is-active', 'fixture-owned.service', check=False).stdout.strip() == 'inactive'
                    assert ctl('is-active', 'waybridge-session.target', check=False).stdout.strip() == 'inactive'
                    assert ctl('is-active', 'fixture-unrelated.service').stdout.strip() == 'active'
                    activation = ctl('show-environment').stdout.splitlines()
                    assert not any(value.startswith(('WAYLAND_DISPLAY=', 'WAYBRIDGE_SESSION_ID=')) for value in activation), activation
                    assert 'KEEP_ME=untouched' in activation
                    assert not (runtime / 'error-session').exists()

                host = start()
                wait(lambda: descriptor.exists() or host.poll() is not None)
                assert host.poll() is None, (logs / 'error-shutdown-compositor.log').read_text()
                saved = json.loads(descriptor.read_text())
                status = Path(saved['environment']['WAYBRIDGE_DESKTOP_SERVICES'])
                wait(lambda: status.exists() and any(state['name'] == 'session-lifecycle' and state['status'] == 'ready'
                                 for state in json.loads(status.read_text())['services']))
                trigger.touch()
                host.wait(timeout=20)
                assert not trigger.exists(), 'test-only accept failure was not injected'
                assert host.returncode != 0, 'original compositor error was swallowed'
                events = [json.loads(line)['event'] for line in record.read_text().splitlines()]
                assert events == ['ready', 'stopped-with-display'], (events, (logs / 'error-shutdown-compositor.log').read_text())
                cleaned()
                assert not descriptor.exists() and not status.exists()

                # A command launch error happens after activation but before the
                # event loop. It must use the same display-aware shutdown path.
                host = start('--', str(root / 'absent-program'))
                host.wait(timeout=20)
                assert host.returncode != 0
                assert 'launching ' + str(root / 'absent-program') in (logs / 'error-shutdown-compositor.log').read_text()
                cleaned()
                assert not descriptor.exists()

                # Publication can fail after services start. Its staged Session
                # remains owned by main, which can dispatch shutdown on failure.
                descriptor.mkdir()
                host = start()
                host.wait(timeout=20)
                assert host.returncode != 0
                assert descriptor.is_dir(), 'failed publication replaced another entry'
                descriptor.rmdir()
                cleaned()
                assert 'Input/output error (os error 5)' in (logs / 'error-shutdown-compositor.log').read_text()
                print('PASS: actual event-loop error preserves original failure and display-aware shutdown; launch and descriptor errors stop owned services, clear activation state and preserve unrelated units')
            finally:
                if host is not None and host.poll() is None:
                    host.terminate()
                    host.wait(timeout=20)
                if manager.poll() is None:
                    ctl('stop', 'fixture-owned.service', 'fixture-unrelated.service', 'dbus.service', 'dbus.socket', check=False)
                    ctl('--force', 'exit', check=False)
                    try:
                        manager.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        manager.kill()
                        manager.wait(timeout=3)


def main():
    if '--component' in sys.argv:
        component(sys.argv[-1])
    elif '--inner' in sys.argv:
        run_inner()
    else:
        (ROOT / 'build/tests').mkdir(parents=True, exist_ok=True)
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror', '-o',
                        str(ROOT / 'build/tests/fail-wayland-accept.so'), str(ROOT / 'tests/fail_wayland_accept.c'), '-ldl'], check=True)
        command = ['systemd-run', '--user', '--scope', '--quiet', '--property=Delegate=yes',
                   '--unit=waybridge-error-shutdown-test-' + str(os.getpid()), sys.executable,
                   str(Path(__file__).resolve()), '--inner']
        raise SystemExit(subprocess.run(command).returncode)


if __name__ == '__main__':
    main()
