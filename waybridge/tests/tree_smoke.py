#!/usr/bin/env python3
"""Real tree inbox/export/Wayland drop and lazy native-drag protocol, no desktop."""
import base64
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from urllib.parse import unquote, urlparse
from drag_out_smoke import wait_for
from smoke import ROOT, Viewer


def fixture(root):
    (root / "empty").mkdir(parents=True)
    (root / "a").mkdir()
    (root / "b").mkdir()
    (root / "a/same.bin").write_bytes(os.urandom(180_000))
    (root / "b/same.bin").write_bytes(b"other directory")
    (root / "零.txt").write_bytes(b"")


def snapshot(root):
    return {str(p.relative_to(root)): None if p.is_dir() else p.read_bytes() for p in root.rglob("*")}


def entries(root):
    out = []
    def visit(path, name):
        out.append(dict(id=len(out), name=name, kind="directory" if path.is_dir() else "file", size=0 if path.is_dir() else path.stat().st_size))
        if path.is_dir():
            for child in sorted(path.iterdir()):
                visit(child, name + "/" + child.name)
    visit(root, root.name)
    return out


def main():
    subprocess.run(["bash", "tests/build-fixture.sh"], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="waybridge-tree-") as directory:
        runtime = Path(directory)
        source = runtime / "源树"
        fixture(source)
        expected = snapshot(source)
        manifest = entries(source)
        uris = runtime / "uris"
        uris.write_text(source.as_uri() + "\r\n")
        token = runtime / "token"
        token.write_text("tree-smoke-" + "7" * 40)
        token.chmod(0o600)
        env = {**os.environ, "XDG_RUNTIME_DIR": directory, "WAYLAND_DISPLAY": "tree-0", "WAYBRIDGE_TEST_DROP": str(runtime / "drop"), "WAYBRIDGE_TEST_DRAG_URIS": str(uris), "WAYBRIDGE_TEST_DRAG_ENABLED": str(runtime / "enable-drag")}
        with socket.socket() as free:
            free.bind(("127.0.0.1", 0))
            port = free.getsockname()[1]
        processes = []
        with open(runtime / "host.log", "w+") as host_log, open(runtime / "app.log", "w+") as app_log:
            try:
                host = subprocess.Popen([str(ROOT / "target/debug/waybridge"), "--socket", "tree-0", "--listen", f"127.0.0.1:{port}", "--token-file", str(token), "--codec", "png"], env=env, stdout=host_log, stderr=host_log)
                processes.append(host)
                wait_for(lambda: (runtime / "tree-0").exists())
                old = Viewer(port, token.read_text())
                old.send(type="tree_upload_begin", batch=1, entries=manifest)
                old.until(lambda k, m: k == "control" and m["type"] == "tree_upload_error")
                old.sock.close()
                time.sleep(0.1)
                viewer = Viewer(port, token.read_text(), file_tree=True, file_drop_out=True)
                app = subprocess.Popen([str(ROOT / "build/tests/test-app")], env=env, stdout=app_log, stderr=app_log)
                processes.append(app)
                window = viewer.until(lambda k, m: k == "control" and m["type"] == "window")["id"]
                viewer.until(lambda k, m: k == "frame" and m[0] == window)

                def upload(batch, drop=False):
                    viewer.send(type="tree_upload_begin", batch=batch, entries=manifest, drop=drop, epoch=0)
                    viewer.until(lambda k, m: k == "control" and m["type"] == "tree_upload_ready" and m["batch"] == batch)
                    for entry in manifest:
                        if entry["kind"] != "file":
                            continue
                        data = (source.parent / entry["name"]).read_bytes()
                        for offset in range(0, len(data), 65536):
                            viewer.send(type="tree_upload_chunk", batch=batch, file=entry["id"], data=base64.b64encode(data[offset:offset + 65536]).decode())
                        viewer.send(type="tree_upload_end", batch=batch, file=entry["id"])
                    viewer.send(type="tree_upload_commit", batch=batch, drop=drop, id=window, x=50, y=70)
                    viewer.until(lambda k, m: k == "control" and m["type"] == "tree_upload_ready" and m["batch"] == batch and m["stage"] == "commit")

                upload(501)
                pasted = subprocess.check_output(["wl-paste", "--no-newline", "--type", "text/uri-list"], env=env).decode().strip()
                destination = Path(unquote(urlparse(pasted).path))
                assert destination.name == source.name and snapshot(destination) == expected
                upload(502, drop=True)
                wait_for(lambda: (runtime / "drop").exists() and (runtime / "drop").stat().st_size)
                dropped = Path(unquote(urlparse((runtime / "drop").read_text().strip()).path))
                assert snapshot(dropped) == expected

                # Full clipboard export includes explicit empty directories and only requested bytes.
                def copy_tree():
                    p = subprocess.Popen(["wl-copy", "--foreground", "--type", "text/uri-list"], env=env, stdin=subprocess.PIPE, stdout=host_log, stderr=host_log)
                    processes.append(p)
                    p.stdin.write((source.as_uri() + "\r\n").encode())
                    p.stdin.close()
                    return p
                copy_tree()
                begin = viewer.until(lambda k, m: k == "control" and m["type"] == "tree_begin")
                assert begin["entries"] == manifest
                received = {e["id"]: bytearray() for e in manifest if e["kind"] == "file"}
                while True:
                    kind, message = viewer.receive()
                    if kind != "control" or message.get("batch") != begin["batch"]:
                        continue
                    if message["type"] == "tree_chunk":
                        received[message["file"]].extend(base64.b64decode(message["data"]))
                    if message["type"] == "tree_done":
                        break
                for entry in manifest:
                    if entry["kind"] == "file":
                        assert received[entry["id"]] == (source.parent / entry["name"]).read_bytes()

                (runtime / "enable-drag").touch()
                viewer.send(type="pointer", id=window, x=50, y=70)
                viewer.send(type="button", id=window, button=272, down=True)
                offer = viewer.until(lambda k, m: k == "control" and m["type"] == "drag_offer")
                assert offer["tree"] and offer["files"] == manifest
                viewer.send(type="drag_claim", drag=offer["drag"])
                viewer.until(lambda k, m: k == "control" and m["type"] == "drag_ready")
                for entry in manifest:
                    if entry["kind"] == "file" and entry["size"]:
                        viewer.send(type="drag_read", drag=offer["drag"], request=entry["id"], file=entry["id"], offset=0, length=min(entry["size"], 65536))
                        data = viewer.until(lambda k, m: k == "control" and m["type"] == "drag_data")
                        assert base64.b64decode(data["data"]) == (source.parent / entry["name"]).read_bytes()[:65536]
                viewer.send(type="drag_drop", drag=offer["drag"])
                viewer.send(type="drag_result", drag=offer["drag"], success=True)
                viewer.until(lambda k, m: k == "control" and m["type"] == "drag_end")
                wait_for(lambda: "SOURCE_FINISHED" in (runtime / "app.log").read_text())

                # Reject untrusted paths, collisions and incomplete commits atomically.
                invalid = ["../escape", "/abs", "C:x", "r\\x", "x:stream", "NUL.txt", "CON .txt", "x.", "x "]
                for i, name in enumerate(invalid, 600):
                    viewer.send(type="tree_upload_begin", batch=i, entries=[dict(id=0, name=name, kind="directory", size=0)])
                    viewer.until(lambda k, m: k == "control" and m["type"] == "tree_upload_error" and m["batch"] == i)
                    assert not (runtime / "waybridge-inbox" / f"tree-{i}").exists()
                bad = [dict(id=0, name="r", kind="directory", size=0), dict(id=1, name="r/A", kind="file", size=0), dict(id=2, name="r/a", kind="file", size=0)]
                viewer.send(type="tree_upload_begin", batch=700, entries=bad)
                viewer.until(lambda k, m: k == "control" and m["type"] == "tree_upload_error" and m["batch"] == 700)
                viewer.send(type="tree_upload_begin", batch=701, entries=manifest)
                viewer.until(lambda k, m: k == "control" and m["type"] == "tree_upload_ready" and m["batch"] == 701)
                viewer.send(type="tree_upload_commit", batch=701)
                viewer.until(lambda k, m: k == "control" and m["type"] == "tree_upload_error" and m["batch"] == 701)
                assert not (runtime / "waybridge-inbox/tree-701").exists()
                viewer.send(type="tree_upload_begin", batch=702, entries=manifest)
                viewer.until(lambda k, m: k == "control" and m["type"] == "tree_upload_ready" and m["batch"] == 702)
                viewer.send(type="tree_upload_cancel", batch=702)
                wait_for(lambda: not (runtime / "waybridge-inbox/tree-702").exists())
                # A newer local Windows clipboard cannot commit an old upload.
                only_directory = [dict(id=0, name="empty-root", kind="directory", size=0)]
                viewer.send(type="tree_upload_begin", batch=704, epoch=0, entries=only_directory)
                viewer.until(lambda k, m: k == "control" and m["type"] == "tree_upload_ready" and m["batch"] == 704)
                viewer.send(type="file_tree_epoch", epoch=1)
                viewer.send(type="tree_upload_commit", batch=704)
                viewer.until(lambda k, m: k == "control" and m["type"] == "tree_upload_error" and m["batch"] == 704)
                assert not (runtime / "waybridge-inbox/tree-704").exists()
                viewer.send(type="tree_upload_begin", batch=705, epoch=0, entries=only_directory)
                viewer.until(lambda k, m: k == "control" and m["type"] == "tree_upload_error" and m["batch"] == 705)
                assert not (runtime / "waybridge-inbox/tree-705").exists()
                viewer.send(type="tree_upload_begin", batch=706, drop=True, entries=only_directory)
                viewer.until(lambda k, m: k == "control" and m["type"] == "tree_upload_ready" and m["batch"] == 706)
                viewer.send(type="tree_upload_commit", batch=706, drop=False)
                viewer.until(lambda k, m: k == "control" and m["type"] == "tree_upload_error" and m["batch"] == 706)
                assert not (runtime / "waybridge-inbox/tree-706").exists()
                viewer.send(type="tree_upload_begin", batch=703, entries=manifest)
                viewer.until(lambda k, m: k == "control" and m["type"] == "tree_upload_ready" and m["batch"] == 703)
                viewer.sock.close()
                wait_for(lambda: not (runtime / "waybridge-inbox/tree-703").exists())
                assert snapshot(source) == expected and snapshot(destination) == expected and snapshot(dropped) == expected
                assert host.poll() is None and app.poll() is None
                print("PASS: negotiated real directory tree clipboard/drop/export/lazy drag; empty/Unicode/zero/same-basename entries; traversal/ADS/devices/case collision rejection; incomplete/cancel/disconnect cleanup; completed trees and source retained")
            except BaseException:
                print((runtime / "host.log").read_text())
                print((runtime / "app.log").read_text())
                raise
            finally:
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
