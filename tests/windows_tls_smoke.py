#!/usr/bin/env python3
"""Windows Schannel against an actual isolated TLS gateway; credentials via stdin."""
import hashlib
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]


def main():
    out = ROOT / "build/tests"
    out.mkdir(parents=True, exist_ok=True)
    binary = out / "windows-tls-probe.exe"
    subprocess.run(["x86_64-w64-mingw32-g++", "-std=c++17", "-static", "-O2", "-Wall", "-Wextra",
                    "tests/windows_tls_probe.cpp", "-o", str(binary), "-lws2_32", "-lsecur32", "-lcrypt32"], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="waybridge-tls-native-") as directory:
        temporary = Path(directory)
        cert, key = temporary / "cert.pem", temporary / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-subj", "/CN=localhost", "-keyout", str(key), "-out", str(cert)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        der = ssl.PEM_cert_to_DER_cert(cert.read_text())
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(cert, key)
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0)); listener.listen(); listener.settimeout(0.2)
        stopped = threading.Event()
        authorized, requests, errors = [], [], []
        credential = "native-test-credential-" + os.urandom(24).hex()

        def server():
            while not stopped.is_set():
                try:
                    connection, _ = listener.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break
                try:
                    with context.wrap_socket(connection, server_side=True) as stream:
                        stream.settimeout(10)
                        line = b""
                        while not line.endswith(b"\n"):
                            piece = stream.recv(1)
                            if not piece:
                                break
                            line += piece
                            if len(line) > 65536:
                                raise AssertionError("unbounded stream request")
                        if not line:  # the wrong certificate was rejected before authorization
                            continue
                        request = json.loads(line)
                        requests.append(request)
                        if request.get("client_token") != credential:
                            stream.sendall(b'{"id":1,"ok":false,"error":{"code":"unauthorized","message":"Rejected"}}\n')
                            continue
                        assert request == dict(id=1, op="stream", client_id="native-probe", client_token=credential, session="waybridge-0")
                        authorized.append(request)
                        # Coalesce the reply and start of the opaque protocol to verify
                        # that the line handshake retains subsequent TLS plaintext.
                        stream.sendall(b'{"id":1,"ok":true,"result":{"stream_ready":true,"version":3}}\nopaque-v3-ready!!')
                        payload = bytearray()
                        while len(payload) < 262144:
                            piece = stream.recv(min(113, 262144 - len(payload)))
                            if not piece:
                                raise AssertionError("truncated encrypted payload")
                            payload.extend(piece)
                        for offset in range(0, len(payload), 4093):
                            stream.sendall(payload[offset:offset + 4093])
                except (ssl.SSLError, ConnectionResetError, BrokenPipeError):
                    pass  # expected when a probe rejects the pin or authorization
                except Exception as error:
                    errors.append(str(error))

        worker = threading.Thread(target=server, daemon=True); worker.start()
        env = {**os.environ, "WINEPREFIX": str(temporary / "wine"), "WINEDEBUG": "-all",
               "WINE_CPU_TOPOLOGY": "4:0,1,2,3"}
        config = dict(type="tls", fingerprint=hashlib.sha256(der).hexdigest(), client_id="native-probe",
                      client_token=credential, session="waybridge-0")
        try:
            def run(settings):
                result = subprocess.run(["wine", str(binary), str(listener.getsockname()[1])], env=env,
                                        input=json.dumps(settings) + "\n", text=True, capture_output=True, timeout=60)
                assert credential not in result.stdout + result.stderr
                return result
            good = run(config)
            assert good.returncode == 0, good.stdout + good.stderr
            assert len(authorized) == 3, "expected three authorized streams"
            count = len(requests)
            bad_pin = run({**config, "fingerprint": "00" * 32})
            assert bad_pin.returncode == 2 and "fingerprint mismatch" in bad_pin.stderr, bad_pin.stdout + bad_pin.stderr
            assert len(requests) == count, "credentials were sent before pin verification"
            rejected = run({**config, "client_token": "rejected-client-credential"})
            assert rejected.returncode == 2 and "authorization rejected" in rejected.stderr, rejected.stdout + rejected.stderr
            assert len(authorized) == 3 and not errors, errors
            print(good.stdout.strip())
            print("PASS: wrong pin rejected before credentials, wrong token rejected, no credentials in probe logs")
        finally:
            stopped.set(); listener.close(); worker.join(timeout=3)
            subprocess.run(["wineserver", "-k"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
