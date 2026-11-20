#!/usr/bin/env python3
"""Real tree inbox/export/Wayland drop and lazy native-drag protocol, no desktop."""
import base64
import argparse
import json
import os
from pathlib import Path
import select
import socket
import shutil
import errno
import subprocess
import struct
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


class FileViewer(Viewer):
    """Use separate sockets while keeping file admission and commit ordered on control."""
    def __init__(self, port, token, **_):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.geometry = {}
        super().send(type="hello", version=3, token=token, file_tree=True,
                     file_drop_out=True, file_channel_v1=True)
        hello = self.receive()[1]
        assert hello["type"] == "hello" and hello["file_channel_v1"]

        def attach(key):
            data = socket.create_connection(("127.0.0.1", port), timeout=10)
            self.write(data, dict(type="hello", version=3, token=token, channel="files", media_key=key))
            return data
        with attach("bad-key") as invalid:
            assert invalid.recv(1) == b"", "unbound file socket was accepted"
        self.files = attach(hello["media_key"])
        assert self.receive_files()[1]["type"] == "files_ready"
        with attach(hello["media_key"]) as duplicate:
            assert duplicate.recv(1) == b"", "second file socket was accepted"

    @staticmethod
    def write(sock, message):
        body = b"\0" + json.dumps(message).encode()
        sock.sendall(struct.pack(">I", len(body)) + body)

    def send(self, **message):
        kind = message["type"]
        if kind in ("tree_upload_chunk", "tree_upload_end"):
            self.write(self.files, message)
        elif kind == "tree_upload_commit":
            self.write(self.files, dict(type="tree_upload_barrier", batch=message["batch"]))
            self.until(lambda k, m: k == "control" and m["type"] == "tree_upload_ready"
                       and m["batch"] == message["batch"] and m["stage"] == "data")
            super().send(**message)
        else:
            super().send(**message)

    def receive_files(self):
        def exact(n):
            data = bytearray()
            while len(data) < n:
                piece = self.files.recv(n - len(data))
                if not piece:
                    raise EOFError("file channel disconnected")
                data.extend(piece)
            return data
        size, = struct.unpack(">I", exact(4))
        assert 1 < size <= 2 * 1024 * 1024
        value = exact(size)
        assert value[0] == 0
        return "control", json.loads(value[1:])

    def until_files(self, predicate):
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            kind, message = self.receive_files()
            if predicate(kind, message):
                return message
        raise AssertionError("expected file event did not arrive")

    def close(self):
        self.files.close()
        self.sock.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--file-channel", action="store_true")
    args = parser.parse_args()
    viewer_class = FileViewer if args.file_channel else Viewer
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
                viewer = viewer_class(port, token.read_text(), file_tree=True, file_drop_out=True)
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
                for change in (lambda: (destination / "a/same.bin").write_bytes(b"edit"),
                               lambda: (destination / "empty/new").mkdir(),
                               lambda: (destination / "b/same.bin").unlink(),
                               lambda: (destination / "a/same.bin").replace(destination / "b/same.bin")):
                    try:
                        change()
                        raise AssertionError("received tree remained writable")
                    except OSError as error:
                        assert error.errno == errno.EROFS, error
                saved = runtime / "user-saved"
                shutil.copytree(destination, saved)
                (saved / "a/same.bin").write_bytes(b"saved edit")
                (saved / "empty/new").mkdir()
                assert snapshot(destination) == expected
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
                if args.file_channel:
                    viewer.send(type="file_transfer_rate", kib_per_second=128)
                    viewer.send(type="ping", nonce=72)
                    viewer.until(lambda k, m: k == "control" and m.get("nonce") == 72)
                until_files = viewer.until_files if args.file_channel else viewer.until
                receive_files = viewer.receive_files if args.file_channel else viewer.receive
                copy_tree()
                begin = until_files(lambda k, m: k == "control" and m["type"] == "tree_begin")
                first_chunk = time.monotonic()
                if args.file_channel:
                    viewer.send(type="ping", nonce=73)
                    viewer.until(lambda k, m: k == "control" and m.get("nonce") == 73)
                    assert time.monotonic() - first_chunk < 1, "control waited behind throttled file data"
                assert begin["entries"] == manifest
                received = {e["id"]: bytearray() for e in manifest if e["kind"] == "file"}
                while True:
                    kind, message = receive_files()
                    if kind != "control" or message.get("batch") != begin["batch"]:
                        continue
                    if message["type"] == "tree_chunk":
                        received[message["file"]].extend(base64.b64decode(message["data"]))
                    if message["type"] == "tree_done":
                        break
                for entry in manifest:
                    if entry["kind"] == "file":
                        assert received[entry["id"]] == (source.parent / entry["name"]).read_bytes()
                if args.file_channel:
                    assert time.monotonic() - first_chunk >= 0.9, "download rate limit was not applied"
                    copy_tree()
                    begin = until_files(lambda k, m: k == "control" and m["type"] == "tree_begin")
                    viewer.send(type="file_download_cancel", batch=begin["batch"])
                    viewer.send(type="ping", nonce=74)
                    viewer.until(lambda k, m: k == "control" and m.get("nonce") == 74)
                    while select.select([viewer.files], [], [], 0.3)[0]:
                        _, pending = receive_files()
                        assert pending.get("batch") != begin["batch"] or pending["type"] != "tree_done"
                    viewer.send(type="file_transfer_rate", kib_per_second=0)

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
                        data = until_files(lambda k, m: k == "control" and m["type"] == "drag_data")
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
                if args.file_channel:
                    # Dropping only the file socket must clean incomplete staging
                    # without disconnecting the viewer's control connection.
                    viewer.files.close()
                    viewer.until(lambda k, m: k == "control" and m["type"] == "file_channel_lost")
                    viewer.send(type="ping", nonce=75)
                    viewer.until(lambda k, m: k == "control" and m.get("nonce") == 75)
                viewer.sock.close()
                wait_for(lambda: not (runtime / "waybridge-inbox/tree-703").exists())
                assert (saved / "a/same.bin").read_bytes() == b"saved edit" and (saved / "empty/new").is_dir()
                assert snapshot(source) == expected and snapshot(destination) == expected and snapshot(dropped) == expected
                viewer = viewer_class(port, token.read_text(), file_tree=True, file_drop_out=True)
                viewer.until(lambda k, m: k == "frame")
                upload(501)
                retry_uri = subprocess.check_output(["wl-paste", "--no-newline", "--type", "text/uri-list"], env=env).decode().strip()
                retry = Path(unquote(urlparse(retry_uri).path))
                assert retry != destination and snapshot(retry) == expected and snapshot(destination) == expected
                if args.file_channel: viewer.close()
                else: viewer.sock.close()
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
