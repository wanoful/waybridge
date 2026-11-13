#!/usr/bin/env python3
"""Real CNG reorder-capacity, twenty-window UDP backpressure and cancellation regressions.

Run: python tests/udp_flow_smoke.py
Needs the normal test dependencies plus Wine, Xvfb, MinGW and OpenSSL development files.
All applications use private Wayland sockets and Wine uses its own Xvfb/prefix.
"""
import base64
from contextlib import contextmanager
import ctypes
import io
import json
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import tempfile
import threading
import time

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/tests"
PROBE = ROOT / "build/windows/waybridge-udp-flow-probe.exe"
MAX_PACKET = 72 * 1024 * 1024


def wait(check, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.02)
    raise AssertionError("UDP flow test timed out; see build/tests/udp-flow.log")


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as stream:
        stream.bind(("127.0.0.1", 0))
        return stream.getsockname()[1]


def exact(stream, size):
    output = bytearray()
    while len(output) < size:
        part = stream.recv(size - len(output))
        if not part:
            raise EOFError()
        output.extend(part)
    return bytes(output)


def read_packet(stream):
    size, = struct.unpack(">I", exact(stream, 4))
    assert 0 < size <= MAX_PACKET
    return exact(stream, size)


def send_json(stream, **value):
    body = b"\0" + json.dumps(value).encode()
    stream.sendall(struct.pack(">I", len(body)) + body)


class Crypto:
    def __init__(self):
        self.library = ctypes.CDLL(str(BUILD / "udp-crypto-fixture.so"))
        self.library.udp_test_gcm.argtypes = [
            ctypes.c_int, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
            ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p,
        ]
        self.library.udp_test_gcm.restype = ctypes.c_int

    def crypt(self, encrypt, key, header, body):
        count = len(body) if encrypt else len(body) - 16
        assert count >= 0 and len(header) == 20
        output = ctypes.create_string_buffer(max(count, 1))
        tag = ctypes.create_string_buffer(16) if encrypt else ctypes.create_string_buffer(body[-16:])
        nonce = b"\0" * 4 + header[12:20]
        written = self.library.udp_test_gcm(
            int(encrypt), key, nonce, header, body[:count], count, output, tag,
        )
        assert written == count, "AES-GCM fixture authentication failed"
        return output.raw[:count] + (tag.raw if encrypt else b"")


class DelayProxy:
    """Delay the first control NACK by 600 ms, preserving TCP bytes and ordering."""
    def __init__(self, address):
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen()
        self.listener.settimeout(0.1)
        self.port = self.listener.getsockname()[1]
        self.closed = threading.Event()
        self.offer_ready = threading.Event()
        self.offer = None
        self.retry_started = None
        self.sockets = []
        self.workers = []
        self.errors = []
        self.thread = threading.Thread(target=self.accept, args=(address,))
        self.thread.start()

    def accept(self, address):
        while not self.closed.is_set():
            try:
                client, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                server = socket.create_connection(address, timeout=15)
            except OSError as error:
                client.close()
                self.errors.append(error)
                continue
            self.sockets.extend((client, server))
            for source, target, upstream in ((client, server, True), (server, client, False)):
                worker = threading.Thread(target=self.forward, args=(source, target, upstream))
                self.workers.append(worker)
                worker.start()

    def forward(self, source, target, upstream):
        try:
            while not self.closed.is_set():
                body = read_packet(source)
                if body[0] == 0:
                    value = json.loads(body[1:])
                    if not upstream and isinstance(value.get("udp_fec_v1"), dict):
                        self.offer = value["udp_fec_v1"]
                        self.offer_ready.set()
                    if upstream and value.get("type") == "udp_retry" and self.retry_started is None:
                        self.retry_started = time.monotonic()
                        self.closed.wait(0.6)
                target.sendall(struct.pack(">I", len(body)) + body)
        except (EOFError, OSError):
            pass
        except Exception as error:
            self.errors.append(error)
        finally:
            try:
                target.shutdown(socket.SHUT_WR)
            except OSError:
                pass

    def close(self):
        self.closed.set()
        self.listener.close()
        self.thread.join(timeout=3)
        for stream in self.sockets:
            try:
                stream.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            stream.close()
        for worker in self.workers:
            worker.join(timeout=3)
            assert not worker.is_alive(), "TCP proxy thread leaked"
        assert not self.thread.is_alive(), "TCP proxy listener leaked"


class LossProxy:
    """Drop every authenticated shard of message 1 and forward all later messages."""
    def __init__(self, server, control, crypto):
        self.stream = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.stream.bind(("127.0.0.1", 0))
        self.stream.settimeout(0.1)
        self.port = self.stream.getsockname()[1]
        self.closed = threading.Event()
        self.events = []
        self.dropped = 0
        self.errors = []
        self.thread = threading.Thread(target=self.run, args=(server, control, crypto))
        self.thread.start()

    def run(self, server, control, crypto):
        client = None
        try:
            while not self.closed.is_set():
                try:
                    packet, address = self.stream.recvfrom(2048)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if address != server:
                    client = address
                    self.stream.sendto(packet, server)
                    continue
                if client is None:
                    continue
                assert control.offer_ready.wait(2), "UDP arrived before its authenticated offer"
                plain = crypto.crypt(False, base64.b64decode(control.offer["tx_key"]),
                                     packet[:20], packet[20:])
                message = int.from_bytes(plain[12:20], "big")
                self.events.append((time.monotonic(), message))
                if message == 1:
                    self.dropped += 1
                else:
                    self.stream.sendto(packet, client)
        except Exception as error:
            self.errors.append(error)

    def close(self):
        self.closed.set()
        self.stream.close()
        self.thread.join(timeout=3)
        assert not self.thread.is_alive(), "UDP proxy thread leaked"


@contextmanager
def server(mbps, log):
    with tempfile.TemporaryDirectory(prefix="waybridge-udp-flow-") as directory:
        runtime = Path(directory)
        environment = {**os.environ, "XDG_RUNTIME_DIR": directory, "WAYLAND_DISPLAY": "flow-0"}
        for name in ("DISPLAY", "WAYBRIDGE_FIXTURE_ALPHA", "WAYBRIDGE_FIXTURE_CSD"):
            environment.pop(name, None)
        token = "udp-flow-" + "a" * 40
        token_path = runtime / "token"
        token_path.write_text(token)
        token_path.chmod(0o600)
        tcp_port = free_port()
        udp_port = free_port(socket.SOCK_DGRAM)
        process = subprocess.Popen([
            str(ROOT / "target/debug/waybridge"), "--listen", f"127.0.0.1:{tcp_port}",
            "--udp-listen", f"127.0.0.1:{udp_port}", "--udp-mbps", str(mbps),
            "--socket", "flow-0", "--token-file", str(token_path), "--codec", "png",
        ], env=environment, stdout=log, stderr=log)
        try:
            wait(lambda: process.poll() is not None or (runtime / "flow-0").exists())
            assert process.poll() is None, "private compositor startup failed"
            yield runtime, environment, token, tcp_port, udp_port
        finally:
            stop(process)


def head_repair(wine):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as endpoint:
        endpoint.bind(("127.0.0.1", 0))
        result = subprocess.run(["wine", str(PROBE), "head", str(endpoint.getsockname()[1])],
                                env=wine, capture_output=True, text=True, timeout=20)
    assert result.returncode == 0, (result.stdout, result.stderr)
    assert "delivered messages 1..17 in order" in result.stdout, result.stdout
    print(result.stdout.strip(), flush=True)


def twenty_windows(wine, crypto, log):
    with server(1000, log) as (runtime, environment, token, tcp_port, udp_port):
        applications = []
        app_logs = []
        relay = loss = None
        try:
            for index in range(10):
                path = runtime / f"app-{index}.log"
                output = path.open("w")
                app_logs.append((path, output))
                applications.append(subprocess.Popen([str(BUILD / "test-app"), "two"],
                                                      env=environment, stdout=output, stderr=output))
            wait(lambda: all("CONFIGURE 2 " in path.read_text() for path, _ in app_logs))
            assert all(app.poll() is None for app in applications)
            relay = DelayProxy(("127.0.0.1", tcp_port))
            loss = LossProxy(("127.0.0.1", udp_port), relay, crypto)
            path = BUILD / "udp-flow-twenty.bin"
            result = subprocess.run([
                "wine", str(PROBE), str(relay.port), token, str(path), "40",
            ], env={**wine, "WAYBRIDGE_UDP_HOST": "127.0.0.1", "WAYBRIDGE_UDP_PORT": str(loss.port)},
                capture_output=True, text=True, timeout=45)
            assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
            assert not relay.errors and not loss.errors, (relay.errors, loss.errors)
            assert relay.retry_started is not None and loss.dropped > 0
            pending = {message for when, message in loss.events if when < relay.retry_started + 0.5}
            assert pending == set(range(1, 9)), pending
            data = path.read_bytes()
            sequences = {}
            count = 0
            while data:
                assert len(data) >= 5
                size, = struct.unpack(">I", data[:4])
                assert 37 < size <= MAX_PACKET and len(data) >= size + 4 and data[4] == 1
                body = data[5:4 + size]
                window, width, height, sequence = struct.unpack(">IIIQ", body[:20])
                assert sequence > sequences.get(window, 0), (window, sequence, sequences)
                sequences[window] = sequence
                image = Image.open(io.BytesIO(body[36:])).convert("RGBA")
                assert image.size == (width, height)
                assert image.getpixel((width // 2, height // 4)) == (255, 0, 0, 255)
                assert image.getpixel((width // 2, height * 3 // 4)) == (0, 0, 255, 255)
                count += 1
                data = data[4 + size:]
            assert len(sequences) == 20 and count >= 40, (len(sequences), count)
            print(f"PASS: 20 real windows, {count} ordered PNG frames, first message lost; "
                  "600ms first NACK delay gates messages 1..8 and ACKs resume progress", flush=True)
        finally:
            if loss is not None:
                loss.close()
            if relay is not None:
                relay.close()
            for app in applications:
                stop(app)
            for _, output in app_logs:
                output.close()


def disconnect(crypto, log):
    with server(1, log) as (_, environment, token, tcp_port, udp_port):
        control = media = app = None
        endpoint = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        endpoint.bind(("127.0.0.1", 0))
        endpoint.settimeout(15)
        try:
            control = socket.create_connection(("127.0.0.1", tcp_port), timeout=10)
            send_json(control, type="hello", version=3, token=token, split_media=True,
                      frame_ack=True, udp_fec_v1=True)
            hello = json.loads(read_packet(control)[1:])
            offer = hello["udp_fec_v1"]
            header = b"WBU1" + int(offer["session"]).to_bytes(8, "big") + (1).to_bytes(8, "big")
            endpoint.sendto(header + crypto.crypt(True, base64.b64decode(offer["rx_key"]),
                                                  header, b"\0"), ("127.0.0.1", udp_port))
            time.sleep(0.03)
            media = socket.create_connection(("127.0.0.1", tcp_port), timeout=10)
            send_json(media, type="hello", version=3, token=token, channel="media",
                      media_key=hello["media_key"])
            assert json.loads(read_packet(media)[1:])["type"] == "media_ready"
            app = subprocess.Popen([str(BUILD / "udp-noise-fixture")], env=environment,
                                   stdout=log, stderr=log)
            first, _ = endpoint.recvfrom(2048)
            plain = crypto.crypt(False, base64.b64decode(offer["tx_key"]), first[:20], first[20:])
            assert int.from_bytes(plain[8:12], "big") > 1024 * 1024, "fixture did not keep sending busy"
            control.close()
            control = None
            media.close()
            media = None
            started = time.monotonic()
            endpoint.settimeout(0.05)
            early = late = 0
            while time.monotonic() - started < 2.1:
                try:
                    endpoint.recvfrom(2048)
                    if time.monotonic() - started < 0.1:
                        early += 1
                    else:
                        late += 1
                except socket.timeout:
                    pass
            assert late == 0, (early, late)
            print(f"PASS: 1Mbps active noisy frame cancelled on TCP disconnect; "
                  f"0 UDP packets after 100ms grace for 2s ({early} queued/racing packets)", flush=True)
        finally:
            for stream in (control, media, endpoint):
                if stream is not None:
                    stream.close()
            stop(app)


def build():
    BUILD.mkdir(parents=True, exist_ok=True)
    PROBE.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cargo", "build", "--locked"], cwd=ROOT, check=True)
    subprocess.run(["bash", "tests/build-fixture.sh"], cwd=ROOT, check=True)
    subprocess.run([
        "x86_64-w64-mingw32-g++", "-municode", "-static", "-O2", "-std=c++17",
        "-D_WIN32_WINNT=0x0A00", "-Iclient", "tests/windows_udp_flow_probe.cpp",
        "-o", str(PROBE), "-lws2_32", "-lcrypt32", "-lbcrypt",
    ], cwd=ROOT, check=True)
    subprocess.run(["cc", "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror",
                    "tests/udp_crypto_fixture.c", "-o", str(BUILD / "udp-crypto-fixture.so"),
                    "-lcrypto"], cwd=ROOT, check=True)
    flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "wayland-client"], text=True).split()
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Ibuild/tests",
                    "tests/udp_noise_fixture.c", "build/tests/xdg-shell-protocol.c",
                    "-o", str(BUILD / "udp-noise-fixture"), *flags], cwd=ROOT, check=True)


def main():
    build()
    crypto = Crypto()
    prefix = ROOT / "build/wine-udp-flow"
    prefix.mkdir(mode=0o700, parents=True, exist_ok=True)
    with (BUILD / "udp-flow.log").open("w") as log:
        read_fd, write_fd = os.pipe()
        xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "1280x1024x24",
                                 "-nolisten", "tcp"], pass_fds=[write_fd], stdout=log, stderr=log)
        os.close(write_fd)
        wine = None
        try:
            assert select.select([read_fd], [], [], 10)[0], "private Xvfb startup timed out"
            with os.fdopen(read_fd) as pipe:
                display = ":" + pipe.readline().strip()
            read_fd = None
            assert display != ":" and xvfb.poll() is None
            wine = {**os.environ, "DISPLAY": display, "WINEPREFIX": str(prefix), "WINEDEBUG": "-all",
                    "WINEDLLOVERRIDES": "winemenubuilder.exe=d;mscoree,mshtml="}
            subprocess.run(["wineboot", "-u"], env=wine, stdout=log, stderr=log, check=True, timeout=60)
            head_repair(wine)
            twenty_windows(wine, crypto, log)
            disconnect(crypto, log)
        finally:
            if read_fd is not None:
                os.close(read_fd)
            if wine is not None:
                subprocess.run(["wineserver", "-k"], env=wine, stdout=log, stderr=log, timeout=10)
            stop(xvfb)


if __name__ == "__main__":
    main()
