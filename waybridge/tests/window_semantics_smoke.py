#!/usr/bin/env python3
"""Real xdg-shell/dialog states, constrained configure, mapping and bounded input regions."""
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time
from smoke import Viewer, ROOT

def wait(check,seconds=12):
    end=time.monotonic()+seconds
    while not check():
        assert time.monotonic()<end,"timed out"
        time.sleep(.03)

class SemViewer(Viewer):
    def __init__(self,port,token,enabled=True):
        self.sock=socket.create_connection(("127.0.0.1",port),timeout=8);self.geometry={};self.infos={};self.sequences={}
        self.send(type="hello",version=3,token=token,dpi=168,window_semantics_v1=enabled,frame_ack=True)
        assert self.receive()[1]["window_semantics_v1"] is enabled
    def receive(self):
        kind,value=super().receive()
        if kind=="control" and value["type"]=="window":self.infos[value["id"]]=value
        if kind=="frame":self.sequences[value[0]]=value[3];self.send(type="frame_ack",id=value[0],sequence=value[3])
        return kind,value
    def metadata(self,id,predicate):
        return self.until(lambda k,v:k=="control" and v["type"]=="window" and v["id"]==id and predicate(v))

def hit(region,x,y):
    for layer in region["layers"]:
        lx=x+region["origin_x"]-layer["x"];ly=y+region["origin_y"]-layer["y"]
        if not(0<=lx<layer["width"] and 0<=ly<layer["height"]):continue
        sensitive=layer["rects"] is None
        for add,rx,ry,rw,rh in layer["rects"] or []:
            if rx<=lx<rx+rw and ry<=ly<ry+rh:sensitive=add
        if sensitive:return True
    return False

def main():
    subprocess.run(["bash","tests/build-fixture.sh"],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix="waybridge-semantics-") as runtime,open(ROOT/"build/tests/window-semantics.log","w+") as log:
        env={**os.environ,"XDG_RUNTIME_DIR":runtime,"WAYLAND_DISPLAY":"semantics-0","WAYBRIDGE_TEST_SEMANTICS":"1"}
        with socket.socket() as free:free.bind(("127.0.0.1",0));port=free.getsockname()[1]
        token="semantics-"+"8"*40;token_path=Path(runtime)/"token";token_path.write_text(token);token_path.chmod(0o600)
        processes=[]
        try:
            host=subprocess.Popen([str(ROOT/"target/debug/waybridge"),"--listen",f"127.0.0.1:{port}","--socket","semantics-0","--token-file",str(token_path),"--codec","png"],env=env,stdout=log,stderr=log);processes.append(host)
            wait(lambda:(Path(runtime)/"semantics-0").exists())
            viewer=SemViewer(port,token)
            app=subprocess.Popen([str(ROOT/"build/tests/test-app")],env=env,stdout=log,stderr=log);processes.append(app)
            while len(viewer.infos)<3 or len(viewer.sequences)<3:viewer.receive()
            ids={int(info["title"].split()[-1]):id for id,info in viewer.infos.items()};a,b,dialog=ids[1],ids[2],ids[3]
            if viewer.infos[dialog]["parent"]!=a:viewer.metadata(dialog,lambda v:v["parent"]==a)
            def key(code,target=dialog):
                viewer.send(type="focus",id=target,focused=True);viewer.send(type="key",id=target,key=code,down=True);viewer.send(type="key",id=target,key=code,down=False)
            def resize(width,height,full=False,maximized=False,epoch=None):
                viewer.send(type="resize",id=a,width=width,height=height,dpi=168,fullscreen=full,maximized=maximized,fullscreen_epoch=viewer.infos[a]["fullscreen_epoch"] if epoch is None else epoch)
            def frame(width,height):return viewer.until(lambda k,v:k=="frame" and v[0:3]==(a,round(width*1.75),round(height*1.75)))
            key(49);viewer.metadata(a,lambda v:v["min_size"]==[320,200] and v["max_size"]==[640,480])
            resize(5,9000);frame(320,480)
            resize(9000,5);frame(640,200)
            key(30);request=viewer.until(lambda k,v:k=="control" and v["type"]=="window_action" and v["action"]=="fullscreen")
            full_info=viewer.metadata(a,lambda v:v["fullscreen"])
            resize(1100,700,True,epoch=request["fullscreen_epoch"]);frame(1100,700)
            wait(lambda:"SEM_CONFIGURE 1 1100 700 1 0" in Path(log.name).read_text())
            key(30);viewer.until(lambda k,v:k=="control" and v["type"]=="window_action" and v["action"]=="unfullscreen")
            restored=viewer.metadata(a,lambda v:not v["fullscreen"])
            resize(800,600,True,epoch=full_info["fullscreen_epoch"])
            viewer.send(type="ping",nonce="old-fullscreen");viewer.until(lambda k,v:k=="control" and v["type"]=="pong")
            assert viewer.infos[a]["fullscreen_epoch"]==restored["fullscreen_epoch"] and not viewer.infos[a]["fullscreen"]
            resize(640,480);frame(640,480)
            key(50);viewer.until(lambda k,v:k=="control" and v["type"]=="window_action" and v["action"]=="maximize")
            resize(640,480,maximized=True);frame(640,480)
            wait(lambda:"SEM_CONFIGURE 1 640 480 0 1" in Path(log.name).read_text())
            key(19);viewer.until(lambda k,v:k=="control" and v["type"]=="window_action" and v["action"]=="restore")
            resize(560,420);frame(560,420)
            key(31);viewer.metadata(a,lambda v:v["max_size"]==[0,0])
            key(48);viewer.metadata(dialog,lambda v:v["parent"]==b)
            key(22);viewer.metadata(dialog,lambda v:v["parent"] is None)
            key(25);viewer.metadata(dialog,lambda v:v["parent"]==a)
            key(32);viewer.metadata(dialog,lambda v:v["modal"])
            before=len(Path(log.name).read_text());viewer.send(type="focus",id=a,focused=True)
            viewer.send(type="pointer",id=a,x=100,y=100);viewer.send(type="button",id=a,button=272,down=True);viewer.send(type="button",id=a,button=272,down=False)
            viewer.send(type="key",id=a,key=12,down=True);viewer.send(type="key",id=a,key=12,down=False);time.sleep(.15)
            appended=Path(log.name).read_text()[before:];assert "SEM_BUTTON 1" not in appended and "SEM_KEY 1" not in appended,appended
            key(12);wait(lambda:"SEM_KEY 3 12" in Path(log.name).read_text())
            key(33);viewer.metadata(dialog,lambda v:not v["modal"])
            key(32);viewer.metadata(dialog,lambda v:v["modal"])
            key(34);viewer.metadata(dialog,lambda v:not v["modal"])
            key(23);region=viewer.metadata(a,lambda v:isinstance(v.get("input_region"),dict) and len(v["input_region"]["layers"])==2)["input_region"]
            assert hit(region,10,10) and not hit(region,30,30) and not hit(region,region["width"]*.8,100)
            assert hit(region,region["width"]//2+25,75),region
            key(37);region=viewer.metadata(a,lambda v:v.get("input_region") and len(v["input_region"]["layers"])==1 and v["input_region"]["layers"][0]["rects"]==[])["input_region"]
            assert not hit(region,10,10)
            key(36);viewer.metadata(a,lambda v:v.get("input_region") and v["input_region"]["layers"][0]["rects"] is None)
            key(35);hidden=viewer.until(lambda k,v:k=="control" and v["type"]=="window_visibility" and v["id"]==a and not v["mapped"])
            viewer.metadata(dialog,lambda v:v["parent"] is None)
            key(47);mapped=viewer.until(lambda k,v:k=="control" and v["type"]=="window_visibility" and v["id"]==a and v["mapped"])
            viewer.send(type="window_state",id=a,visible=False,minimized=True,mapping_epoch=hidden["mapping_epoch"])
            viewer.until(lambda k,v:k=="frame" and v[0]==a)
            current=viewer.infos[a].get("mapping_epoch",mapped["mapping_epoch"])
            viewer.send(type="window_state",id=a,visible=False,minimized=True,mapping_epoch=mapped["mapping_epoch"])
            resize(600,400)
            wait(lambda:re.search(r"SEM_CONFIGURE 1 600 400 [01] [01] [01] 1",Path(log.name).read_text()))
            # Drain older credits; hidden resizes must not strand a restored viewer.
            viewer.send(type="window_state",id=a,visible=True,minimized=False,mapping_epoch=mapped["mapping_epoch"])
            frame(600,400)
            wait(lambda:re.search(r"SEM_CONFIGURE 1 600 400 [01] [01] [01] 0",Path(log.name).read_text()))
            assert mapped["mapping_epoch"]>hidden["mapping_epoch"]>=current-1
            key(25);viewer.metadata(dialog,lambda v:v["parent"]==a)
            key(45);viewer.until(lambda k,v:k=="control" and v["type"]=="window_closed" and v["id"]==a)
            viewer.metadata(dialog,lambda v:v["parent"] is None);assert app.poll() is None
            key(12);key(46);viewer.until(lambda k,v:k=="control" and v["type"]=="window_closed" and v["id"]==dialog)
            key(12,b);wait(lambda:"SEM_KEY 2 12" in Path(log.name).read_text())
            viewer.sock.close();time.sleep(.25);viewer=SemViewer(port,token,False)
            remaining=viewer.until(lambda k,v:k=="control" and v["type"]=="window");assert remaining["id"]==b and "min_size" not in remaining
            wait(lambda:set(map(int,re.findall(r"^SEM_CAPS(.*)$",Path(log.name).read_text(),re.M)[-1].split()))=={2,4})
            caps=set(map(int,re.findall(r"^SEM_CAPS(.*)$",Path(log.name).read_text(),re.M)[-1].split()));assert caps=={2,4},caps
            viewer.send(type="close",id=b);wait(lambda:app.poll() is not None);viewer.sock.close()
            print("PASS: real xdg fullscreen vs maximize, stale fullscreen epoch, min/max configure, dynamic owner clear/reparent/death, xdg-dialog modal+destroy/input blocking, surface-tree add/subtract/empty input regions, unmap/remap epochs, suspended/minimized restore credits and legacy v3 capabilities")
        finally:
            for p in reversed(processes):
                if p.poll() is None:
                    p.terminate()
                    try:p.wait(timeout=3)
                    except subprocess.TimeoutExpired:p.kill();p.wait()

if __name__=="__main__":main()
