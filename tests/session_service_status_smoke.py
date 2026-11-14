#!/usr/bin/env python3
"""A failed initial status persist must not strand activation environment."""
import json
from pathlib import Path
import socket
import subprocess
import tempfile
import time

from session_fixture import ROOT, SessionFixture


def wait(predicate):
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.03)
    raise AssertionError('session status-failure fixture timed out')


def main():
    with tempfile.TemporaryDirectory(prefix='wb-status-failure-') as directory, SessionFixture(
            directory, status_write_failure=True) as fixture:
        runtime = Path(fixture.env['XDG_RUNTIME_DIR'])
        # An explicit empty value must remain distinct from an absent value.
        fixture.manager.write_text(json.dumps(dict(WAYLAND_DISPLAY='stale-display', DISPLAY='', KEEP_ME='unchanged')))
        token = runtime / 'token'
        token.write_text('status-failure-' + '8' * 40)
        token.chmod(0o600)
        with socket.socket() as reservation:
            reservation.bind(('127.0.0.1', 0))
            port = reservation.getsockname()[1]
        log = Path(directory) / 'host.log'
        descriptor = runtime / 'status-failure.session.json'
        with log.open('w') as output:
            host = subprocess.Popen([str(ROOT / 'target/debug/waybridge'), '--socket', 'status-failure',
                                     '--listen', f'127.0.0.1:{port}', '--token-file', str(token),
                                     '--codec', 'png', '--no-audio', '--no-xwayland'],
                                    env=fixture.env, stdout=output, stderr=output)
            try:
                wait(lambda: descriptor.exists() or host.poll() is not None)
                assert host.poll() is None, log.read_text()
                saved = json.loads(descriptor.read_text())
                status = Path(saved['environment']['WAYBRIDGE_DESKTOP_SERVICES'])
                assert status.is_dir(), 'fault fixture did not block status persistence'
                assert json.loads(fixture.manager.read_text())['WAYLAND_DISPLAY'] == 'status-failure'
                assert 'cannot write desktop-service status' in log.read_text()
            finally:
                if host.poll() is None:
                    host.terminate()
                host.wait(timeout=20)
        assert host.returncode == 0, log.read_text()
        assert json.loads(fixture.manager.read_text()) == {'KEEP_ME': 'unchanged', 'DISPLAY': ''}
        assert not descriptor.exists() and not status.exists()
        destination = ROOT / 'build/tests/status-failure-compositor.log'
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(log.read_text())
        print('PASS: initial status write failure leaves session usable and activation environment is cleaned on exit')


if __name__ == '__main__':
    main()
