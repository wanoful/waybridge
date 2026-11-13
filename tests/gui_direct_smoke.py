#!/usr/bin/env python3
"""Exercise real .NET pinned direct RPC against the TLS service and compositor.

Pairing/client/session credentials stay in stdin and private IPC, never argv,
environment variables or logs. Uses temporary state and no real user credentials.
"""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--service", type=Path, default=ROOT / "target/debug/waybridge-service")
    parser.add_argument("--compositor", type=Path, default=ROOT / "target/debug/waybridge")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="waybridge-gui-direct-") as directory:
        root = Path(directory)
        runtime, state = root / "runtime", root / "state"
        runtime.mkdir(mode=0o700)
        state.mkdir(mode=0o700)
        application_directory = root / "data/applications"
        application_directory.mkdir(parents=True)
        (application_directory / "waybridge-direct-smoke.desktop").write_text(
            "[Desktop Entry]\nType=Application\nName=Waybridge Direct Test 应用\nExec=/usr/bin/true\n")
        env = os.environ.copy()
        env.update(XDG_RUNTIME_DIR=str(runtime), XDG_DATA_HOME=str(root / "data"),
                   XDG_CONFIG_HOME=str(root / "config"), XDG_STATE_HOME=str(root / "app-state"),
                   XDG_CACHE_HOME=str(root / "cache"))
        command = [str(args.service.resolve()), "--state-dir", str(state), "--runtime-dir", str(runtime / "entry")]
        with (root / "service.log").open("w+") as log:
            daemon = subprocess.Popen([*command, "serve", "--listen", "127.0.0.1:0", "--compositor", str(args.compositor.resolve())],
                                      env=env, stdin=subprocess.DEVNULL, stdout=log, stderr=log)
            try:
                ready_path = runtime / "entry/ready.json"
                deadline = time.monotonic() + 15
                while not ready_path.exists():
                    if daemon.poll() is not None:
                        raise AssertionError("private direct service failed to start")
                    if time.monotonic() > deadline:
                        raise TimeoutError("private direct service startup timed out")
                    time.sleep(.02)
                ready = json.loads(ready_path.read_text())
                pairing = json.loads(subprocess.check_output([*command, "pair"], env=env))["result"]
                host, port = ready["listen"].rsplit(":", 1)
                bootstrap = dict(host=host, port=int(port), certificate_sha256=ready["certificate_sha256"],
                                 pair_code=pairing["pair_code"], runtime=str(runtime), session="gui-direct-test")
                result = subprocess.run(["dotnet", "run", "--project", "gui/Waybridge.Control.Tests", "-c", "Release", "--", "--direct-smoke"],
                                        input=json.dumps(bootstrap).encode() + b"\n", env=env, cwd=ROOT,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=150)
                output = result.stdout.decode(errors="replace")
                # Never echo a process output that accidentally included the
                # locally generated bootstrap secret.
                assert pairing["pair_code"] not in output, "direct test leaked pairing code"
                print(output, end="")
                assert result.returncode == 0, ".NET direct smoke failed"
                assert not (runtime / "gui-direct-test.session.json").exists(), "session cleanup was not complete"
                print("PASS: real .NET direct API and controller exercised without SSH or native-media forwarding through C#")
            finally:
                # On failure touch only the test's own still-matching process.
                descriptor = runtime / "gui-direct-test.session.json"
                if descriptor.exists():
                    saved = json.loads(descriptor.read_text())
                    try:
                        started = Path(f"/proc/{saved['pid']}/stat").read_text().rsplit(")", 1)[1].split()[19]
                        if started == str(saved["started"]):
                            os.kill(saved["pid"], signal.SIGTERM)
                    except FileNotFoundError:
                        pass
                daemon.terminate()
                try:
                    daemon.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    daemon.kill()
                    daemon.wait()


if __name__ == "__main__":
    main()
