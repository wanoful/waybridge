#!/usr/bin/env python3
"""Real Windows native worker + rustls service + actual compositor, without SSH.

Only temporary pairing/session state and private Wine/Xvfb are used. Tokens enter
the PE worker through a named pipe; generated audio uses private null endpoints.
"""
import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import signal
import socket
import ssl
import struct
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]


def wait(check, seconds=20):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        value = check()
        if value:
            return value
        time.sleep(.03)
    raise AssertionError("native direct fixture timed out")


class Native:
    def __init__(self, env, log):
        self.log = log
        self.process = subprocess.Popen(["wine", str(ROOT / "build/tests/windows-control-probe.exe"),
                                         "Z:" + str(ROOT / "build/windows/waybridge-client.exe"), "Z:" + str(log)],
                                        env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.DEVNULL, text=True)
        self.events = queue.Queue()

        def reader():
            for line in self.process.stdout:
                self.events.put(json.loads(line))
        threading.Thread(target=reader, daemon=True).start()

    def send(self, **message):
        self.process.stdin.write(json.dumps(message) + "\n")
        self.process.stdin.flush()

    def until(self, check, seconds=25):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                event = self.events.get(timeout=.2)
            except queue.Empty:
                if self.process.poll() is not None:
                    raise AssertionError("native worker exited before expected event: " + self.log.read_text())
                continue
            if check(event):
                return event
        raise AssertionError("native worker event timeout: " + self.log.read_text())

    def close(self):
        if self.process.poll() is None:
            self.send(type="disconnect")
            self.process.stdin.close()
            self.process.wait(timeout=15)
        assert self.process.returncode == 0, self.log.read_text()


def main():
    subprocess.run(["bash", "tests/build-fixture.sh"], cwd=ROOT, check=True)
    for name, libraries in [("windows_control_probe", []),
                             ("windows_probe", ["-lgdi32", "-lshell32", "-lole32", "-luuid"]),
                             ("windows_audio_probe", ["-lole32", "-luuid"])]:
        subprocess.run(["x86_64-w64-mingw32-g++", "-std=c++17", "-municode", "-static", "-O2",
                        "-D_WIN32_WINNT=0x0A00", f"tests/{name}.cpp", "-o", f"build/tests/{name}.exe", *libraries],
                       cwd=ROOT, check=True)
    processes, natives, modules, secrets = [], [], [], []
    service_command = None
    credentials = cleanup_credentials = None
    session = None
    pulse = None
    default_sink = None
    wine_env = None
    with tempfile.TemporaryDirectory(prefix="waybridge-native-direct-") as directory:
        temporary = Path(directory)
        runtime = temporary / "runtime"; runtime.mkdir(mode=0o700)
        state = temporary / "state"; state.mkdir(mode=0o700)
        env = {**os.environ, "XDG_RUNTIME_DIR": str(runtime), "XDG_STATE_HOME": str(temporary / "app-state"),
               "XDG_CONFIG_HOME": str(temporary / "config"), "XDG_DATA_HOME": str(temporary / "data"),
               "XDG_CACHE_HOME": str(temporary / "cache")}
        with (temporary / "fixture.log").open("w+") as log:
            try:
                display_read, display_write = os.pipe()
                xvfb = subprocess.Popen(["Xvfb", "-displayfd", str(display_write), "-screen", "0", "1280x1024x24", "-nolisten", "tcp"],
                                        pass_fds=[display_write], stdout=log, stderr=log)
                processes.append(xvfb); os.close(display_write)
                with os.fdopen(display_read) as reader:
                    display = ":" + reader.readline().strip()
                wine_env = {**os.environ, "DISPLAY": display, "WINEPREFIX": str(temporary / "wine"),
                            "WINEDEBUG": "-all", "WINE_CPU_TOPOLOGY": "4:0,1,2,3",
                            "WINEDLLOVERRIDES": "winemenubuilder.exe=d;mscoree,mshtml=", "WAYBRIDGE_GDI": "1"}
                subprocess.run(["wineboot", "-u"], env=wine_env, stdout=log, stderr=log, check=True, timeout=60)
                audio_device = None
                if all(shutil.which(tool) for tool in ["pactl", "pacat", "parec"]):
                    result = subprocess.run(["pactl", "info"], capture_output=True, text=True, timeout=5)
                    if result.returncode == 0:
                        server = next(line.split(": ", 1)[1] for line in result.stdout.splitlines() if line.startswith("Server String:"))
                        pulse = {**os.environ, "PULSE_SERVER": server}
                        default_sink = subprocess.check_output(["pactl", "get-default-sink"], env=pulse, text=True).strip()
                        env["PULSE_SERVER"] = server; wine_env["PULSE_SERVER"] = server
                        output = "waybridge-direct-out-" + str(os.getpid())
                        description = "Waybridge-Direct-Out-" + str(os.getpid())
                        module = subprocess.check_output(["pactl", "load-module", "module-null-sink", "sink_name=" + output,
                                                          "rate=48000", "channels=2", "sink_properties=device.description=" + description], env=pulse, text=True)
                        modules.append(int(module.strip()))
                        resolved = subprocess.run(["wine", str(ROOT / "build/tests/windows_audio_probe.exe"), description],
                                                  env=wine_env, capture_output=True, text=True, timeout=20)
                        if resolved.returncode == 0 and resolved.stdout.strip().startswith("{"):
                            audio_device = resolved.stdout.strip()
                service_command = [str(ROOT / "target/debug/waybridge-service"), "--state-dir", str(state),
                                   "--runtime-dir", str(runtime / "entry")]
                daemon = subprocess.Popen([*service_command, "serve", "--listen", "127.0.0.1:0",
                                           "--compositor", str(ROOT / "target/debug/waybridge")], env=env,
                                          stdin=subprocess.DEVNULL, stdout=log, stderr=log)
                processes.append(daemon)
                ready_file = runtime / "entry/ready.json"
                wait(ready_file.exists)
                ready = json.loads(ready_file.read_text())
                host, port = ready["listen"].rsplit(":", 1)
                pin = ready["certificate_sha256"]
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
                context.check_hostname = False; context.verify_mode = ssl.CERT_NONE
                context.minimum_version = ssl.TLSVersion.TLSv1_2

                def rpc(message, auth=None):
                    request = {"id": 1, **message, **(auth or {})}
                    with context.wrap_socket(socket.create_connection((host, int(port)), timeout=40), server_hostname="waybridge.local") as connection:
                        assert hashlib.sha256(connection.getpeercert(binary_form=True)).hexdigest() == pin
                        connection.sendall(json.dumps(request).encode() + b"\n")
                        response = bytearray()
                        while not response.endswith(b"\n"):
                            piece = connection.recv(1)
                            if not piece:
                                raise EOFError("service reply missing")
                            response += piece
                            assert len(response) <= 4 * 1024 * 1024
                        return json.loads(response)

                def ok(message, auth=None):
                    response = rpc(message, auth or credentials)
                    assert response.get("ok"), "service operation failed: " + response.get("error", {}).get("code", "unknown")
                    return response["result"]

                def pair(label):
                    local = json.loads(subprocess.check_output([*service_command, "pair"], env=env))["result"]
                    secrets.append(local["pair_code"])
                    assert local["certificate_sha256"] == pin
                    value = rpc(dict(op="pair", pair_code=local["pair_code"], label=label))
                    assert value.get("ok"), "pairing failed"
                    result = {key: value["result"][key] for key in ["client_id", "client_token"]}
                    secrets.append(result["client_token"])
                    return result

                credentials = pair("native integration")
                cleanup_credentials = pair("fixture cleanup")
                session = ok(dict(op="ensure", session="native-direct-0", port=0, mode="auto", codec="png",
                                  audio=audio_device is not None, xwayland=False))
                secrets.append(session["token"])
                app = ok(dict(op="launch", session=session["name"], argv=[str(ROOT / "build/tests/test-app")]))
                app_log = runtime / "waybridge-control-center" / (session["name"] + ".log")
                settings = dict(codec="png", fps=60, bitrate=30000, audio=audio_device is not None, files=True)
                if audio_device:
                    settings["audio_device"] = audio_device
                transport = dict(type="tls", fingerprint=pin, session=session["name"], **credentials)

                def launch(label, connection=transport):
                    native = Native(wine_env, temporary / (label + ".log")); natives.append(native)
                    native.send(type="start", version=1, host=host, port=int(port), token=session["token"], settings=settings, transport=connection)
                    return native

                def probe(*args):
                    return subprocess.run(["wine", str(ROOT / "build/tests/windows_probe.exe"), *args], env=wine_env,
                                          capture_output=True, text=True, timeout=20)

                native = launch("native-first")
                native.until(lambda event: event.get("type") == "connected")
                first = native.until(lambda event: event.get("type") == "status" and event.get("windows"))
                assert first["tcp_bytes"] > 0 and first["windows"][0]["codec"] == "PNG"
                window_id = first["windows"][0]["id"]
                wait(lambda: probe("inspect").returncode == 0)
                before = app_log.read_text().count("BUTTON")
                assert probe("click", "80", "80").returncode == 0
                assert probe("type", "a").returncode == 0
                wait(lambda: app_log.read_text().count("BUTTON") > before and "KEY" in app_log.read_text())
                native.send(type="settings", settings={**settings, "fps": 30, "read_only": True})
                assert native.until(lambda event: event.get("type") == "stream_config" and event.get("fps") == 30)["read_only"]
                native.send(type="settings", settings=settings)
                native.until(lambda event: event.get("type") == "stream_config" and not event.get("read_only"))
                source = temporary / "你好.txt"; source.write_text("encrypted native file transfer")
                native.send(type="send_files", paths=["Z:" + str(source)])
                native.until(lambda event: event.get("type") == "transfer" and event.get("event") == "tree_upload_ready" and event.get("stage") == "commit")
                if audio_device:
                    wait(lambda: "WASAPI 48 kHz stereo audio playback active" in native.log.read_text())
                    sink_path = temporary / "sink"
                    code = "import os,pathlib;pathlib.Path(" + repr(str(sink_path)) + ").write_text(os.environ['PULSE_SINK'])"
                    ok(dict(op="launch", session=session["name"], argv=["python3", "-I", "-c", code]))
                    wait(sink_path.exists)
                    from audio_smoke import tone
                    raw = temporary / "audio.raw"
                    with raw.open("wb") as recording:
                        capture = subprocess.Popen(["parec", "--raw", "--format=s16le", "--rate=48000", "--channels=2",
                                                    "--latency-msec=10", "--device=" + output + ".monitor"], env=pulse, stdout=recording, stderr=log)
                        processes.append(capture)
                        player, played, result = tone(sink_path.read_text(), pulse)
                        processes.append(player); played.join(timeout=12)
                        assert not played.is_alive() and result and result[0][0] == 0
                        time.sleep(.5); capture.terminate(); capture.wait(timeout=5)
                    raw_bytes = raw.read_bytes(); raw_bytes = raw_bytes[:len(raw_bytes) // 4 * 4]
                    samples = struct.unpack("<" + "h" * (len(raw_bytes) // 2), raw_bytes)
                    left, right = (max(map(abs, samples[offset::2]), default=0) for offset in [0, 1])
                    assert left > 5000 and right > 2000 and .3 < right / left < .7, "private TLS audio loopback did not match the test tone"
                native.close()
                retained = ok(dict(op="ensure", session=session["name"], mode="attach"))
                assert retained["pid"] == session["pid"]
                os.kill(app["pid"], 0)
                native = launch("native-reconnected")
                native.until(lambda event: event.get("type") == "connected")
                again = native.until(lambda event: event.get("type") == "status" and event.get("windows"))
                assert any(window["id"] == window_id for window in again["windows"])
                wait(lambda: probe("inspect").returncode == 0)
                native.close()
                for label, changed, expected in [
                    ("bad-pin", {**transport, "fingerprint": "00" * 32}, "fingerprint mismatch"),
                    ("bad-auth", {**transport, "client_token": "wrong-client-token-for-fixture"}, "authorization rejected"),
                ]:
                    rejected = launch(label, changed)
                    error = rejected.until(lambda event: event.get("type") == "error")
                    assert expected in error.get("message", "")
                    rejected.process.stdin.close(); rejected.process.wait(timeout=15)
                    assert rejected.process.returncode != 0
                native = launch("native-revoked")
                native.until(lambda event: event.get("type") == "connected")
                native.until(lambda event: event.get("type") == "status" and event.get("windows"))
                ok(dict(op="revoke", revoke_client_id=credentials["client_id"]), cleanup_credentials)
                native.process.stdin.close(); native.process.wait(timeout=15)
                assert not rpc(dict(op="list"), credentials).get("ok")
                assert ok(dict(op="ensure", session=session["name"], mode="attach"), cleanup_credentials)["pid"] == session["pid"]
                ok(dict(op="end", session=session["name"]), cleanup_credentials); session = None
                for path in temporary.glob("*.log"):
                    contents = path.read_text(errors="replace")
                    assert not any(secret in contents for secret in secrets), "credential leaked into fixture logs"
                print("PASS: actual rustls service + Schannel native control/media, pixels, input, live settings, Unicode upload, retained-session reconnect, pin/auth rejection and active-client revocation")
                print("PASS: encrypted native audio via private WASAPI endpoint and null-sink loopback" if audio_device else
                      "SKIP: physical/native audio endpoint unavailable; control and media passed")
            finally:
                if session and cleanup_credentials:
                    try:
                        ok(dict(op="end", session=session["name"]), cleanup_credentials)
                    except Exception:
                        try: os.kill(session["pid"], signal.SIGTERM)
                        except ProcessLookupError: pass
                for native in reversed(natives):
                    if native.process.poll() is None:
                        native.process.terminate()
                        try: native.process.wait(timeout=5)
                        except subprocess.TimeoutExpired: native.process.kill(); native.process.wait()
                for process in reversed(processes):
                    if process.poll() is None:
                        process.terminate()
                        try: process.wait(timeout=5)
                        except subprocess.TimeoutExpired: process.kill(); process.wait()
                if wine_env:
                    subprocess.run(["wineserver", "-k"], env=wine_env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                for module in modules:
                    subprocess.run(["pactl", "unload-module", str(module)], env=pulse, check=True)
                if default_sink:
                    assert subprocess.check_output(["pactl", "get-default-sink"], env=pulse, text=True).strip() == default_sink


if __name__ == "__main__":
    main()
