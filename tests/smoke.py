#!/usr/bin/env python3
"""Exercise the real compositor, real Wayland clients, and the viewer protocol."""
import base64
import io
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]


class Viewer:
    def __init__(self, port, token, dpi=96, file_tree=False, file_drop_out=False, data_drag_v1=False, **hello):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.geometry = {}
        self.send(type="hello", version=3, token=token, dpi=dpi, file_tree=file_tree, file_drop_out=file_drop_out, data_drag_v1=data_drag_v1, **hello)
        self.hello = self.receive()[1]
        assert self.hello["type"] == "hello"

    def send(self, **message):
        if message.get("type") in ("pointer", "button", "upload_commit") and message.get("id") in self.geometry:
            _, _, x, y = self.geometry[message["id"]]
            message.setdefault("origin_x", x)
            message.setdefault("origin_y", y)
        body = b"\0" + json.dumps(message).encode()
        self.sock.sendall(struct.pack(">I", len(body)) + body)

    def exact(self, size):
        out = bytearray()
        while len(out) < size:
            piece = self.sock.recv(size - len(out))
            if not piece:
                raise EOFError("viewer disconnected")
            out.extend(piece)
        return out

    def receive(self):
        size, = struct.unpack(">I", self.exact(4))
        assert 0 < size <= 72 * 1024 * 1024
        body = self.exact(size)
        if body[0] == 0:
            return "control", json.loads(body[1:])
        assert body[0] == 1 and len(body) >= 21
        window, width, height, seq = struct.unpack(">IIIQ", body[1:21])
        self.geometry[window] = struct.unpack(">iiii", body[21:37])
        image = Image.open(io.BytesIO(body[37:])).convert("RGBA")
        assert image.size == (width, height)
        return "frame", (window, width, height, seq, image)

    def until(self, predicate):
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            packet = self.receive()
            if predicate(*packet):
                return packet[1]
        raise AssertionError("expected event did not arrive")


def main():
    subprocess.run(["bash", "tests/build-fixture.sh"], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="waybridge-test-") as runtime:
        env = {**os.environ, "XDG_RUNTIME_DIR": runtime, "WAYLAND_DISPLAY": "test-0", "WAYBRIDGE_TEST_DROP": str(Path(runtime)/"drop.txt")}
        with socket.socket() as free:
            free.bind(("127.0.0.1", 0))
            port = free.getsockname()[1]
        token = "test-token-" + "7" * 40
        token_path = Path(runtime) / "token"
        token_path.write_text(token)
        token_path.chmod(0o600)
        host_log = open(ROOT / "build/tests/host.log", "w+")
        app_log = open(ROOT / "build/tests/app.log", "w+")
        processes = []
        try:
            host = subprocess.Popen([str(ROOT / "target/debug/waybridge"), "--listen", f"127.0.0.1:{port}", "--socket", "test-0", "--token-file", str(token_path)], env=env, stdout=host_log, stderr=subprocess.STDOUT)
            processes.append(host)
            deadline = time.monotonic() + 10
            while not (Path(runtime) / "test-0").exists():
                assert host.poll() is None, "host exited; see build/tests/host.log"
                assert time.monotonic() < deadline
                time.sleep(0.05)
            # An unauthenticated peer must not acquire a session or learn window data.
            bad = socket.create_connection(("127.0.0.1", port), timeout=3)
            payload = b'\0{"type":"hello","version":3,"token":"wrong"}'
            bad.sendall(struct.pack(">I", len(payload)) + payload)
            assert bad.recv(1) == b""
            bad.close()
            viewer = Viewer(port, token)
            app = subprocess.Popen([str(ROOT / "build/tests/test-app"), "two"], env=env, stdout=app_log, stderr=subprocess.STDOUT)
            processes.append(app)
            ids, seen = set(), set()
            while len(seen) < 2:
                kind, value = viewer.receive()
                if kind == "control" and value["type"] == "window":
                    ids.add(value["id"])
                if kind == "frame":
                    window, w, h, _, image = value
                    top, bottom = image.getpixel((w//2,h//4)), image.getpixel((w//2,h*3//4))
                    assert top[0] > 220 and top[2] < 30, ("top must be red",top)
                    assert bottom[2] > 220 and bottom[0] < 30, ("bottom must be blue",bottom)
                    seen.add(window)
                    image.save(ROOT / "build/tests/frame.png")
            assert ids == seen
            first = min(ids)
            viewer.send(type="resize", id=first, width=640, height=480)
            viewer.until(lambda kind, v: kind == "frame" and v[0:3] == (first,640,480))
            viewer.send(type="focus", id=first, focused=True)
            viewer.send(type="pointer", id=first, x=120.0, y=80.0)
            viewer.send(type="button", id=first, button=0x110, down=True)
            viewer.send(type="button", id=first, button=0x110, down=False)
            viewer.send(type="key", id=first, key=30, down=True)
            # Deliberately disconnect with a key held; release must be delivered.
            viewer.sock.close()
            time.sleep(0.3)
            app_log.flush()
            log = Path(app_log.name).read_text()
            assert "ENTER 120.0 80.0" in log, log
            assert "BUTTON 272 1" in log and "BUTTON 272 0" in log, log
            assert "KEY 30 1" in log and "KEY 30 0" in log, log
            viewer = Viewer(port,token)
            restored=set()
            while len(restored)<2:
                kind,v=viewer.receive()
                if kind=="control" and v["type"]=="window": restored.add(v["id"])
            assert restored==ids
            # Clipboard Windows-protocol -> Wayland, Unicode and exact PNG bytes.
            text="Waybridge 你好 ✓\nsecond line"
            viewer.send(type="clipboard", items={"text/plain;charset=utf-8":base64.b64encode(text.encode()).decode()})
            time.sleep(0.1)
            pasted=subprocess.run(["wl-paste","--no-newline","--type","text/plain;charset=utf-8"],env=env,capture_output=True,timeout=5,check=True).stdout
            assert pasted.decode()==text
            png_io=io.BytesIO();Image.new("RGBA",(13,7),(20,80,200,120)).save(png_io,format="PNG");png=png_io.getvalue()
            viewer.send(type="clipboard",items={"image/png":base64.b64encode(png).decode()})
            time.sleep(0.1)
            pasted=subprocess.run(["wl-paste","--no-newline","--type","image/png"],env=env,capture_output=True,timeout=5,check=True).stdout
            assert pasted==png
            # Clipboard Wayland -> Windows-protocol, real wl-copy process.
            copier=subprocess.Popen(["wl-copy","--foreground","--type","text/plain;charset=utf-8"],stdin=subprocess.PIPE,env=env)
            processes.append(copier);copier.stdin.write(text.encode());copier.stdin.close()
            clip=viewer.until(lambda k,v:k=="control" and v["type"]=="clipboard" and "text/plain;charset=utf-8" in v["items"])
            assert base64.b64decode(clip["items"]["text/plain;charset=utf-8"]).decode()==text
            copier.terminate();copier.wait(timeout=3)
            copier=subprocess.Popen(["wl-copy","--foreground","--type","image/png"],stdin=subprocess.PIPE,env=env)
            processes.append(copier);copier.stdin.write(png);copier.stdin.close()
            clip=viewer.until(lambda k,v:k=="control" and v["type"]=="clipboard" and "image/png" in v["items"])
            assert base64.b64decode(clip["items"]["image/png"])==png
            # Files are materialized on the destination, with exact bytes and original names.
            payload=os.urandom(180_000)
            viewer.send(type="upload_begin",transfer=101,name="图片 data.bin",size=len(payload))
            for offset in range(0,len(payload),65536):
                viewer.send(type="upload_chunk",transfer=101,data=base64.b64encode(payload[offset:offset+65536]).decode())
            viewer.send(type="upload_end",transfer=101)
            viewer.send(type="upload_commit",transfers=[101])
            viewer.until(lambda k,v:k=="control" and v["type"]=="upload_ready")
            uris=subprocess.run(["wl-paste","--no-newline","--type","text/uri-list"],env=env,capture_output=True,timeout=5,check=True).stdout.decode()
            from urllib.parse import urlparse,unquote
            local=Path(unquote(urlparse(uris.strip()).path))
            assert local.name=="图片 data.bin" and local.read_bytes()==payload
            viewer.send(type="upload_begin",transfer=102,name="../escape",size=0)
            viewer.until(lambda k,v:k=="control" and v["type"]=="error")
            assert not (Path(runtime)/"escape").exists()
            source=Path(runtime)/"download 图.bin";source.write_bytes(payload)
            copier=subprocess.Popen(["wl-copy","--foreground","--type","text/uri-list"],stdin=subprocess.PIPE,env=env)
            processes.append(copier);copier.stdin.write((source.as_uri()+"\r\n").encode());copier.stdin.close()
            begin=viewer.until(lambda k,v:k=="control" and v["type"]=="files_begin")
            assert begin["files"][0]["name"]==source.name
            received=bytearray()
            while True:
                kind,message=viewer.receive()
                if kind!="control":continue
                if message["type"]=="file_chunk":received.extend(base64.b64decode(message["data"]))
                if message["type"]=="files_done":break
            assert received==payload
            viewer.send(type="upload_begin",transfer=103,name="dropped.bin",size=len(payload))
            for offset in range(0,len(payload),65536):
                viewer.send(type="upload_chunk",transfer=103,data=base64.b64encode(payload[offset:offset+65536]).decode())
            viewer.send(type="upload_end",transfer=103)
            viewer.send(type="upload_commit",transfers=[103],drop=True,id=first,x=100,y=70)
            viewer.until(lambda k,v:k=="control" and v["type"]=="upload_ready")
            drop_file=Path(runtime)/"drop.txt"
            deadline=time.monotonic()+5
            while not drop_file.exists() or not drop_file.stat().st_size:
                assert time.monotonic()<deadline, "Wayland drop did not complete"
                time.sleep(0.05)
            dropped=Path(unquote(urlparse(drop_file.read_text().strip()).path))
            assert dropped.name=="dropped.bin" and dropped.read_bytes()==payload
            viewer.send(type="close",id=first)
            viewer.until(lambda k,v:k=="control" and v["type"]=="window_closed" and v["id"]==first)
            assert host.poll() is None and app.poll() is None
            viewer.sock.close()
            print("PASS: authentication, 2 independent windows, pixel colors/orientation, real resize, pointer/keyboard, stuck-key release, reconnect, bidirectional Unicode/PNG clipboard, bidirectional chunked files, Wayland file drop, path traversal rejection, close")
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=3)
                    except subprocess.TimeoutExpired: process.kill();process.wait()
            host_log.close();app_log.close()


if __name__ == "__main__":
    main()
