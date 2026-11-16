#!/usr/bin/env python3
"""Real GIO launch/activation with disposable HOME, catalog and user bus."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from session_fixture import SessionFixture

ROOT = Path(__file__).resolve().parents[1]


def wait(predicate, seconds=8):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        result = predicate()
        if result:
            return result
        time.sleep(.02)
    raise AssertionError('desktop fixture timed out')


def reject(function, fragment):
    try:
        function()
    except ValueError as error:
        assert fragment in str(error), error
    else:
        raise AssertionError('expected rejection: ' + fragment)


def main():
    spec = importlib.util.spec_from_file_location('desktop_helper', ROOT / 'scripts/session-control.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    original = dict(os.environ)
    with tempfile.TemporaryDirectory(prefix='waybridge-desktop-') as directory, SessionFixture(Path(directory) / 'private user') as fixture:
        os.environ.clear()
        os.environ.update(fixture.env)
        os.environ['XDG_DATA_DIRS'] = str(Path(directory) / 'empty-data')
        home = Path(os.environ['HOME'])
        runtime = Path(os.environ['XDG_RUNTIME_DIR'])
        owned_runtime = runtime / 'session-owned'
        owned_runtime.mkdir(mode=0o700)
        helper.RUNTIME = runtime
        # Keep an unusual source path while resolving by its full flattened ID.
        applications = Path(os.environ['XDG_DATA_HOME']) / 'applications'
        applications.mkdir()
        nested = applications / "suite % quote's $"
        nested.mkdir()
        exe = home / 'fixture executable'
        exe.write_text('#!' + sys.executable + '\nimport json,os,sys,pathlib\npathlib.Path(os.environ["MARKER"]).write_text(json.dumps({"argv":sys.argv,"cwd":os.getcwd(),"keep":os.environ.get("KEEP"),"drop":os.environ.get("DROP"),"desktop":os.environ.get("XDG_CURRENT_DESKTOP"),"gio_file":os.environ.get("GIO_LAUNCHED_DESKTOP_FILE"),"activation":os.environ.get("XDG_ACTIVATION_TOKEN"),"startup":os.environ.get("DESKTOP_STARTUP_ID")}))\n')
        exe.chmod(0o700)
        source = nested / 'fixture.desktop'
        original_text = '[Desktop Entry]\nType=Application\nName=Fixture 你好\nIcon=fixture-icon\nExec=' + helper.desktop_escape(helper.exec_literal(str(exe))) + ' base -- %U %c %k %i %% %%k\nActions=Edit;Other;\n\n[Desktop Action Edit]\nName=Edit\nExec=' + helper.desktop_escape(helper.exec_literal(str(exe))) + ' action-edit %k\n\n[Desktop Action Other]\nName=Other\nExec=' + helper.desktop_escape(helper.exec_literal(str(exe))) + ' action-other\n'
        source.write_text(original_text)
        app_id = str(source.relative_to(applications)).replace('/', '-')
        overrides = Path(os.environ['XDG_CONFIG_HOME']) / 'waybridge/desktop-overrides'
        overrides.mkdir(parents=True)
        override = overrides / (app_id + '.toml')
        cwd = home / 'working dir'
        cwd.mkdir()
        marker = runtime / 'marker.json'
        descriptor_path = runtime / 'desktop-test.session.json'
        saved = dict(version=1, pid=os.getpid(), started=helper.identity(os.getpid()),
                     connection=dict(listen='127.0.0.1:9'), environment=dict(WAYLAND_DISPLAY='desktop-test', XDG_CURRENT_DESKTOP='Waybridge',
                     WAYBRIDGE_SESSION_RUNTIME=str(owned_runtime), MARKER=str(marker), KEEP='baseline', DROP='remove-me'))
        descriptor_path.write_text(json.dumps(saved))
        descriptor_path.chmod(0o600)

        def request(op='launch', **fields):
            return helper.dispatch(dict(op=op, session='desktop-test', **fields))

        def launch(**fields):
            marker.unlink(missing_ok=True)
            response = request(**fields)
            assert response['readiness'] == 'not-observed'
            wait(marker.exists)
            return json.loads(marker.read_text()), response

        result, response = launch(argv=[str(exe), 'raw literal $() ; 你好'])
        assert result['cwd'] == str(home)
        assert result['argv'] == [str(exe), 'raw literal $() ; 你好']
        assert response['pid_role'] == 'spawned-process'
        token = 'A'*32
        assert len(token) == 32
        activated, _ = launch(argv=[str(exe)], activation_token=token)
        assert activated['activation'] == activated['startup'] == token
        reject(lambda: request(argv=[str(exe)], activation_token='bad token'), 'Invalid activation token')
        result, _ = launch(argv=[str(exe)], cwd=str(cwd), env={'KEEP': 'changed'}, unset_env=['DROP'])
        assert result['cwd'] == str(cwd) and result['keep'] == 'changed' and result['drop'] is None
        print('PASS: raw argv defaults HOME, explicit cwd/env/unset are scoped, no executable-name policy')

        result, response = launch(app=app_id, uris=['https://example.invalid/a?b=1', (home / 'file 你好').as_uri()])
        assert result['cwd'] == str(home)
        assert result['argv'][1:5] == ['base', '--', 'https://example.invalid/a?b=1', str(home / 'file 你好')], result['argv']
        assert str(source) in result['argv'] and '%' in result['argv'] and '%k' in result['argv']
        assert response['plan']['desktop_id'] == app_id and response['plan']['derived']
        assert response['pid'] > 1 and response['pid_role'] == 'gio-launched-process'
        assert Path(result['gio_file']).name == app_id and Path(result['gio_file']).is_file()

        # NBSP is part of an executable filename in GIO, not an ASCII Exec
        # separator. Both the source and the derived launch must keep it whole.
        unicode_exe = Path(directory) / 'unicode\u00a0executable'
        unicode_exe.write_text(exe.read_text())
        unicode_exe.chmod(0o700)
        unicode_id = 'unicode-space.desktop'
        unicode_source = applications / unicode_id
        unicode_source.write_text('[Desktop Entry]\nType=Application\nName=Unicode whitespace\nExec=' + str(unicode_exe) + ' base\n')
        marker.unlink(missing_ok=True)
        subprocess.run(['gio', 'launch', str(unicode_source)], env={**os.environ, 'MARKER': str(marker)}, cwd=home, check=True)
        wait(marker.exists)
        assert json.loads(marker.read_text())['argv'] == [str(unicode_exe), 'base']
        (overrides / (unicode_id + '.toml')).write_text('version=1\n[default]\nextra_argv=["unicode-extra"]\n')
        result, _ = launch(app=unicode_id)
        assert result['argv'] == [str(unicode_exe), 'unicode-extra', 'base']

        # A Python-specific environment choice belongs to the target process;
        # it cannot be allowed to prevent the Python GIO dispatcher from booting.
        shell = Path(directory) / 'shell-fixture'
        shell.write_text('#!/bin/sh\nprintf \'{"value":"%s"}\' "$PYTHONHOME" > "$MARKER"\n')
        shell.chmod(0o700)
        shell_id = 'shell-fixture.desktop'
        (applications / shell_id).write_text('[Desktop Entry]\nType=Application\nName=Shell fixture\nExec=' + str(shell) + '\n')
        (overrides / (shell_id + '.toml')).write_text('version=1\n[default.env]\nPYTHONHOME="/application-only-python-home"\n')
        result, response = launch(app=shell_id)
        assert result['value'] == '/application-only-python-home'
        assert response['plan']['env_keys'] == ['PYTHONHOME']
        print('PASS: Unicode whitespace executable names and application-only PYTHONHOME through GIO context')

        source.write_text(original_text.replace('Icon=fixture-icon', 'Icon=fixture-icon\nPath=' + str(cwd)))
        result, response = launch(app=app_id)
        assert result['cwd'] == str(cwd) and response['plan']['cwd_source'] == 'desktop'
        source.write_text(original_text)

        extras = ['extra 你好', 'literal %U %k 100%', 'backslash \\ quote " $() `cmd` ;', '']
        override.write_text('version=1\n[default]\nextra_argv=' + json.dumps(extras, ensure_ascii=False) + '\ncwd=' + json.dumps(str(cwd)) + '\nunset_env=["DROP"]\nname="  Overridden 名称  "\nicon="override-icon"\n[default.env]\nKEEP="credential-not-exported"\n[actions.Edit]\nextra_argv=["only-edit"]\n')
        plan = request('launch-plan', app=app_id)
        assert 'credential-not-exported' not in json.dumps(plan)
        assert plan['env_keys'] == ['KEEP'] and plan['unset_env'] == ['DROP']
        assert plan['override'] == str(override) and plan['override_section'] == 'default'
        result, response = launch(app=app_id)
        assert result['argv'][1:1 + len(extras)] == extras, result['argv']
        assert result['argv'][1 + len(extras):3 + len(extras)] == ['base', '--']
        assert str(source) in result['argv'] and '%k' in result['argv'] and '%' in result['argv']
        assert '  Overridden 名称  ' in result['argv'] and 'override-icon' in result['argv']
        assert response['plan']['name'] == '  Overridden 名称  '
        assert result['cwd'] == str(cwd) and result['keep'] == 'credential-not-exported' and result['drop'] is None
        result, response = launch(app=app_id, action='Edit')
        assert result['argv'][1:] == ['only-edit', 'action-edit', str(source)]
        assert result['cwd'] == str(home) and result['keep'] == 'baseline' and result['drop'] == 'remove-me'
        assert response['plan']['override_section'] == 'actions.Edit'
        result, _ = launch(app=app_id, action='Other')
        assert result['argv'][1:] == ['action-other']
        result, _ = launch(argv=[str(exe)])
        assert result['argv'] == [str(exe)] and result['cwd'] == str(home)
        assert source.read_text() == original_text
        # A local standard GIO launch sees the source entry, without the overlay.
        marker.unlink()
        subprocess.run(['gio', 'launch', str(source)], env={**os.environ, 'MARKER': str(marker)}, cwd=home, check=True)
        wait(marker.exists)
        result = json.loads(marker.read_text())
        assert result['argv'][1:3] == ['base', '--'] and 'Fixture 你好' in result['argv']
        print('PASS: full desktop ID, original %k, literal extras before --, URI fields, action independence and unchanged local launch')

        override.write_text('version=1\n[default]\nexec=' + json.dumps(helper.desktop_escape(helper.exec_literal(str(exe))) + ' replacement %k') + '\nclear=["cwd", "icon"]\n')
        source.write_text(original_text.replace('Icon=fixture-icon', 'Icon=fixture-icon\nPath=' + str(cwd)))
        result, _ = launch(app=app_id)
        assert result['argv'][1:] == ['replacement', str(source)] and result['cwd'] == str(home)
        source.write_text(original_text)
        for invalid, message in [
            ('version=1\n[default]\nunknown=true\n', 'Unknown desktop override'),
            ('version=2\n', 'version=1'),
            ('version=1\n[default]\nextra_argv=[3]\n', 'Invalid extra_argv'),
            ('version=1\n[default]\ncwd="relative"\n', 'absolute directory'),
            ('version=1\n[default]\nunset_env=["DROP"]\nenv={DROP="x"}\n', 'cannot contain the same'),
            ('version=1\n[default]\nexec="x"\nclear=["exec"]\n', 'clear accepts'),
            ('version=1\n[actions.NoSuch]\nextra_argv=[]\n', 'unknown desktop actions')]:
            override.write_text(invalid)
            reject(lambda: request('launch-plan', app=app_id), message)
        override.unlink()
        reject(lambda: request('launch-plan', app=app_id, action='Edit', uris=['file:///tmp/a']), 'do not accept')
        reject(lambda: request('launch-plan', app=app_id, uris=['/tmp/file']), 'absolute URI')
        print('PASS: replacement/clear semantics, unknown-field/schema/env/action validation')

        # Own a genuine org.freedesktop.Application on the disposable test bus.
        service = home / 'activation-fixture.py'
        events = runtime / 'dbus-event.json'
        service.write_text('''import json,sys
from pathlib import Path
from gi.repository import Gio,GLib
bus=Gio.bus_get_sync(Gio.BusType.SESSION,None)
def method(c,s,p,i,n,a,inv):
 Path(sys.argv[1]).write_text(json.dumps({"method":n,"args":a.unpack()}))
 inv.return_value(GLib.Variant("()",()))
xml='<node><interface name="org.freedesktop.Application"><method name="Activate"><arg type="a{sv}" direction="in"/></method><method name="Open"><arg type="as" direction="in"/><arg type="a{sv}" direction="in"/></method><method name="ActivateAction"><arg type="s" direction="in"/><arg type="av" direction="in"/><arg type="a{sv}" direction="in"/></method></interface></node>'
bus.register_object('/org/example/LaunchFixture',Gio.DBusNodeInfo.new_for_xml(xml).interfaces[0],method,None,None)
bus.call_sync('org.freedesktop.DBus','/org/freedesktop/DBus','org.freedesktop.DBus','RequestName',GLib.Variant('(su)',('org.example.LaunchFixture',0)),None,Gio.DBusCallFlags.NONE,3000,None)
print('ready',flush=True)
GLib.MainLoop().run()
''')
        server = subprocess.Popen([sys.executable, str(service), str(events)], env=os.environ, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            assert server.stdout.readline().strip() == 'ready'
            dbus_id = 'org.example.LaunchFixture.desktop'
            dbus_source = applications / dbus_id
            dbus_source.write_text(original_text.replace('DBusActivatable=false\n', '').replace('Type=Application', 'Type=Application\nDBusActivatable=true'))
            def activate(**fields):
                events.unlink(missing_ok=True)
                response = request(app=dbus_id, **fields)
                assert response['status'] == 'activation-requested' and 'pid' not in response
                wait(events.exists)
                return json.loads(events.read_text())
            assert activate()['method'] == 'Activate'
            activated = activate(activation_token=token)
            assert activated['args'][0].get('desktop-startup-id') == token or activated['args'][0].get('activation-token') == token
            opened = activate(uris=['https://example.invalid/'])
            assert opened['method'] == 'Open' and opened['args'][0] == ['https://example.invalid/']
            action = activate(action='Edit')
            assert action['method'] == 'ActivateAction' and action['args'][0] == 'Edit'
            dbus_override = overrides / (dbus_id + '.toml')
            dbus_override.write_text('version=1\n[default]\nclear=["exec"]\nname="Metadata only"\n')
            assert activate()['method'] == 'Activate', 'clearing fallback Exec disabled D-Bus activation'
            dbus_override.write_text('version=1\n[default]\nextra_argv=["dbus-conflict"]\n')
            reject(lambda: request('launch-plan', app=dbus_id), 'explicitly set launch_mode')
            assert activate(action='Other')['args'][0] == 'Other', 'default conflict leaked into independent action'
            dbus_override.write_text('version=1\n[default]\nlaunch_mode="exec"\nextra_argv=["explicit-exec"]\n')
            events.unlink()
            result, response = launch(app=dbus_id)
            assert result['argv'][1] == 'explicit-exec' and response['status'] == 'dispatched'
            assert not events.exists() and 'DBusActivatable=true' in dbus_source.read_text()
            print('PASS: real GIO D-Bus Activate/Open/ActivateAction, explicit process conflict and exec opt-in')
        finally:
            server.terminate()
            server.wait(timeout=5)
    os.environ.clear()
    os.environ.update(original)


if __name__ == '__main__':
    main()
