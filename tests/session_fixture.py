#!/usr/bin/env python3
"""Disposable user bus and logind/systemd fixtures; never touch the live desktop."""
import json
import os
import signal
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


class SessionFixture:
    def __init__(self, directory, real_manager=False, status_write_failure=False):
        self.real_manager = real_manager
        self.status_write_failure = status_write_failure
        self.root = Path(directory)
        self.root.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.env = dict(os.environ)
        # An isolated user bus must never activate clients against host displays.
        for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'WAYLAND_SOCKET', 'QT_QPA_PLATFORM',
                    'GDK_BACKEND', 'SESSION_MANAGER', 'XDG_CURRENT_DESKTOP'):
            self.env.pop(key, None)
        # Keep compiler/package caches while giving applications a disposable
        # HOME. Otherwise rustup would lose the user's selected toolchain.
        for key, leaf in [('RUSTUP_HOME', '.rustup'), ('CARGO_HOME', '.cargo'),
                          ('NUGET_PACKAGES', '.nuget/packages')]:
            self.env.setdefault(key, str(Path.home() / leaf))
        original_runtime = Path(os.environ.get('XDG_RUNTIME_DIR', f'/run/user/{os.getuid()}'))
        pulse = original_runtime / 'pulse/native'
        if pulse.exists():
            self.env.setdefault('PULSE_SERVER', f'unix:{pulse}')
        self.env.setdefault('PIPEWIRE_RUNTIME_DIR', str(original_runtime))
        for key, leaf in [('HOME', 'home'), ('XDG_RUNTIME_DIR', 'runtime'),
                          ('XDG_CONFIG_HOME', 'config'), ('XDG_DATA_HOME', 'data'),
                          ('XDG_CACHE_HOME', 'cache'), ('XDG_STATE_HOME', 'state')]:
            path = self.root / leaf
            path.mkdir(mode=0o700, exist_ok=True)
            self.env[key] = str(path)
        self.sessions = self.root / 'sessions.json'
        self.sessions.write_text('[]')
        self.manager = self.root / 'manager.json'
        self.manager.write_text(json.dumps({'WAYLAND_DISPLAY': 'previous-display', 'KEEP_ME': 'unchanged'}))
        self.bus = self.server = None
        # Older compositor tests each choose another temporary display runtime.
        # Shared document services must keep their FUSE mount in the fixture's
        # own runtime until this user bus ends, rather than those shorter-lived
        # per-test directories. This override exists only on the disposable bus.
        document = next((Path(p) for p in ('/usr/lib/xdg-document-portal', '/usr/libexec/xdg-document-portal') if Path(p).is_file()), None)
        if document:
            catalog = Path(self.env['XDG_DATA_HOME']) / 'dbus-1/services'
            catalog.mkdir(parents=True)
            wrapper = self.root / 'document-provider.py'
            wrapper.write_text('import os\nos.environ["XDG_RUNTIME_DIR"]=' + repr(self.env['XDG_RUNTIME_DIR']) + '\nos.execv(' + repr(str(document)) + ', [' + repr(str(document)) + '])\n')
            quoted = str(wrapper).replace('\\', '\\\\').replace('"', '\\"')
            (catalog / 'org.freedesktop.portal.Documents.service').write_text(
                '[D-BUS Service]\nName=org.freedesktop.portal.Documents\nExec=/usr/bin/python3 "' + quoted + '"\n')

        # Per-test display runtimes are shorter-lived than this shared bus.
        # GVFS URI APIs remain real; only its optional filesystem mirror is
        # disabled on the disposable test bus, so it cannot pin those runtimes.
        gvfs = next((Path(p) for p in ('/usr/lib/gvfsd', '/usr/libexec/gvfsd') if Path(p).is_file()), None)
        if gvfs:
            catalog = Path(self.env['XDG_DATA_HOME']) / 'dbus-1/services'
            catalog.mkdir(parents=True, exist_ok=True)
            (catalog / 'org.gtk.vfs.Daemon.service').write_text(
                '[D-BUS Service]\nName=org.gtk.vfs.Daemon\nExec=' + str(gvfs) + ' --no-fuse\n')

    def __enter__(self):
        self.log = (self.root / 'bus.log').open('w')
        self.bus = subprocess.Popen(['dbus-daemon', '--session', '--nofork', '--print-address=1'],
                                    env=self.env, stdout=subprocess.PIPE, stderr=self.log, text=True, start_new_session=True)
        address = self.bus.stdout.readline().strip()
        assert address.startswith('unix:'), 'fixture bus did not start'
        self.env.update(DBUS_SESSION_BUS_ADDRESS=address, DBUS_SYSTEM_BUS_ADDRESS=address,
                        WAYBRIDGE_TEST_SESSION_FIXTURE='1')
        self.server = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), '--server',
                                        str(self.sessions), str(self.manager),
                                        "real" if self.real_manager else "status-failure" if self.status_write_failure else "fake"], env=self.env,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        assert self.server.stdout.readline().strip() == 'ready', self.server.stderr.read()
        return self

    def __exit__(self, *unused):
        if self.bus and self.bus.poll() is None:
            os.killpg(self.bus.pid, signal.SIGTERM)
        for child in (self.server, self.bus):
            if child and child.poll() is None:
                child.terminate()
                child.wait(timeout=8)
        if self.bus:
            try:
                os.killpg(self.bus.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        # The document portal belongs to this disposable user fixture. Its
        # FUSE mount must be detached before TemporaryDirectory removes it.
        mount = Path(self.env['XDG_RUNTIME_DIR']) / 'doc'
        unmount = shutil.which('fusermount3') or shutil.which('fusermount')
        if unmount:
            subprocess.run([unmount, '-u', '-z', '--', str(mount)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.log.close()

    def set_sessions(self, entries):
        temporary = self.sessions.with_suffix('.tmp')
        temporary.write_text(json.dumps(entries))
        temporary.replace(self.sessions)


def server(sessions_path, manager_path, mode="fake"):
    import gi
    gi.require_version('Gio', '2.0')
    from gi.repository import Gio, GLib
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    manager_path = Path(manager_path)
    def session(index):
        entries = json.loads(Path(sessions_path).read_text())
        return entries[index] if index < len(entries) else {}
    def method(connection, sender, path, interface, name, args, invocation):
        if name == 'ListSessions':
            entries = json.loads(Path(sessions_path).read_text())
            value = [(str(i), s.get('uid', os.getuid()), 'fixture', '',
                      f'/org/freedesktop/login1/session/s{i}') for i, s in enumerate(entries)]
            invocation.return_value(GLib.Variant('(a(susso))', (value,)))
        else:
            values = json.loads(manager_path.read_text())
            for value in args.unpack()[0]:
                if name == 'SetEnvironment':
                    key, data = value.split('=', 1)
                    values[key] = data
                else:
                    values.pop(value, None)
            manager_path.write_text(json.dumps(values))
            if mode == 'status-failure' and name == 'SetEnvironment' and values.get('WAYBRIDGE_SESSION_ID'):
                # Fail the compositor's first status persist, after environment
                # publication but before its lifecycle worker starts.
                (Path(values['WAYBRIDGE_SESSION_ID']) / 'status.json').mkdir(exist_ok=True)
            invocation.return_value(GLib.Variant('()', ()))
    manager_xml = '''<node><interface name="org.freedesktop.login1.Manager"><method name="ListSessions"><arg type="a(susso)" direction="out"/></method></interface></node>'''
    session_xml = '''<node><interface name="org.freedesktop.login1.Session"><property name="Type" type="s" access="read"/><property name="Class" type="s" access="read"/><property name="State" type="s" access="read"/></interface></node>'''
    systemd_xml = '''<node><interface name="org.freedesktop.systemd1.Manager"><property name="Environment" type="as" access="read"/><method name="SetEnvironment"><arg type="as" direction="in"/></method><method name="UnsetEnvironment"><arg type="as" direction="in"/></method></interface></node>'''
    bus.register_object('/org/freedesktop/login1', Gio.DBusNodeInfo.new_for_xml(manager_xml).interfaces[0], method, None, None)
    for i in range(32):
        def prop(connection, sender, path, interface, key, index=i):
            return GLib.Variant('s', session(index).get(key.lower(), {'Type': 'tty', 'Class': 'user', 'State': 'active'}[key]))
        bus.register_object(f'/org/freedesktop/login1/session/s{i}', Gio.DBusNodeInfo.new_for_xml(session_xml).interfaces[0], None, prop, None)
    def environment(*unused):
        return GLib.Variant('as', [f'{k}={v}' for k, v in json.loads(manager_path.read_text()).items()])
    if mode != 'real':
        bus.register_object('/org/freedesktop/systemd1', Gio.DBusNodeInfo.new_for_xml(systemd_xml).interfaces[0], method, environment, None)
    for name in (('org.freedesktop.login1',) if mode == 'real' else ('org.freedesktop.login1', 'org.freedesktop.systemd1')):
        bus.call_sync('org.freedesktop.DBus', '/org/freedesktop/DBus', 'org.freedesktop.DBus',
                      'RequestName', GLib.Variant('(su)', (name, 0)), None, Gio.DBusCallFlags.NONE, 3000, None)
    print('ready', flush=True)
    GLib.MainLoop().run()


if __name__ == '__main__':
    if sys.argv[1:2] == ['--server']:
        server(*sys.argv[2:])
    else:
        with tempfile.TemporaryDirectory(prefix='wb-test-user-') as directory, SessionFixture(directory) as fixture:
            raise SystemExit(subprocess.run(sys.argv[1:], cwd=ROOT, env=fixture.env).returncode)
