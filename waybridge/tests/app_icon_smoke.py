#!/usr/bin/env python3
"""Real Wayland app_id -> isolated XDG PNG/SVG resolution and optional v3 icons."""
import base64
import io
import os
from pathlib import Path
import shlex
import shutil
import socket
import subprocess
import tempfile
import time
from PIL import Image
from smoke import Viewer, ROOT

COLORS={"Png":(255,0,0,255),"Svg":(0,0,255,255),"Absolute":(255,0,255,255),"Inherited":(0,255,0,255),"Hicolor":(255,255,0,255),"Pixmap":(0,255,255,255)}
KEYS={"Png":30,"Svg":48,"Missing":46,"Absolute":32,"Inherited":18,"Hicolor":33,"Pixmap":34,"Hidden":35,"Malformed":23,"Huge":24,"Slow":25,"nested-app":49}

def wait(check,seconds=15):
    deadline=time.monotonic()+seconds
    while not check():
        assert time.monotonic()<deadline,"timed out"
        time.sleep(.03)

def fixtures(runtime):
    root=Path(runtime);(root/"home").mkdir();data=root/"data";system=root/"system";applications=data/"applications";applications.mkdir(parents=True)
    def png(path,color,size=(128,128)):
        path.parent.mkdir(parents=True,exist_ok=True);Image.new("RGBA",size,color).save(path)
    def desktop(name,icon,extra=""):
        (applications/f"org.waybridge.{name}.desktop").write_text(f"[Desktop Entry]\nType=Application\nName={name}\nIcon={icon}\n{extra}")
    themes=data/"icons"
    theme=themes/"Test";theme.mkdir(parents=True)
    (theme/"index.theme").write_text("[Icon Theme]\nName=Test\nDirectories=32x32/apps,128x128/apps,scalable/apps\nInherits=Inherited\n[32x32/apps]\nSize=32\nType=Fixed\n[128x128/apps]\nSize=128\nType=Fixed\n[scalable/apps]\nSize=128\nType=Scalable\nMinSize=16\nMaxSize=256\n")
    png(theme/"32x32/apps/bridge-png.png",(0,255,0,255),(32,32));png(theme/"128x128/apps/bridge-png.png",COLORS["Png"])
    desktop("Png","bridge-png")
    svg=theme/"scalable/apps/bridge-svg.svg";svg.parent.mkdir(parents=True)
    svg.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="64" height="32"><rect width="64" height="32" fill="blue"/></svg>')
    desktop("Svg","bridge-svg")
    absolute=root/"absolute icon.png";png(absolute,COLORS["Absolute"]);desktop("Absolute",str(absolute).replace(" ","\\s"))
    for name in ("Inherited","hicolor"):
        directory=themes/name;directory.mkdir()
        (directory/"index.theme").write_text("[Icon Theme]\nName=Test\nDirectories=128x128/apps\n[128x128/apps]\nSize=128\nType=Fixed\n")
    png(themes/"Inherited/128x128/apps/bridge-inherit.png",COLORS["Inherited"]);desktop("Inherited","bridge-inherit")
    png(themes/"hicolor/128x128/apps/bridge-hicolor.png",COLORS["Hicolor"]);desktop("Hicolor","bridge-hicolor")
    png(data/"pixmaps/pixmap-only.png",COLORS["Pixmap"]);desktop("Pixmap","pixmap-only")
    desktop("Hidden","bridge-png","Hidden=true\n")
    (system/"applications").mkdir(parents=True)
    (system/"applications/org.waybridge.Hidden.desktop").write_text("[Desktop Entry]\nIcon=bridge-png\n")
    broken=root/"broken.svg";broken.write_text("<svg");desktop("Malformed",str(broken))
    huge=root/"huge.png";png(huge,(255,0,0,255),(2048,2048));desktop("Huge",str(huge))
    slow=root/"slow.svg";slow.write_text(svg.read_text());desktop("Slow",str(slow))
    nested=applications/"nested";nested.mkdir();(nested/"app.desktop").write_text("[Desktop Entry]\nIcon=bridge-inherit\n")
    # Deliberately stall one SVG request; the compositor and newer app_id must remain responsive.
    renderer=shutil.which("rsvg-convert");assert renderer,"SVG fixture requires rsvg-convert"
    bin_dir=root/"bin";bin_dir.mkdir();converter=bin_dir/"rsvg-convert"
    converter.write_text(f'#!/bin/sh\ncase "$*" in *slow.svg*) echo started > {shlex.quote(str(root/"converter.started"))}; exec /bin/sleep 10;; esac\nexec {shlex.quote(renderer)} "$@"\n');converter.chmod(0o755)
    return {**os.environ,"HOME":str(root/"home"),"XDG_DATA_HOME":str(data),"XDG_DATA_DIRS":str(system),"XDG_CONFIG_HOME":str(root/"config"),"WAYBRIDGE_ICON_THEME":"Test","PATH":str(bin_dir)+":"+os.environ["PATH"],"WAYBRIDGE_TEST_ICON":"1","WAYBRIDGE_TEST_APP_ID":"org.waybridge.Png"}

class IconViewer(Viewer):
    def __init__(self,port,token,enabled=True):
        self.sock=socket.create_connection(("127.0.0.1",port),timeout=8);self.geometry={}
        self.send(type="hello",version=3,token=token,window_icon_v1=enabled)
        assert self.receive()[1]["window_icon_v1"] is enabled
    def icon(self,id,revision=None):
        return self.until(lambda k,v:k=="control" and v["type"]=="window_icon" and v["id"]==id and (revision is None or v["revision"]==revision))

def validate(message,name):
    if name not in COLORS and name!="nested-app":assert message["png"] is None,message;return
    image=Image.open(io.BytesIO(base64.b64decode(message["png"],validate=True))).convert("RGBA")
    assert image.size==(128,128)
    expected=COLORS["Inherited"] if name=="nested-app" else COLORS[name]
    assert image.getpixel((64,64))==expected,(name,image.getpixel((64,64)))
    if name=="Svg":assert image.getpixel((0,0))==(0,0,0,0)
    assert len(base64.b64decode(message["png"]))<=96*1024

def main():
    subprocess.run(["bash","tests/build-fixture.sh"],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix="waybridge-icons-") as runtime,open(ROOT/"build/tests/app-icons.log","w+") as log:
        env=fixtures(runtime);env.update(XDG_RUNTIME_DIR=runtime,WAYLAND_DISPLAY="icons-0")
        with socket.socket() as free:free.bind(("127.0.0.1",0));port=free.getsockname()[1]
        token="app-icons-"+"8"*40;token_path=Path(runtime)/"token";token_path.write_text(token);token_path.chmod(0o600)
        processes=[]
        try:
            host=subprocess.Popen([str(ROOT/"target/debug/waybridge"),"--listen",f"127.0.0.1:{port}","--socket","icons-0","--token-file",str(token_path),"--codec","png"],env=env,stdout=log,stderr=log);processes.append(host)
            wait(lambda:(Path(runtime)/"icons-0").exists())
            viewer=IconViewer(port,token)
            processes.append(subprocess.Popen([str(ROOT/"build/tests/test-app")],env=env,stdout=log,stderr=log))
            window=viewer.until(lambda k,v:k=="control" and v["type"]=="window");id=window["id"]
            validate(viewer.icon(id,window["icon_revision"]),"Png")
            viewer.send(type="focus",id=id,focused=True)
            def select(name,receive=True):
                code=KEYS[name];viewer.send(type="key",id=id,key=code,down=True);viewer.send(type="key",id=id,key=code,down=False)
                app_id=name if name=="nested-app" else f"org.waybridge.{name}"
                info=viewer.until(lambda k,v:k=="control" and v["type"]=="window" and v["id"]==id and v["app_id"]==app_id)
                if receive:validate(viewer.icon(id,info["icon_revision"]),name)
                return info
            for name in ("Svg","Absolute","Inherited","Hicolor","Pixmap","nested-app","Hidden","Malformed","Huge","Missing"):select(name)
            slow=select("Slow",False);wait(lambda:(Path(runtime)/"converter.started").exists())
            next_window=select("Png",False)
            rtts=[]
            for nonce in range(3):
                start=time.monotonic();viewer.send(type="ping",nonce=nonce)
                viewer.until(lambda k,v:k=="control" and v["type"]=="pong" and v["nonce"]==nonce)
                rtts.append(time.monotonic()-start);time.sleep(.1)
            assert max(rtts)<.75,rtts
            while True:
                kind,message=viewer.receive()
                if kind=="control" and message["type"]=="window_icon":
                    assert message["revision"]!=slow["icon_revision"],"stale SVG result escaped"
                    if message["revision"]==next_window["icon_revision"]:validate(message,"Png");break
            # A fresh request reuses the bounded positive cache even after the source disappears.
            (Path(runtime)/"data/icons/Test/128x128/apps/bridge-png.png").unlink()
            select("Svg");select("Png")
            viewer.sock.close();time.sleep(.25)
            viewer=IconViewer(port,token)
            info=viewer.until(lambda k,v:k=="control" and v["type"]=="window")
            validate(viewer.icon(id,info["icon_revision"]),"Png")
            viewer.sock.close();time.sleep(.25)
            viewer=IconViewer(port,token,False)
            viewer.until(lambda k,v:k=="control" and v["type"]=="window")
            viewer.send(type="ping",nonce="legacy")
            while True:
                kind,message=viewer.receive()
                assert kind!="control" or message["type"]!="window_icon"
                if kind=="control" and message["type"]=="pong":break
            viewer.sock.close()
            print(f"PASS: real app_id/desktop/theme PNG + SVG, size ranking/inheritance/hicolor/pixmaps/absolute/nested IDs, hidden/malformed/oversized/unknown fallback, timed-out SVG responsiveness (max ping {max(rtts)*1000:.1f} ms), stale rejection, cache, reconnect and v3 opt-out")
        finally:
            for p in reversed(processes):
                if p.poll() is None:
                    p.terminate()
                    try:p.wait(timeout=3)
                    except subprocess.TimeoutExpired:p.kill();p.wait()

if __name__=="__main__":main()
