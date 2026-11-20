#!/usr/bin/env python3
"""Check live viewer appearance through the real XDG Settings frontend."""
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from gui_session_smoke import ROOT, Rpc, Viewer, stop_process, wait
from session_fixture import SessionFixture

DESKTOP = 'org.freedesktop.portal.Desktop'
OBJECT = '/org/freedesktop/portal/desktop'
SETTINGS = 'org.freedesktop.portal.Settings'
APPEARANCE = 'org.freedesktop.appearance'


def main():
    executable = next((p for p in ('/usr/lib/xdg-desktop-portal', '/usr/libexec/xdg-desktop-portal') if Path(p).is_file()), None)
    if not executable:
        raise SystemExit('Install xdg-desktop-portal 1.22 or later to run this test')
    with tempfile.TemporaryDirectory(prefix='waybridge-appearance-') as directory, SessionFixture(directory) as fixture:
        shutil.copytree(ROOT / 'integration/xdg-desktop-portal', Path(fixture.env['XDG_DATA_HOME']) / 'xdg-desktop-portal')
        env = {**fixture.env, 'XDG_CURRENT_DESKTOP': 'Waybridge', 'WAYLAND_DISPLAY': 'appearance-0'}
        rpc = Rpc(env)
        session = viewer = portal = monitor = None
        log = Path(directory) / 'portal.log'
        try:
            session = rpc.ok(op='ensure', session=env['WAYLAND_DISPLAY'], binary=str(ROOT / 'target/debug/waybridge'),
                             port=0, codec='png', audio=False, xwayland=False)
            address = env['DBUS_SESSION_BUS_ADDRESS']

            def call(dest, object_path, method, *args):
                return subprocess.run(['gdbus', 'call', '--address', address, '--dest', dest, '--object-path',
                                       object_path, '--method', method, *args], text=True,
                                      capture_output=True, timeout=4)

            def has_owner(bus_name):
                return 'true' in call('org.freedesktop.DBus', '/org/freedesktop/DBus',
                                      'org.freedesktop.DBus.NameHasOwner', bus_name).stdout

            wait(lambda: has_owner('org.freedesktop.impl.portal.desktop.waybridge'))
            with log.open('w') as output:
                portal = subprocess.Popen([executable, '--verbose'], env=env, stdout=output, stderr=subprocess.STDOUT)
            wait(lambda: has_owner(DESKTOP))

            def read(expected):
                result = call(DESKTOP, OBJECT, SETTINGS + '.ReadOne', APPEARANCE, 'color-scheme')
                return f'uint32 {expected}>' in result.stdout

            assert read(0), 'appearance should start with no preference'
            viewer = Viewer(session['port'], session['token'], color_scheme='dark')
            assert 'appearance_v1' in viewer.hello['features']
            wait(lambda: read(1))
            all_settings = call(DESKTOP, OBJECT, SETTINGS + '.ReadAll', "['org.freedesktop.*']")
            assert re.search(r"'color-scheme': <uint32 1>", all_settings.stdout), all_settings.stdout

            event_log = Path(directory) / 'settings.log'
            with event_log.open('w') as output:
                monitor = subprocess.Popen(['gdbus', 'monitor', '--address', address, '--dest', DESKTOP,
                                            '--object-path', OBJECT], stdout=output, stderr=subprocess.STDOUT, text=True)
            wait(lambda: 'owned by' in event_log.read_text())
            viewer.send(type='appearance', color_scheme='light')
            wait(lambda: read(2))
            wait(lambda: 'SettingChanged' in event_log.read_text() and 'uint32 2' in event_log.read_text())
            viewer.send(type='appearance', color_scheme='invalid')
            viewer.send(type='ping', nonce=9)
            viewer.until(lambda kind, value: kind == 'control' and value.get('type') == 'pong' and value.get('nonce') == 9)
            assert read(2), 'an unknown color scheme must be ignored'
            print('PASS: authenticated initial theme and live changes reach the XDG Appearance portal')
        except Exception:
            if log.exists():
                print(log.read_text())
            raise
        finally:
            if viewer:
                viewer.sock.close()
            for child in (monitor, portal):
                if child:
                    stop_process(child.pid)
                    child.wait(timeout=3)
            if session:
                rpc.ok(op='end', session=env['WAYLAND_DISPLAY'])
            rpc.close()


if __name__ == '__main__':
    main()
