#!/usr/bin/env python3
"""Real Wayland cursor shapes, wl_pointer surfaces, animation, DPI and legacy v3."""
import base64
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from smoke import Viewer, ROOT

class CursorViewer(Viewer):
    def __init__(self, port, token, enabled=True):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=6)
        self.geometry = {}
        self.send(type="hello", version=3, token=token, dpi=168, cursor_v1=enabled)
        self.hello = self.receive()[1]
        assert self.hello["cursor_v1"] is enabled

    def cursor(self, predicate):
        return self.until(lambda kind, v: kind == "control" and v["type"] == "cursor" and predicate(v))


def wait(check):
    deadline=time.monotonic()+15
    while not check():
        assert time.monotonic()<deadline
        time.sleep(.05)


def main():
    subprocess.run(["bash", "tests/build-fixture.sh"], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix="waybridge-cursor-") as runtime:
        env={**os.environ,"XDG_RUNTIME_DIR":runtime,"WAYLAND_DISPLAY":"cursor-0","WAYBRIDGE_TEST_CURSOR":"1"}
        with socket.socket() as free:
            free.bind(("127.0.0.1",0));port=free.getsockname()[1]
        token="cursor-test-"+"8"*40
        token_path=Path(runtime)/"token";token_path.write_text(token);token_path.chmod(0o600)
        processes=[]
        with open(ROOT/"build/tests/cursor.log","w+") as log:
            try:
                host=subprocess.Popen([str(ROOT/"target/debug/waybridge"),"--listen",f"127.0.0.1:{port}","--socket","cursor-0","--token-file",str(token_path),"--codec","png"],env=env,stdout=log,stderr=log)
                processes.append(host);wait(lambda:(Path(runtime)/"cursor-0").exists())
                viewer=CursorViewer(port,token)
                app=subprocess.Popen([str(ROOT/"build/tests/test-app"),"two"],env=env,stdout=log,stderr=log);processes.append(app)
                first=viewer.until(lambda k,v:k=="control" and v["type"]=="window")["id"]
                second=viewer.until(lambda k,v:k=="control" and v["type"]=="window" and v["id"]!=first)["id"]
                viewer.until(lambda k,v:k=="frame" and v[0]==first)
                def move(x,id=first):viewer.send(type="pointer",id=id,x=x,y=100)
                move(20);m=viewer.cursor(lambda v:v["id"]==first and v.get("name")=="text")
                generation=m["generation"]
                move(120);viewer.cursor(lambda v:v["kind"]=="hidden")
                move(220);m=viewer.cursor(lambda v:v["kind"]=="bitmap")
                assert (m["width"],m["height"],m["hotspot_x"],m["hotspot_y"],m["scale"])==(28,21,5,9,1.75),m
                pixels=base64.b64decode(m["pixels"],validate=True)
                assert len(pixels)==28*21*4
                def pixel(x,y):return tuple(pixels[(y*28+x)*4:(y*28+x+1)*4])
                assert pixel(2,3)==(255,0,0,255),pixel(2,3)
                assert pixel(13,3)==(0,128,0,128),pixel(13,3)
                assert pixel(26,3)==(0,0,0,0),pixel(26,3)
                move(920);viewer.cursor(lambda v:v["id"]==first and v.get("name")=="default")
                move(220);viewer.cursor(lambda v:v["kind"]=="bitmap")
                viewer.send(type="resize",id=first,width=800,height=500,dpi=120)
                m=viewer.cursor(lambda v:v["kind"]=="bitmap" and v["scale"]==1.25)
                assert (m["width"],m["height"],m["hotspot_x"],m["hotspot_y"])==(20,15,4,6),m
                move(320)
                updates=[];start=time.monotonic()
                while time.monotonic()-start<1.5:
                    updates.append(viewer.cursor(lambda v:v["kind"]=="bitmap"))
                assert 10<=len(updates)<=50,len(updates)
                assert all(a["generation"]<b["generation"] for a,b in zip(updates,updates[1:]))
                assert len({m["pixels"] for m in updates}) == 2, "animation buffer commits did not change pixels"
                move(420);viewer.cursor(lambda v:v["kind"]=="named" and v["name"]=="default")
                move(520);viewer.cursor(lambda v:v.get("name")=="ew-resize")
                move(620);m=viewer.cursor(lambda v:v["kind"]=="bitmap")
                assert (m["width"],m["height"],m["hotspot_x"],m["hotspot_y"])==(24,26,0,25),m
                move(720);viewer.cursor(lambda v:v["kind"]=="hidden")
                move(220);viewer.cursor(lambda v:v["kind"]=="bitmap")
                viewer.send(type="resize",id=first,width=1000,height=500,dpi=120)
                viewer.until(lambda k,v:k=="frame" and v[0:3]==(first,1250,625))
                move(820);viewer.cursor(lambda v:v["id"]==first and v.get("name")=="default")
                move(220);viewer.cursor(lambda v:v["kind"]=="bitmap")
                viewer.send(type="focus",id=second,focused=True)
                move(220)
                viewer.send(type="button",id=first,button=272,down=True)
                viewer.send(type="button",id=first,button=272,down=False)
                viewer.cursor(lambda v:v["id"]==first and v["kind"]=="bitmap")
                move(120);viewer.cursor(lambda v:v["kind"]=="hidden")
                move(20,second);viewer.cursor(lambda v:v["id"]==second and v.get("name")=="text")
                viewer.send(type="leave",id=second);viewer.cursor(lambda v:v["id"]==0)
                viewer.sock.close();time.sleep(.25)
                # Reconnect sends current state; an old/simple v3 viewer opts out entirely.
                viewer=CursorViewer(port,token,False)
                viewer.until(lambda k,v:k=="control" and v["type"]=="window")
                move(320)
                for i in range(4):
                    viewer.send(type="ping",nonce=i)
                    while True:
                        k,v=viewer.receive()
                        assert k!="control" or v["type"]!="cursor",v
                        if k=="control" and v["type"]=="pong" and v["nonce"]==i:break
                    time.sleep(.1)
                viewer.sock.close();time.sleep(.25)
                viewer=CursorViewer(port,token)
                viewer.until(lambda k,v:k=="control" and v["type"]=="window")
                move(220);m=viewer.cursor(lambda v:v["kind"]=="bitmap")
                assert m["generation"]>generation
                viewer.sock.close()
                print("PASS: cursor_v1 opt-in/legacy v3, named/hidden/custom RGBA, alpha, hotspot, 175%/125% DPI, animation callbacks/rate, oversized fallback, window switch, leave and reconnect")
            finally:
                for p in reversed(processes):
                    if p.poll() is None:
                        p.terminate()
                        try:p.wait(timeout=3)
                        except subprocess.TimeoutExpired:p.kill();p.wait()

if __name__=="__main__":main()
