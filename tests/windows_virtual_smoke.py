#!/usr/bin/env python3
"""Actual STA IDataObject -> native viewer -> real Linux app, no CF_HDROP."""
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import unquote, urlparse
from drag_out_smoke import wait_for
from smoke import ROOT


def pattern(index, size):
    return bytes((i * 37 + index * 19 + 11) & 255 for i in range(size))


def verify(uris, ansi=False):
    paths = [Path(unquote(urlparse(uri).path)) for uri in uris.decode().splitlines() if uri]
    roots = {p.name: p for p in paths}
    attachment = "attachment.bin" if ansi else "附件 图.bin"
    assert set(roots) == {attachment, "empty.bin", "archive"}, roots
    assert roots[attachment].read_bytes() == pattern(0, 180000)
    assert roots["empty.bin"].read_bytes() == b""
    assert (roots["archive"] / "empty").is_dir()
    assert (roots["archive"] / "folder/inside.bin").read_bytes() == pattern(2, 1234)
    return paths


def main():
    subprocess.run(["bash", "tests/build-fixture.sh"], cwd=ROOT, check=True)
    for name in ("windows_virtual_probe", "windows_probe"):
        subprocess.run(["x86_64-w64-mingw32-g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-municode", "-static", "-O2", f"tests/{name}.cpp", "-o", f"build/tests/{name}.exe", "-lshell32", "-lole32", "-luuid", "-lgdi32"], cwd=ROOT, check=True)
    prefix = ROOT / "build/wine-virtual"
    prefix.mkdir(parents=True, mode=0o700, exist_ok=True)
    processes = []
    with tempfile.TemporaryDirectory(prefix="waybridge-virtual-") as directory, open(ROOT / "build/tests/virtual.log", "w+") as log, open(ROOT / "build/tests/virtual-app.log", "w+") as app_log:
        root = Path(directory)
        read, write = os.pipe()
        xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write), "-screen", "0", "1280x1024x24", "-nolisten", "tcp"], pass_fds=[write], stdout=log, stderr=log)
        processes.append(xvfb)
        os.close(write)
        with os.fdopen(read) as pipe:
            display = ":" + pipe.readline().strip()
        wine = {**os.environ, "DISPLAY": display, "WINEPREFIX": str(prefix), "WINEDEBUG": "-all", "WINEDLLOVERRIDES": "winemenubuilder.exe=d;mscoree,mshtml=", "WAYBRIDGE_GDI": "1"}
        env = {**os.environ, "XDG_RUNTIME_DIR": directory, "WAYLAND_DISPLAY": "virtual-0", "WAYBRIDGE_TEST_DROP": str(root / "drop")}
        with socket.socket() as free:
            free.bind(("127.0.0.1", 0))
            port = free.getsockname()[1]
        token = root / "token"
        token.write_text("virtual-smoke-" + "5" * 40)
        token.chmod(0o600)
        def native(*args, check=True):
            return subprocess.run(["wine", str(ROOT / "build/tests/windows_probe.exe"), *args], env=wine, capture_output=True, text=True, timeout=20, check=check)
        def pasted(mime):
            result = subprocess.run(["wl-paste", "--no-newline", "--type", mime], env=env, capture_output=True, timeout=3)
            return result.stdout if result.returncode == 0 else b""
        try:
            subprocess.run(["wineboot", "-u"], env=wine, stdout=log, stderr=log, timeout=60, check=True)
            host = subprocess.Popen([str(ROOT / "target/debug/waybridge"), "--socket", "virtual-0", "--listen", f"127.0.0.1:{port}", "--token-file", str(token), "--codec", "png"], env=env, stdout=log, stderr=log)
            processes.append(host)
            wait_for(lambda: (root / "virtual-0").exists())
            app = subprocess.Popen([str(ROOT / "build/tests/test-app")], env=env, stdout=app_log, stderr=app_log)
            processes.append(app)
            client = subprocess.Popen(["wine", str(ROOT / "build/windows/waybridge-client.exe"), "127.0.0.1", str(port), token.read_text()], env=wine, stdout=log, stderr=log)
            processes.append(client)
            wait_for(lambda: native("inspect", check=False).returncode == 0, 20)
            cases = [("clipboard", "stream"), ("drop", "stream"), ("drop-sync", "stream"), ("clipboard", "hglobal"), ("drop", "ansi"), ("clipboard", "unknown"), ("drop", "noseek"), ("drop", "pending"),
                     ("drop", "read-fail"), ("drop-sync", "size-bad"), ("clipboard", "badname"), ("drop", "bad-utf16"), ("drop", "truncated"), ("drop", "oversize"), ("clipboard", "nostat"), ("drop", "hglobal-big"), ("drop", "hglobal-unknown"), ("clipboard", "replace"), ("drop-sync", "cancel"), ("drop", "die"), ("drop", "disconnect")]
            retained = []
            if len(sys.argv) > 1:
                cases = [case for case in cases if case[1] in sys.argv[1:]]
            for number, (command, variant) in enumerate(cases):
                baseline = f"virtual baseline {number}".encode()
                owner = subprocess.Popen(["wl-copy", "--foreground", "--type", "text/plain;charset=utf-8"], env=env, stdin=subprocess.PIPE, stdout=log, stderr=log)
                processes.append(owner)
                owner.stdin.write(baseline)
                owner.stdin.close()
                wait_for(lambda: pasted("text/plain;charset=utf-8") == baseline)
                # Allow its native clipboard publication before installing the provider.
                wait_for(lambda: (lambda r: r.returncode == 0 and baseline.decode() in r.stdout)(native("clipboard", check=False)))
                drop = root / "drop"
                drop.unlink(missing_ok=True)
                before = set((root / "waybridge-inbox").glob("tree-*")) if (root / "waybridge-inbox").exists() else set()
                provider = subprocess.Popen(["wine", str(ROOT / "build/tests/windows_virtual_probe.exe"), command, variant], env=wine, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                processes.append(provider)
                actions = []
                if variant == "replace":
                    def replace():
                        try:
                            wait_for(lambda: any(path.is_file() and path.stat().st_size for tree in set((root / "waybridge-inbox").glob("tree-*")) - before for path in tree.rglob("*")))
                            native("clipboard", "replacement clipboard")
                        except BaseException as error:
                            actions.append(error)
                    replacement = threading.Thread(target=replace)
                    replacement.start()
                try:
                    stdout, stderr = provider.communicate(timeout=35)
                except subprocess.TimeoutExpired as error:
                    print("PROVIDER TIMEOUT", command, variant, error.output, error.stderr)
                    raise
                if variant == "replace":
                    replacement.join(timeout=5)
                    assert not replacement.is_alive() and not actions, actions
                failure = variant not in {"stream", "hglobal", "ansi", "unknown", "noseek", "pending"}
                assert provider.returncode == (43 if variant == "die" else 0), (command, variant, provider.returncode, stdout, stderr)
                assert "NO_HDROP" in stdout, stdout
                if not failure:
                    assert "RESULT 1" in stdout, stdout
                    uris = wait_for(lambda: pasted("text/uri-list")) if command == "clipboard" else wait_for(lambda: drop.read_bytes() if drop.exists() and drop.stat().st_size else None)
                    retained.extend(verify(uris, ansi=variant == "ansi"))
                    if variant not in ("hglobal", "noseek"):
                        assert "CLONE " in stdout and "SEEK " in stdout, stdout
                    if variant == "hglobal":
                        assert "GLOBAL " in stdout and "CONTENTS " not in stdout, stdout
                    if variant == "unknown":
                        assert "STAT 0" in stdout, stdout
                    if variant == "noseek":
                        assert "NO_CLONE" in stdout and "NO_SEEK" in stdout, stdout
                    if variant == "pending":
                        assert "PENDING_GET" in stdout and "PENDING_READ" in stdout, stdout
                    if command == "drop-sync":
                        assert "END_SUCCESS" not in stdout and "reads=0" not in stdout, stdout
                    elif command != "clipboard" or "ASYNC_START" in stdout:
                        assert "END_SUCCESS" in stdout, stdout
                else:
                    if variant != "die":
                        assert "RESULT 0" in stdout, stdout
                        if command == "drop" or "ASYNC_START" in stdout:
                            assert "END_FAILURE" in stdout, stdout
                    wait_for(lambda: set((root / "waybridge-inbox").glob("tree-*")) == before)
                    assert not drop.exists(), (command, variant, drop.read_bytes())
                    assert not pasted("text/uri-list"), (command, variant)
                if variant == "disconnect":
                    assert client.wait(timeout=10) == 0
                    assert app.poll() is None and host.poll() is None
                else:
                    assert client.poll() is None and app.poll() is None and host.poll() is None, (command, variant)
                print("PASS:", command, variant, flush=True)
            assert all(path.exists() for path in retained)
            print("PASS: true no-CF_HDROP STA virtual-file clipboard/drop import; cloned/rewound and sequential streams, bounded memory, ANSI/Unicode/empty/implicit directories, async/sync OLE, pending data; source/read/size/name/cancel/disconnect failures leave no partial Linux tree")
        except BaseException:
            print(Path(log.name).read_text())
            print(Path(app_log.name).read_text())
            raise
        finally:
            subprocess.run(["wineserver", "-k"], env=wine, stdout=log, stderr=log, timeout=10)
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


if __name__ == "__main__":
    main()
