#!/usr/bin/env python3
"""Shared app data/Steam policy with controlled executable fixtures.

Optional --actual-chrome opens only about:blank in a new temporary private
user configuration and real Waybridge session; no host browser profile or wallet is read.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def wait(predicate, seconds=8):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.02)
    raise AssertionError("controlled launch fixture timed out")


def reject(action, contains):
    try:
        action()
    except ValueError as error:
        assert contains in str(error), str(error)
        return
    raise AssertionError("expected launch policy rejection")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--actual-chrome", action="store_true")
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("launch_helper", ROOT / "scripts/session-control.py")
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    with tempfile.TemporaryDirectory(prefix="waybridge-launch-policy-") as directory:
        root = Path(directory)
        runtime = root / "runtime % quote's $"
        data = root / "data % quote's $"
        runtime.mkdir(mode=0o700)
        data.mkdir(mode=0o700)
        os.environ.update(XDG_RUNTIME_DIR=str(runtime), XDG_DATA_HOME=str(data), XDG_DATA_DIRS=str(root / "no-system-apps"),
                          XDG_CONFIG_HOME=str(root / "config"), XDG_CACHE_HOME=str(root / "cache"), XDG_STATE_HOME=str(root / "state"))
        helper.RUNTIME = runtime
        binary_directory = root / "bin"
        binary_directory.mkdir()
        (data / "applications").mkdir()
        fixture = "#!/usr/bin/python3\nimport json,os,sys,pathlib\npathlib.Path(os.environ['WAYBRIDGE_POLICY_MARKER']).write_text(json.dumps({'argv':sys.argv,'desktop':os.environ.get('XDG_CURRENT_DESKTOP'),'display':os.environ.get('WAYLAND_DISPLAY')}))\n"
        chrome = binary_directory / "google-chrome-stable"
        generic = binary_directory / "generic-app"
        for path in [chrome, generic]:
            path.write_text(fixture)
            path.chmod(0o700)
        compositor = binary_directory / "fixture-compositor"
        compositor.write_text("#!/usr/bin/python3\nimport json,os,pathlib,sys\nsession=sys.argv[sys.argv.index('--socket')+1]\nsaved=json.loads((pathlib.Path(os.environ['XDG_RUNTIME_DIR'])/(session+'.session.json')).read_text())\nfor k,v in saved['environment'].items():\n if v is None: os.environ.pop(k,None)\n else: os.environ[k]=v\nargv=sys.argv[sys.argv.index('--')+1:]\nos.execvpe(argv[0],argv,os.environ)\n")
        compositor.chmod(0o700)
        saved = dict(version=1, pid=os.getpid(), started=helper.identity(os.getpid()),
                     connection=dict(listen="127.0.0.1:9", token_file=str(runtime / "unused-token")),
                     environment=dict(WAYLAND_DISPLAY="policy-0", DISPLAY=":policy", DBUS_SESSION_BUS_ADDRESS="unix:path=/controlled-private-bus",
                                      XDG_DATA_HOME=str(data), XDG_CURRENT_DESKTOP="Waybridge"))

        def launch(argv=None, app=None, session="policy-0"):
            marker = runtime / ("marker-" + str(time.monotonic_ns()) + ".json")
            current = json.loads(json.dumps(saved))
            current["environment"]["WAYLAND_DISPLAY"] = session
            current["environment"]["WAYBRIDGE_POLICY_MARKER"] = str(marker)
            descriptor = runtime / (session + ".session.json")
            descriptor.write_text(json.dumps(current))
            descriptor.chmod(0o600)
            request = dict(op="launch", session=session, binary=str(compositor))
            request.update(dict(app=app) if app else dict(argv=argv))
            helper.dispatch(request)
            wait(marker.exists)
            return json.loads(marker.read_text())

        literal = "literal ; $(not-a-shell) 你好"
        first = launch([str(chrome), literal])
        assert first["argv"] == [str(chrome), literal]
        assert first["desktop"] == "Waybridge" and first["display"] == "policy-0"
        explicit = str(root / "explicit profile")
        chosen = [str(chrome), "--user-data-dir=" + explicit, "--profile-directory=Work profile", "--password-store=kwallet6"]
        assert launch(chosen)["argv"] == chosen
        assert launch([str(chrome), "--profile-directory=Work profile"])["argv"] == [str(chrome), "--profile-directory=Work profile"]
        assert helper.launch_policy(["flatpak", "run", "com.google.Chrome"], "policy-0", saved) == ["flatpak", "run", "com.google.Chrome"]
        assert not (data / "waybridge/sessions").exists()
        print("PASS: browser default data/backend unchanged, explicit profile choices survive, no isolated profile created")

        browser_entry = data / "applications/google-chrome.desktop"
        browser_entry.write_text("[Desktop Entry]\nType=Application\nName=Browser fixture 你好\nIcon=fixture-icon\nDBusActivatable=true\nExec=" + str(chrome) + " \"--profile-directory=Work profile\" \"literal ; $(not-a-shell)\" %c %k %i %%\n")
        catalog = launch(app="google-chrome.desktop")
        assert not any(value.startswith(("--user-data-dir=", "--password-store=")) for value in catalog["argv"])
        assert "--profile-directory=Work profile" in catalog["argv"]
        assert literal.split(" 你好")[0] in catalog["argv"]
        assert "Browser fixture 你好" in catalog["argv"] and "--icon" in catalog["argv"] and "fixture-icon" in catalog["argv"]
        assert "%" in catalog["argv"]
        assert "DBusActivatable=true" in browser_entry.read_text()
        browser_entry.write_text("[Desktop Entry]\nType=Application\nName=Browser fixture\nExec=" + str(chrome) + " \"--user-data-dir=" + explicit + "\" --password-store=kwallet6 %U\n")
        chosen_catalog = launch(app="google-chrome.desktop")["argv"]
        assert chosen_catalog == [str(chrome), "--user-data-dir=" + explicit, "--password-store=kwallet6"]
        browser_entry.write_text("[Desktop Entry]\nType=Application\nName=Browser wrapper\nExec=/usr/bin/env " + str(chrome) + " %U\n")
        assert launch(app="google-chrome.desktop")["argv"] == [str(chrome)]
        generic_entry = data / "applications/generic.desktop"
        generic_entry.write_text("[Desktop Entry]\nType=Application\nName=Generic fixture\nExec=" + str(generic) + " \"literal ; $(not-a-shell)\" %c %k %%\n")
        normal = launch(app="generic.desktop")["argv"]
        assert normal == [str(generic), "literal ; $(not-a-shell)", "Generic fixture", str(generic_entry), "%"]
        assert launch([str(generic), literal])["argv"] == [str(generic), literal]
        print("PASS: GIO retains .desktop quoting, Unicode/field expansion and generic app semantics; original browser entry remains unchanged")

        steam = binary_directory / "steam"
        shutil.copyfile("/usr/bin/sleep", steam)
        steam.chmod(0o700)
        host = subprocess.Popen([str(steam), "30"], env={**os.environ, "DISPLAY":":host", "WAYLAND_DISPLAY":"host-wayland", "DBUS_SESSION_BUS_ADDRESS":"unix:path=/controlled-host-bus"})
        try:
            wait(lambda: Path(f"/proc/{host.pid}/exe").exists())
            reject(lambda: helper.check_steam_instance(saved, [Path(f"/proc/{host.pid}")]), "already running outside")
            reject(lambda: helper.launch_policy([str(steam)], "policy-0", saved), "already running outside")
            steam_entry = data / "applications/steam.desktop"
            steam_entry.write_text("[Desktop Entry]\nType=Application\nName=Steam fixture\nExec=" + str(steam) + " %U\n")
            reject(lambda: helper.dispatch(dict(op="launch", session="policy-0", binary=str(compositor), app="steam.desktop")), "already running outside")
            assert host.poll() is None, "conflict detection killed existing Steam"
        finally:
            host.terminate()
            host.wait(timeout=5)
        same = subprocess.Popen([str(steam), "30"], env={**os.environ, **saved["environment"]})
        try:
            helper.check_steam_instance(saved, [Path(f"/proc/{same.pid}")])
            assert same.poll() is None
        finally:
            same.terminate()
            same.wait(timeout=5)
        print("PASS: same-UID Steam routing metadata distinguishes host/private session; conflicting instances are rejected without stopping them")

        if args.actual_chrome:
            from smoke import Viewer
            from session_fixture import SessionFixture
            with SessionFixture(runtime / 'actual user') as user:
                os.environ.update(user.env)
                runtime = Path(user.env['XDG_RUNTIME_DIR'])
                helper.RUNTIME = runtime
                for path in runtime.glob("*.session.json"):
                    path.unlink()
                # Keep the escaped runtime/data paths for the real compositor and
                # installed Chrome as well as the controlled GIO cases above.
                try:
                    actual = helper.ensure(dict(session="actual-browser", binary=str(ROOT / "target/debug/waybridge"), codec="png", audio=False, xwayland=False))
                except ValueError as error:
                    diagnostic = runtime / "waybridge-control-center/actual-browser.log"
                    raise AssertionError(diagnostic.read_text()[-3000:]) from error
                child = None
                viewer = None
                try:
                    viewer = Viewer(actual["port"], actual["token"])
                    child = helper.dispatch(dict(op="launch", session="actual-browser", binary=str(ROOT / "target/debug/waybridge"),
                                                 argv=["/usr/bin/google-chrome-stable", "--no-first-run", "--no-default-browser-check", "--disable-background-networking", "about:blank"]))["pid"]
                    metadata = viewer.until(lambda kind, message: kind == "control" and message.get("type") == "window" and "chrome" in message.get("app_id", "").lower())
                    assert metadata["id"] > 0
                    actual_profile = Path(os.environ["XDG_CONFIG_HOME"]) / "google-chrome"
                    lock = actual_profile / "SingletonLock"
                    wait(lock.is_symlink)
                    browser_pid = int(os.readlink(lock).rsplit("-", 1)[1])
                    assert Path(f"/proc/{browser_pid}").stat().st_uid == os.getuid()
                    time.sleep(.15)
                    state = Path(f"/proc/{browser_pid}/stat").read_text().rsplit(")", 1)[1].split()[0]
                    assert state != "Z", "browser exited after announcing its window"
                    actual_applications = Path(os.environ["XDG_DATA_HOME"]) / "applications"
                    actual_applications.mkdir(exist_ok=True)
                    installed_entry = Path("/usr/share/applications/google-chrome.desktop").read_text()
                    installed_entry = installed_entry.replace("Exec=/usr/bin/google-chrome-stable %U", "Exec=/usr/bin/google-chrome-stable --new-window about:blank %U", 1)
                    (actual_applications / "google-chrome.desktop").write_text(installed_entry)
                    helper.dispatch(dict(op="launch", session="actual-browser", binary=str(ROOT / "target/debug/waybridge"), app="google-chrome.desktop"))
                    second = viewer.until(lambda kind, message: kind == "control" and message.get("type") == "window" and message.get("id") != metadata["id"] and "chrome" in message.get("app_id", "").lower())
                    assert second["id"] != metadata["id"]
                    assert int(os.readlink(lock).rsplit("-", 1)[1]) == browser_pid
                    print("PASS: installed Chrome opens an actual Waybridge window using its normal user profile inside temporary test XDG directories and Waybridge desktop identity")
                finally:
                    if child:
                        try:
                            os.killpg(child, signal.SIGTERM)
                        except ProcessLookupError:
                            pass
                    if viewer:
                        viewer.sock.close()
                    helper.dispatch(dict(op="end", session="actual-browser"))
                    wait(lambda: not (runtime / "actual-browser.session.json").exists())



if __name__ == "__main__":
    main()
