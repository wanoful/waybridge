#!/usr/bin/env python3
"""Actual Windows fullscreen/limits/owner/modal/mapping/input semantics, private Wine/Xvfb."""
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time
from window_semantics_smoke import wait, ROOT

def main():
    subprocess.run(["bash","tests/build-fixture.sh"],cwd=ROOT,check=True)
    subprocess.run(["x86_64-w64-mingw32-g++","-std=c++17","-static","-O2","-DUNICODE","-D_UNICODE","-DWINVER=0x0A00","-D_WIN32_WINNT=0x0A00","-Iclient","tests/windows_semantics_probe.cpp","-o","build/tests/windows-semantics-probe.exe","-lgdi32"],cwd=ROOT,check=True)
    prefix=ROOT/"build/wine-semantics";prefix.mkdir(mode=0o700,parents=True,exist_ok=True)
    processes=[]
    with tempfile.TemporaryDirectory(prefix="waybridge-native-semantics-") as runtime,open(ROOT/"build/tests/windows-semantics.log","w+") as log:
        read_fd,write_fd=os.pipe();xvfb=subprocess.Popen(["Xvfb","-displayfd",str(write_fd),"-screen","0","1920x1200x24","-nolisten","tcp"],pass_fds=[write_fd],stdout=log,stderr=log);processes.append(xvfb);os.close(write_fd)
        with os.fdopen(read_fd) as pipe:display=":"+pipe.readline().strip()
        wine_env={**os.environ,"DISPLAY":display,"WINEPREFIX":str(prefix),"WINEDEBUG":"-all","WINEDLLOVERRIDES":"winemenubuilder.exe=d;mscoree,mshtml=","WAYBRIDGE_GDI":"1","WAYBRIDGE_PNG":"1","WAYBRIDGE_DPI":"96"}
        env={**os.environ,"XDG_RUNTIME_DIR":runtime,"WAYLAND_DISPLAY":"native-semantics-0","WAYBRIDGE_TEST_SEMANTICS":"1"}
        with socket.socket() as free:free.bind(("127.0.0.1",0));port=free.getsockname()[1]
        token="native-semantics-"+"8"*40;token_path=Path(runtime)/"token";token_path.write_text(token);token_path.chmod(0o600)
        def probe(*args,check=True):
            result=subprocess.run(["wine",str(ROOT/"build/tests/windows-semantics-probe.exe"),*map(str,args)],env=wine_env,capture_output=True,text=True,timeout=20)
            if check:assert result.returncode==0,(args,result.stdout,result.stderr)
            return result
        def state(id):return json.loads(probe("state",id).stdout)
        def key(code,target=2):probe("key",target,code)
        def full():
            s=state(1);return s["rect"]==s["monitor"] and not s["caption"] and not s["maximized"]
        def latest():
            matches=re.findall(r"^SEM_CONFIGURE 1 (.*)$",Path(log.name).read_text(),re.M)
            return list(map(int,matches[-1].split())) if matches else []
        try:
            subprocess.run(["wineboot","-u"],env=wine_env,stdout=log,stderr=log,check=True,timeout=60)
            print(probe("self").stdout.strip())
            processes.append(subprocess.Popen([str(ROOT/"target/debug/waybridge"),"--listen",f"127.0.0.1:{port}","--socket","native-semantics-0","--token-file",str(token_path),"--codec","png"],env=env,stdout=log,stderr=log))
            wait(lambda:(Path(runtime)/"native-semantics-0").exists())
            app=subprocess.Popen([str(ROOT/"build/tests/test-app")],env=env,stdout=log,stderr=log);processes.append(app)
            processes.append(subprocess.Popen(["wine",str(ROOT/"build/windows/waybridge-client.exe"),"127.0.0.1",str(port),token],env=wine_env,stdout=log,stderr=log))
            wait(lambda:all(probe("exists",i,check=False).returncode==0 for i in (1,2,3)))
            wait(lambda:all(state(i)["visible"] for i in (1,2,3)))
            wait(lambda:state(3)["owner"]==1)
            initial=state(1)["rect"];assert state(3)["owner"]==1 and not state(3)["appwindow"]
            key(30);wait(full);wait(lambda:latest()[2:4]==[1,0])
            key(30);wait(lambda:state(1)["rect"]==initial and state(1)["caption"])
            key(50);wait(lambda:state(1)["maximized"]);wait(lambda:latest()[2:4]==[0,1])
            key(30);wait(full)
            key(30);wait(lambda:state(1)["maximized"] and state(1)["caption"])
            key(19);wait(lambda:not state(1)["maximized"])
            key(49);wait(lambda:state(1)["client"][0]<=640 and state(1)["client"][1]<=480)
            probe("resize",1,1,9000);wait(lambda:state(1)["client"]==[320,480]);wait(lambda:latest()[:2]==[320,480])
            probe("resize",1,9000,1);wait(lambda:state(1)["client"]==[640,200]);wait(lambda:latest()[:2]==[640,200])
            key(31);probe("resize",1,600,420);wait(lambda:state(1)["client"]==[600,420])
            key(48);wait(lambda:state(3)["owner"]==2)
            key(22);wait(lambda:state(3)["owner"]==0 and state(3)["appwindow"])
            key(25);wait(lambda:state(3)["owner"]==1)
            key(32);wait(lambda:not state(1)["enabled"] and state(2)["enabled"] and state(3)["enabled"])
            key(48);wait(lambda:state(1)["enabled"] and not state(2)["enabled"])
            key(33,3);wait(lambda:state(2)["enabled"])
            key(32);wait(lambda:not state(2)["enabled"])
            key(34,3);wait(lambda:state(2)["enabled"])
            key(25);wait(lambda:state(3)["owner"]==1)
            key(23);wait(lambda:probe("hit",1,30,30).stdout.strip()=="-1")
            assert probe("hit",1,10,80).stdout.strip()=="1"
            assert probe("hit",1,480,100).stdout.strip()=="-1"
            assert probe("hit",1,325,75).stdout.strip()=="1"
            key(37);wait(lambda:probe("hit",1,10,80).stdout.strip()=="-1")
            key(36);wait(lambda:probe("hit",1,10,80).stdout.strip()=="1")
            key(35);wait(lambda:not state(1)["visible"]);wait(lambda:state(3)["owner"]==0)
            key(47);wait(lambda:state(1)["visible"])
            probe("hide",1);wait(lambda:not state(1)["visible"]);wait(lambda:latest()[-1]==1)
            probe("show",1);wait(lambda:state(1)["visible"]);wait(lambda:latest()[-1]==0)
            probe("minimize",1);wait(lambda:state(1)["minimized"]);wait(lambda:latest()[-1]==1)
            probe("restore",1);wait(lambda:not state(1)["minimized"]);wait(lambda:latest()[-1]==0)
            # Keep unmanaged Wine/Xvfb HWNDs from obscuring the restored window.
            probe("move",2,800,500);probe("move",3,1100,650);key(24);wait(lambda:(lambda p:len(p)==3 and int(p[0])<10 and int(p[1])<10 and int(p[2])>240)(probe("pixel",1,20,80).stdout.split()))
            key(25);wait(lambda:state(3)["owner"]==1)
            key(45);wait(lambda:probe("exists",1,check=False).returncode!=0)
            wait(lambda:state(3)["owner"]==0 and state(3)["enabled"] and state(3)["visible"]);assert app.poll() is None
            key(48);wait(lambda:state(3)["owner"]==2)
            key(32);wait(lambda:not state(2)["enabled"])
            key(44,3);wait(lambda:not state(3)["visible"])
            wait(lambda:state(2)["enabled"] and state(2)["foreground"])
            key(21,2);wait(lambda:state(3)["visible"] and not state(2)["enabled"])
            key(46,3);wait(lambda:probe("exists",3,check=False).returncode!=0)
            wait(lambda:state(2)["enabled"] and state(2)["foreground"])
            key(12);wait(lambda:"SEM_KEY 2 12" in Path(log.name).read_text())
            print("PASS: actual Win32 fullscreen/maximize/placement, min/max native resize, owner reparent/clear/survival, real modal disable+cleanup, HTTRANSPARENT input masks, unmap/remap, minimize/suspended/restore and fresh pixels after hidden-frame credits")
        finally:
            subprocess.run(["wineserver","-k"],env=wine_env,stdout=log,stderr=log,timeout=10)
            for p in reversed(processes):
                if p.poll() is None:
                    p.terminate()
                    try:p.wait(timeout=3)
                    except subprocess.TimeoutExpired:p.kill();p.wait()

if __name__=="__main__":main()
