#!/usr/bin/env python3
"""Verify real Dolphin consumes focus/keyboard/clipboard at 175%, in a private D-Bus session."""
import base64
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
from smoke import ROOT, Viewer


def main():
    with tempfile.TemporaryDirectory(prefix="waybridge-dolphin-") as runtime:
        env = {**os.environ, "XDG_RUNTIME_DIR": runtime, "WAYLAND_DISPLAY": "dolphin-0", "QT_QPA_PLATFORM": "wayland", "XDG_CONFIG_HOME":str(Path(runtime)/"config"),
               "XDG_CACHE_HOME":str(Path(runtime)/"cache"), "XDG_DATA_HOME":str(Path(runtime)/"data")}
        for name in ("QT_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS", "QT_WAYLAND_DISABLE_WINDOWDECORATION"):
            env.pop(name, None)
        with socket.socket() as free:
            free.bind(("127.0.0.1",0)); port=free.getsockname()[1]
        token="dolphin-test-"+"3"*40
        token_path=Path(runtime)/"token";token_path.write_text(token);token_path.chmod(0o600)
        source=Path(runtime)/"start";source.mkdir()
        target=Path(runtime)/"waybridge-focus-target";target.mkdir()
        with open(ROOT/"build/tests/dolphin-host.log","w") as host_log, open(ROOT/"build/tests/dolphin-app.log","w") as app_log:
            host=subprocess.Popen([str(ROOT/"target/debug/waybridge"),"--listen",f"127.0.0.1:{port}","--socket","dolphin-0","--token-file",str(token_path)],env=env,stdout=host_log,stderr=host_log)
            app=None
            try:
                deadline=time.monotonic()+10
                while not (Path(runtime)/"dolphin-0").exists():
                    assert host.poll() is None and time.monotonic()<deadline
                    time.sleep(.05)
                viewer=Viewer(port,token,dpi=168)
                app=subprocess.Popen(["dbus-run-session","--","dolphin","--new-window",str(source)],env=env,stdout=app_log,stderr=app_log,start_new_session=True)
                info=viewer.until(lambda k,v:k=="control" and v["type"]=="window" and v["decoration"]=="server" and v["app_id"]=="org.kde.dolphin")
                id=info["id"]
                viewer.until(lambda k,v:k=="frame" and v[0]==id and v[1:3]==(1680,1120))
                viewer.send(type="focus",id=id,focused=True)
                # Focus the address field, then paste a real path and navigate.
                def key(code,down):viewer.send(type="key",id=id,key=code,down=down)
                key(29,True);key(38,True);key(38,False);key(29,False)  # Ctrl+L
                viewer.send(type="clipboard",items={"text/plain;charset=utf-8":base64.b64encode(str(target).encode()).decode()})
                time.sleep(.2)
                key(29,True);key(47,True);key(47,False);key(29,False)  # Ctrl+V
                key(28,True);key(28,False)
                viewer.until(lambda k,v:k=="control" and v["type"]=="window" and "waybridge-focus-target" in v["title"])
                frame=viewer.until(lambda k,v:k=="frame" and v[0]==id)
                viewer.sock.settimeout(.1)
                deadline=time.monotonic()+.6
                while time.monotonic()<deadline:
                    try:
                        k,v=viewer.receive()
                        if k=="frame" and v[0]==id:frame=v
                    except socket.timeout:pass
                frame[-1].save(ROOT/"build/tests/dolphin-175.png")
                viewer.sock.close()
                print("PASS: actual Dolphin with server decoration at 175% accepts focus, Ctrl+L, clipboard paste and Enter; navigates to the requested directory")
            finally:
                if app and app.poll() is None:
                    os.killpg(app.pid,signal.SIGTERM)
                    try:app.wait(timeout=5)
                    except subprocess.TimeoutExpired:os.killpg(app.pid,signal.SIGKILL);app.wait()
                host.terminate();host.wait(timeout=5)


if __name__=="__main__":main()
