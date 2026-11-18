#!/usr/bin/env python3
"""Exercise authenticated launcher icons against an isolated TLS user service."""

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import tempfile
import time
import zlib


def solid_png(color):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    header = struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
            chunk(b"IDAT", zlib.compress(b"\x00" + bytes(color))) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--service", type=Path, default=Path("target/debug/waybridge-service"))
    parser.add_argument("--compositor", type=Path, default=Path("target/debug/waybridge"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="waybridge-icons-smoke-") as temporary:
        root = Path(temporary)
        data = root / "data"
        apps = data / "applications"
        apps.mkdir(parents=True)
        pixmaps = data / "pixmaps"
        pixmaps.mkdir()
        (pixmaps / "named.png").write_bytes(solid_png([255, 0, 0, 255]))
        absolute = root / "file with space.png"
        absolute.write_bytes(solid_png([0, 255, 0, 255]))
        unrelated = root / "unrelated.png"
        unrelated.write_bytes(solid_png([0, 0, 255, 255]))

        def desktop(app_id, icon, extra=""):
            path = apps / (app_id + ".desktop")
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(f"[Desktop Entry]\nType=Application\nName={app_id}\nExec=/bin/true\nIcon={icon}\n{extra}")

        desktop("named", "named")
        desktop("sub/folder", str(absolute).replace(" ", "\\s"))
        desktop("missing", "not-installed")
        desktop("hidden", str(unrelated), "Hidden=true\n")
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        env = dict(os.environ, HOME=str(root), XDG_DATA_HOME=str(data), XDG_DATA_DIRS=str(root / "empty"),
                   XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(root / "config"))
        command = [str(args.service.resolve()), "--runtime-dir", str(runtime / "entry"),
                   "--state-dir", str(root / "state")]
        daemon = subprocess.Popen([*command, "serve", "--listen", "127.0.0.1:0", "--compositor", str(args.compositor.resolve())],
                                  env=env, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            ready_path = runtime / "entry/ready.json"
            deadline = time.monotonic() + 10
            while not ready_path.exists():
                if daemon.poll() is not None:
                    raise AssertionError(daemon.stderr.read().decode())
                if time.monotonic() > deadline:
                    raise TimeoutError("TLS service did not start")
                time.sleep(0.02)
            ready = json.loads(ready_path.read_text())
            host, port = ready["listen"].rsplit(":", 1)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE

            def rpc(request):
                with context.wrap_socket(socket.create_connection((host, int(port)), timeout=15), server_hostname="waybridge.local") as connection:
                    assert hashlib.sha256(connection.getpeercert(binary_form=True)).hexdigest() == ready["certificate_sha256"]
                    connection.sendall(json.dumps(request).encode() + b"\n")
                    with connection.makefile("rb") as stream:
                        line = stream.readline(4 * 1024 * 1024 + 1)
                    assert len(line) <= 4 * 1024 * 1024 and line.endswith(b"\n")
                    return json.loads(line)

            request = {"op": "app_icons", "apps": ["named.desktop", "sub-folder.desktop", "missing.desktop",
                                                      "hidden.desktop", "unknown.desktop"]}
            assert "app_icons" in rpc({"op": "hello"})["result"]["features"]
            assert rpc(request)["error"]["code"] == "unauthorized"
            pair = json.loads(subprocess.check_output([*command, "pair"], env=env))["result"]
            credentials = rpc({"op": "pair", "pair_code": pair["pair_code"], "label": "Launcher test"})["result"]
            authorization = {key: credentials[key] for key in ("client_id", "client_token")}
            reply = rpc({**request, **authorization, "binary": "/bin/false", "icon": str(unrelated)})
            assert reply["ok"], reply
            result = reply["result"]
            assert not result["omitted"]
            assert set(result["icons"]) == set(request["apps"])
            hashes = []
            for app_id in ("named.desktop", "sub-folder.desktop"):
                icon = result["icons"][app_id]
                png = base64.b64decode(icon["png"], validate=True)
                assert png[:8] == b"\x89PNG\r\n\x1a\n"
                assert struct.unpack(">II", png[16:24]) == (128, 128)
                assert icon["sha256"] == hashlib.sha256(png).hexdigest()
                hashes.append(icon["sha256"])
            assert hashes[0] != hashes[1]
            for app_id in ("missing.desktop", "hidden.desktop", "unknown.desktop"):
                assert result["icons"][app_id] is None
            assert rpc({**request, **authorization})["result"] == result
            for malformed in (["../named.desktop"], ["named.desktop"] * 2,
                              [f"test-{item}.desktop" for item in range(25)], ["named.desktop", 1]):
                assert not rpc({"op": "app_icons", "apps": malformed, **authorization})["ok"]
            print("PASS: authenticated launcher icon batches, exact Desktop IDs, PNG dimensions and hash, safe fallback and bounds")
        finally:
            daemon.terminate()
            daemon.wait(timeout=10)


if __name__ == "__main__":
    main()
