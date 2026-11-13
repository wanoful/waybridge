#!/usr/bin/env python3
"""Exercise the C# SSH agent against a private, temporary OpenSSH server."""
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import tempfile
import time
from smoke import ROOT


def main():
    with tempfile.TemporaryDirectory(prefix="waybridge-gui-ssh-") as directory:
        root = Path(directory)
        for key in ("host", "identity"):
            subprocess.run(["ssh-keygen", "-q", "-t", "ed25519", "-N", "", "-f", str(root/key)], check=True)
        with socket.socket() as free:
            free.bind(("127.0.0.1", 0))
            port = free.getsockname()[1]
        # sshd deliberately does not inherit arbitrary caller variables. Pass
        # the disposable user/system buses and application paths explicitly;
        # the SSH path must exercise the same admission policy as TLS.
        fixture_keys = ('DBUS_SYSTEM_BUS_ADDRESS', 'DBUS_SESSION_BUS_ADDRESS', 'HOME',
                        'XDG_CONFIG_HOME', 'XDG_DATA_HOME', 'XDG_CACHE_HOME', 'XDG_STATE_HOME')
        fixture_environment = ' '.join(key + '="' + os.environ[key].replace('\\', '\\\\').replace('"', '\\"') + '"'
                                        for key in fixture_keys if key in os.environ)
        (root/"sshd_config").write_text(f"""Port {port}
ListenAddress 127.0.0.1
HostKey {root/'host'}
PidFile {root/'sshd.pid'}
AuthorizedKeysFile {root/'identity.pub'}
StrictModes no
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
SetEnv XDG_RUNTIME_DIR={root} {fixture_environment}
LogLevel ERROR
""")
        with open(ROOT/"build/tests/gui-sshd.log", "w+") as log:
            daemon = subprocess.Popen([shutil.which("sshd"), "-D", "-e", "-f", str(root/"sshd_config")], stdout=log, stderr=log)
            try:
                for _ in range(100):
                    assert daemon.poll() is None, (ROOT/"build/tests/gui-sshd.log").read_text()
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=.1):
                            break
                    except OSError:
                        time.sleep(.05)
                subprocess.run(["dotnet", "run", "--project", "gui/Waybridge.Control.Tests", "-c", "Release", "--",
                                "--ssh-smoke", str(port), str(root/"identity"), str(ROOT/"target/debug/waybridge"), str(root)],
                               cwd=ROOT, check=True, timeout=90)
            finally:
                daemon.terminate()
                daemon.wait(timeout=5)
                # On failure clean up only the test's own published server process.
                descriptor = root/"gui-ssh-test.session.json"
                if descriptor.exists():
                    saved = json.loads(descriptor.read_text())
                    try:
                        started = Path(f"/proc/{saved['pid']}/stat").read_text().rsplit(")", 1)[1].split()[19]
                        if started == str(saved["started"]):
                            os.kill(saved["pid"], signal.SIGTERM)
                    except FileNotFoundError:
                        pass


if __name__ == "__main__":
    main()
