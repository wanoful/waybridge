#!/usr/bin/env python3
"""Native OLE virtual-file COPY, in a private Wine/Xvfb desktop and prefix."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import sys
from urllib.parse import unquote, urlparse
from tree_smoke import fixture, snapshot, entries
from drag_out_smoke import wait_for
from smoke import ROOT


def main():
    tree = "--tree" in sys.argv
    subprocess.run(["bash", "tests/build-fixture.sh"], cwd=ROOT, check=True)
    for name in ("windows_probe", "windows_drag_out_probe", "windows_tree_probe"):
        subprocess.run(["x86_64-w64-mingw32-g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-municode", "-static", "-O2", f"tests/{name}.cpp", "-o", f"build/tests/{name}.exe", "-lgdi32", "-lshell32", "-lole32", "-luuid", "-lcrypt32"], cwd=ROOT, check=True)
    prefix = ROOT / "build/wine-drag-out"
    prefix.mkdir(parents=True, mode=0o700, exist_ok=True)
    processes = []
    with tempfile.TemporaryDirectory(prefix="waybridge-native-drag-") as directory, open(ROOT / "build/tests/drag-native.log", "w+") as log, open(ROOT / "build/tests/drag-native-app.log", "w+") as app_log:
        root = Path(directory)
        read, write = os.pipe()
        xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write), "-screen", "0", "1280x1024x24", "-nolisten", "tcp"], pass_fds=[write], stdout=log, stderr=log)
        processes.append(xvfb)
        os.close(write)
        with os.fdopen(read) as pipe:
            display = ":" + pipe.readline().strip()
        wine = {**os.environ, "DISPLAY": display, "WINEPREFIX": str(prefix), "WINEDEBUG": "-all", "WINEDLLOVERRIDES": "winemenubuilder.exe=d;mscoree,mshtml=", "WAYBRIDGE_GDI": "1"}
        payload = os.urandom(180_000)
        sources = [root / "图片 data.bin", root / "empty.txt"]
        sources[0].write_bytes(payload)
        sources[1].write_bytes(b"")
        if tree:
            tree_source = root / "目录树"
            fixture(tree_source)
            sources = [tree_source]
            expected = snapshot(tree_source)
            descriptor_count = len(entries(tree_source))
        uris = root / "uris"
        uris.write_text("".join(p.as_uri() + "\r\n" for p in sources))
        env = {**os.environ, "XDG_RUNTIME_DIR": directory, "WAYLAND_DISPLAY": "drag-native-0", "WAYBRIDGE_TEST_DRAG_URIS": str(uris)}
        if tree:
            env["WAYBRIDGE_TEST_DROP"] = str(root / "drop.txt")
            env["WAYBRIDGE_TEST_DRAG_ENABLED"] = str(root / "enable-drag")
        with socket.socket() as free:
            free.bind(("127.0.0.1", 0))
            port = free.getsockname()[1]
        token = root / "token"
        token.write_text("native-drag-" + "9" * 40)
        token.chmod(0o600)
        def probe(*args, check=True):
            return subprocess.run(["wine", str(ROOT / "build/tests/windows_probe.exe"), *args], env=wine, capture_output=True, text=True, check=check, timeout=20)
        try:
            subprocess.run(["wineboot", "-u"], env=wine, stdout=log, stderr=log, timeout=60, check=True)
            if tree:
                checks = subprocess.run(["wine", str(ROOT / "build/tests/windows_tree_probe.exe")], env=wine, capture_output=True, text=True, timeout=15, check=True)
                assert "PASS: Win32 tree" in checks.stdout, checks.stdout
            host = subprocess.Popen([str(ROOT / "target/debug/waybridge"), "--socket", "drag-native-0", "--listen", f"127.0.0.1:{port}", "--token-file", str(token), "--codec", "png"], env=env, stdout=log, stderr=log)
            processes.append(host)
            wait_for(lambda: (root / "drag-native-0").exists())
            app = subprocess.Popen([str(ROOT / "build/tests/test-app")], env=env, stdout=app_log, stderr=app_log)
            processes.append(app)
            client = subprocess.Popen(["wine", str(ROOT / "build/windows/waybridge-client.exe"), "127.0.0.1", str(port), token.read_text()], env=wine, stdout=log, stderr=log)
            processes.append(client)
            wait_for(lambda: probe("inspect", check=False).returncode == 0, 20)
            if tree:
                win_source = "Z:" + str(tree_source).replace("/", "\\")
                probe("copyfile", win_source)
                def linux_tree():
                    result = subprocess.run(["wl-paste", "--no-newline", "--type", "text/uri-list"], env=env, capture_output=True, timeout=3)
                    return Path(unquote(urlparse(result.stdout.decode().strip()).path)) if result.returncode == 0 else None
                uploaded = wait_for(lambda: (lambda p: p if p and p.name == tree_source.name and p.exists() else None)(linux_tree()))
                assert snapshot(uploaded) == expected
                probe("drop", win_source)
                wait_for(lambda: (root / "drop.txt").exists() and (root / "drop.txt").stat().st_size)
                dropped = Path(unquote(urlparse((root / "drop.txt").read_text().strip()).path))
                assert snapshot(dropped) == expected
                copier = subprocess.Popen(["wl-copy", "--foreground", "--type", "text/uri-list"], env=env, stdin=subprocess.PIPE, stdout=log, stderr=log)
                processes.append(copier)
                copier.stdin.write((tree_source.as_uri() + "\r\n").encode()); copier.stdin.close()
                path = wait_for(lambda: (lambda r: r.stdout.strip() if r.returncode == 0 and "Waybridge-tree-" in r.stdout else None)(probe("files", check=False)))
                staged = Path(subprocess.check_output(["winepath", "-u", path], env=wine, text=True).strip())
                assert snapshot(staged) == expected
                (root / "enable-drag").touch()
            probe("clipboard", "drag clipboard sentinel")
            finished = cancelled = 0
            for mode in ("sync", "async", "cancel", "reject", "early"):
                output = root / mode
                output.mkdir()
                result = subprocess.run(["wine", str(ROOT / "build/tests/windows_drag_out_probe.exe"), "Z:" + str(output).replace("/", "\\"), mode], env=wine, capture_output=True, text=True, timeout=30)
                assert result.returncode == 0, (mode, result.stdout, result.stderr)
                if mode in ("sync", "async"):
                    finished += 1
                    if tree:
                        assert snapshot(output / tree_source.name) == expected
                        assert f"EXTRACTED {descriptor_count}" in result.stdout, result.stdout
                    else:
                        assert (output / sources[0].name).read_bytes() == payload
                        assert (output / sources[1].name).read_bytes() == b""
                        assert "EXTRACTED 2" in result.stdout, result.stdout
                    if mode == "async":
                        assert "ASYNC_STARTED" in result.stdout
                else:
                    cancelled += 1
                    assert not list(output.iterdir())
                wait_for(lambda: Path(app_log.name).read_text().count("SOURCE_FINISHED") == finished and Path(app_log.name).read_text().count("SOURCE_CANCELLED") == cancelled)
                assert client.poll() is None and host.poll() is None and app.poll() is None
            assert "drag clipboard sentinel" in probe("clipboard").stdout
            if tree:
                assert snapshot(tree_source) == expected and snapshot(uploaded) == expected and snapshot(dropped) == expected and snapshot(staged) == expected
            else:
                assert sources[0].read_bytes() == payload and sources[1].exists()
            # Disconnect while OLE owns capture and the real mouse is still down.
            output = root / "disconnect"
            output.mkdir()
            with open(root / "disconnect.log", "w+") as drop_log:
                drop = subprocess.Popen(["wine", str(ROOT / "build/tests/windows_drag_out_probe.exe"), "Z:" + str(output).replace("/", "\\"), "disconnect"], env=wine, stdout=drop_log, stderr=drop_log)
                processes.append(drop)
                wait_for(lambda: "ENTER 1" in (root / "disconnect.log").read_text())
                host.terminate()
                host.wait(timeout=5)
                client.wait(timeout=2)
                assert drop.wait(timeout=8) == 0
            if tree:
                print("PASS: native directory clipboard in both directions and Windows OLE folder drop in/out; relative virtual directory descriptors, empty/Unicode/zero/same-basename entries, sync/async extraction; completed trees retained")
            print("PASS: actual Windows PE OLE drag source -> native virtual-file target; Unicode/multiple/empty files; IStream Stat/Seek/Clone/EOF; async extraction; Esc/rejection/early-release/disconnect; clipboard and source files preserved (Wine/Xvfb)")
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
