#!/usr/bin/env python3
"""Actual Qt Widgets at 175%: fractional buffers, alpha, focus, menus and dialogs."""
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time

from smoke import ROOT, Viewer


def main():
    flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "Qt6Widgets"], text=True).split()
    subprocess.run(["c++", "-std=c++17", "-fPIC", "-no-pie", "-Wall", "-Wextra", "-Werror",
                    "tests/qt_app.cpp", "-o", "build/tests/qt-app", *flags], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="waybridge-qt-") as runtime:
        env = {**os.environ, "XDG_RUNTIME_DIR": runtime, "WAYLAND_DISPLAY": "qt-0",
               "QT_QPA_PLATFORM": "wayland", "WAYLAND_DEBUG": "1"}
        # Tests follow the remote monitor DPI, independent of the host desktop's Qt overrides.
        for name in ("QT_SCALE_FACTOR", "QT_SCREEN_SCALE_FACTORS", "QT_WAYLAND_DISABLE_WINDOWDECORATION"):
            env.pop(name, None)
        with socket.socket() as free:
            free.bind(("127.0.0.1", 0)); port = free.getsockname()[1]
        token = "qt-test-" + "6" * 40
        token_file = Path(runtime) / "token"; token_file.write_text(token); token_file.chmod(0o600)
        processes = []
        app_path = ROOT / "build/tests/qt-app.log"
        with open(ROOT / "build/tests/qt-host.log", "w") as host_log, open(app_path, "w") as app_log:
            try:
                host = subprocess.Popen([str(ROOT / "target/debug/waybridge"), "--socket", "qt-0",
                                         "--listen", f"127.0.0.1:{port}", "--token-file", str(token_file)],
                                        env=env, stdout=host_log, stderr=host_log)
                processes.append(host)
                deadline = time.monotonic() + 10
                while not (Path(runtime) / "qt-0").exists():
                    assert host.poll() is None and time.monotonic() < deadline
                    time.sleep(.05)
                viewer = Viewer(port, token, dpi=168)
                app = subprocess.Popen([str(ROOT / "build/tests/qt-app")], env=env, stdout=app_log, stderr=app_log)
                processes.append(app)
                infos = {}; latest = {}; closed = set()

                def pump():
                    k, v = viewer.receive()
                    if k == "control" and v["type"] in ("window","popup"): infos[v["id"]] = v
                    elif k=="control" and v["type"]=="window_closed":closed.add(v["id"])
                    elif k == "frame": latest[v[0]] = v
                    return k, v

                def until(check):
                    end = time.monotonic() + 12
                    while time.monotonic() < end:
                        value = check()
                        if value: return value
                        pump()
                    raise AssertionError("Qt condition timed out; see build/tests/qt-app.log")

                main_id = until(lambda: next((id for id,v in infos.items() if v.get("title") == "Waybridge Qt fixture" and v["decoration"] == "server"), None))
                alpha_id = until(lambda: next((id for id,v in infos.items() if v.get("title") == "Waybridge alpha fixture"), None))
                until(lambda: "DPR 1.75" in app_path.read_text())
                frame = until(lambda: latest.get(main_id) if latest.get(main_id, (0,0,0))[1:3] == (1680,1120) else None)
                frame[-1].save(ROOT / "build/tests/qt-175.png")
                alpha = until(lambda: latest.get(alpha_id) if latest.get(alpha_id,(0,0,0))[1:3] == (1680,1120) else None)[-1]
                alpha.save(ROOT / "build/tests/alpha.png")
                w, h = alpha.size
                assert alpha.getpixel((w//6,h//2)) == (255,0,0,255)
                assert alpha.getpixel((w//2,h//2)) == (0,255,0,128), alpha.getpixel((w//2,h//2))
                assert alpha.getpixel((w*5//6,h//2))[3] == 0
                alpha.save(ROOT / "build/tests/alpha.png")

                def click(id, x, y):
                    viewer.send(type="pointer", id=id, x=x, y=y)
                    viewer.send(type="button", id=id, button=272, down=True, x=x, y=y)
                    viewer.send(type="button", id=id, button=272, down=False, x=x, y=y)

                def key(id, code):
                    viewer.send(type="key", id=id, key=code, down=True)
                    viewer.send(type="key", id=id, key=code, down=False)

                viewer.send(type="focus",id=main_id,focused=True)
                click(main_id,80,40); key(main_id,30)
                until(lambda: "TEXT a\n" in app_path.read_text())
                # Late events from another native window must not clear the active window's focus.
                viewer.send(type="focus",id=alpha_id,focused=True)
                viewer.send(type="focus",id=main_id,focused=True)
                viewer.send(type="focus",id=alpha_id,focused=False)
                viewer.send(type="leave",id=alpha_id)
                key(main_id,48)
                until(lambda: "TEXT ab\n" in app_path.read_text())
                click(main_id,80,100)
                menu = until(lambda: re.search(r"^MENU (-?\d+) (-?\d+) (\d+) (\d+)$", app_path.read_text(), re.M))
                x,y,w,h = map(int, menu.groups())
                click(main_id,x+w//2,y+h//2)
                until(lambda: "ACTION\n" in app_path.read_text())
                # Keyboard-triggered menus need a valid key serial after the key has been released.
                click(main_id,80,40)
                viewer.send(type="key",id=main_id,key=29,down=True)
                key(main_id,50)
                viewer.send(type="key",id=main_id,key=29,down=False)
                until(lambda: len(re.findall(r"^MENU ",app_path.read_text(),re.M)) >= 2)
                popup_id = until(lambda:next((id for id,v in infos.items() if v["type"]=="popup" and id not in closed),None))
                key(main_id,1)  # Escape must reach the grabbed popup.
                until(lambda:popup_id in closed)
                click(main_id,200,40);key(main_id,46)
                until(lambda: "TEXT abc\n" in app_path.read_text())
                # A real popup extends the parent without changing the parent's stream geometry.
                parent_geometry = (infos[main_id]["width"], infos[main_id]["height"])
                click(main_id,450,100)
                overflow_id = until(lambda:next((id for id,v in infos.items() if v["type"]=="popup" and id not in closed),None))
                overflow = infos[overflow_id]
                until(lambda:latest.get(overflow_id))
                assert overflow["x"]+overflow["width"] > parent_geometry[0] or overflow["y"]+overflow["height"] > parent_geometry[1],overflow
                assert (infos[main_id]["width"],infos[main_id]["height"]) == parent_geometry
                click(overflow_id,overflow["width"]//2,12)
                until(lambda:"OVERFLOW_ACTION\n" in app_path.read_text())
                until(lambda:overflow_id in closed)
                # Nested popups retain a separate native parent and input surface.
                click(main_id,450,100)
                overflow_id=until(lambda:next((id for id,v in infos.items() if v["type"]=="popup" and id not in closed),None))
                overflow=infos[overflow_id]
                click(overflow_id,overflow["width"]//2,overflow["height"]-12)
                nested_id=until(lambda:next((id for id,v in infos.items() if v["type"]=="popup" and v["parent"]==overflow_id and id not in closed),None))
                until(lambda:latest.get(nested_id))
                click(nested_id,infos[nested_id]["width"]//2,12)
                until(lambda:"NESTED_ACTION\n" in app_path.read_text())
                until(lambda:nested_id in closed and overflow_id in closed)
                click(main_id,250,100)
                dialog_id = until(lambda: next((id for id,v in infos.items() if v.get("title") == "Waybridge Qt dialog"), None))
                assert infos[dialog_id]["parent"] == main_id, infos[dialog_id]
                until(lambda: latest.get(dialog_id))
                viewer.send(type="focus",id=dialog_id,focused=True)
                viewer.send(type="focus",id=main_id,focused=False)
                click(dialog_id,80,40); key(dialog_id,46)
                until(lambda: "DIALOG_TEXT c\n" in app_path.read_text())
                # DPI changes while the application remains alive.
                viewer.send(type="resize",id=main_id,width=800,height=500,dpi=120)
                until(lambda: "DPR 1.25" in app_path.read_text())
                until(lambda: latest.get(main_id) if latest.get(main_id,(0,0,0))[1:3] == (1000,625) else None)
                assert infos[alpha_id]["scale"] == 1.75, "per-window DPI must not change other windows"
                print("PASS: actual Qt at 175%, 1680x1120 buffers, exact alpha 0/128/255, text input, stale focus/leave, pointer/keyboard popup grabs, Escape/focus restoration, modal dialog/parent, live 125% DPI change")
                viewer.sock.close()
            finally:
                for p in reversed(processes):
                    if p.poll() is None:
                        p.terminate()
                        try: p.wait(timeout=3)
                        except subprocess.TimeoutExpired: p.kill(); p.wait()


if __name__ == "__main__": main()
