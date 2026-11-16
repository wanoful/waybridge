#!/usr/bin/env python3
"""Actual Windows HICON/WM_GETICON and native caption rendering under isolated Wine."""
import os
import ctypes
import re
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from app_icon_smoke import fixtures, wait, ROOT, KEYS

def xicons(display):
    # Wine cannot export process-local HICON handles through cross-process
    # WM_GETICON. Read the actual icon data that user32 publishes to X11 instead.
    lib=ctypes.CDLL("libX11.so.6")
    lib.XOpenDisplay.argtypes=[ctypes.c_char_p];lib.XOpenDisplay.restype=ctypes.c_void_p
    connection=lib.XOpenDisplay(display.encode());assert connection
    lib.XInternAtom.argtypes=[ctypes.c_void_p,ctypes.c_char_p,ctypes.c_int];lib.XInternAtom.restype=ctypes.c_ulong
    atom=lib.XInternAtom(connection,b"_NET_WM_ICON",False)
    lib.XGetWindowProperty.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.c_ulong,ctypes.c_long,ctypes.c_long,ctypes.c_int,ctypes.c_ulong,ctypes.POINTER(ctypes.c_ulong),ctypes.POINTER(ctypes.c_int),ctypes.POINTER(ctypes.c_ulong),ctypes.POINTER(ctypes.c_ulong),ctypes.POINTER(ctypes.c_void_p)]
    lib.XFree.argtypes=[ctypes.c_void_p]
    lib.XDefaultRootWindow.argtypes=[ctypes.c_void_p];lib.XDefaultRootWindow.restype=ctypes.c_ulong
    lib.XFetchName.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.POINTER(ctypes.c_void_p)]
    lib.XQueryTree.argtypes=[ctypes.c_void_p,ctypes.c_ulong,ctypes.POINTER(ctypes.c_ulong),ctypes.POINTER(ctypes.c_ulong),ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),ctypes.POINTER(ctypes.c_uint)]
    def window():
        pending=[lib.XDefaultRootWindow(connection)]
        for _ in range(4096):
            if not pending:return None
            item=pending.pop();name=ctypes.c_void_p()
            if lib.XFetchName(connection,item,ctypes.byref(name)) and name.value:
                title=ctypes.string_at(name);lib.XFree(name)
                if title==b"Waybridge fixture 1":return item
            root=ctypes.c_ulong();parent=ctypes.c_ulong();count=ctypes.c_uint();children=ctypes.POINTER(ctypes.c_ulong)()
            if lib.XQueryTree(connection,item,ctypes.byref(root),ctypes.byref(parent),ctypes.byref(children),ctypes.byref(count)):
                pending.extend(children[:count.value])
                if children:lib.XFree(children)
        return None
    def read():
        found=window()
        if found is None:return None
        actual=ctypes.c_ulong();fmt=ctypes.c_int();count=ctypes.c_ulong();remaining=ctypes.c_ulong();data=ctypes.c_void_p()
        lib.XGetWindowProperty(connection,found,atom,0,32768,False,0,ctypes.byref(actual),ctypes.byref(fmt),ctypes.byref(count),ctypes.byref(remaining),ctypes.byref(data))
        if not data.value:return None
        result=None
        if fmt.value==32 and not remaining.value and count.value<=32768:
            values=ctypes.cast(data,ctypes.POINTER(ctypes.c_ulong))
            result=tuple(v & 0xffffffff for v in values[:count.value])
        lib.XFree(data);return result
    return read

def color(icon):
    if not icon or len(icon)<3:return None
    width,height=icon[:2]
    return icon[2+(height//2)*width+width//2]

def main():
    subprocess.run(["bash","tests/build-fixture.sh"],cwd=ROOT,check=True)
    subprocess.run(["x86_64-w64-mingw32-g++","-std=c++17","-static","-O2","-DUNICODE","-D_UNICODE","-DWINVER=0x0A00","-D_WIN32_WINNT=0x0A00","-Iclient","tests/windows_icon_probe.cpp","-o","build/tests/windows-icon-probe.exe","-lole32","-lwindowscodecs","-luuid","-lgdi32","-lshell32"],cwd=ROOT,check=True)
    prefix=ROOT/"build/wine-app-icons";prefix.mkdir(mode=0o700,parents=True,exist_ok=True)
    processes=[]
    with tempfile.TemporaryDirectory(prefix="waybridge-native-icons-") as runtime,open(ROOT/"build/tests/windows-app-icons.log","w+") as log:
        env=fixtures(runtime);env.update(XDG_RUNTIME_DIR=runtime,WAYLAND_DISPLAY="native-icons-0")
        read_fd,write_fd=os.pipe();xvfb=subprocess.Popen(["Xvfb","-displayfd",str(write_fd),"-screen","0","1920x1200x24","-nolisten","tcp"],pass_fds=[write_fd],stdout=log,stderr=log);processes.append(xvfb);os.close(write_fd)
        with os.fdopen(read_fd) as pipe:display=":"+pipe.readline().strip()
        wine_env={**os.environ,"DISPLAY":display,"WINEPREFIX":str(prefix),"WINEDEBUG":"-all","WINEDLLOVERRIDES":"winemenubuilder.exe=d;mscoree,mshtml=","WAYBRIDGE_GDI":"1","WAYBRIDGE_DPI":"168","WAYBRIDGE_PNG":"1","WAYBRIDGE_ICON_TRACE":"1"}
        with socket.socket() as free:free.bind(("127.0.0.1",0));port=free.getsockname()[1]
        token="native-icons-"+"8"*40;token_path=Path(runtime)/"token";token_path.write_text(token);token_path.chmod(0o600)
        def probe(*args,check=True):
            r=subprocess.run(["wine",str(ROOT/"build/tests/windows-icon-probe.exe"),*args],env=wine_env,capture_output=True,text=True,timeout=20)
            if check:assert r.returncode==0,(args,r.stdout,r.stderr)
            return r
        inspect=xicons(display)
        def change(name):probe("key",str(KEYS[name]));time.sleep(.15)
        try:
            subprocess.run(["wineboot","-u"],env=wine_env,stdout=log,stderr=log,check=True,timeout=60)
            path="Z:"+str(Path(runtime)/"data/icons/Test/128x128/apps/bridge-png.png").replace("/","\\")
            print(probe("self",path).stdout.strip())
            processes.append(subprocess.Popen([str(ROOT/"target/debug/waybridge"),"--listen",f"127.0.0.1:{port}","--socket","native-icons-0","--token-file",str(token_path),"--codec","png"],env=env,stdout=log,stderr=log))
            wait(lambda:(Path(runtime)/"native-icons-0").exists())
            processes.append(subprocess.Popen([str(ROOT/"build/tests/test-app")],env=env,stdout=log,stderr=log))
            processes.append(subprocess.Popen(["wine",str(ROOT/"build/windows/waybridge-client.exe"),"127.0.0.1",str(port),token],env=wine_env,stdout=log,stderr=log))
            wait(lambda:probe("ready",check=False).returncode==0)
            wait(lambda:color(inspect())==0xffff0000)
            change("Svg");wait(lambda:color(inspect())==0xff0000ff);assert inspect()[2]==0
            change("Inherited");wait(lambda:color(inspect())==0xff00ff00)
            change("Missing");wait(lambda:color(inspect())!=0xff00ff00);fallback=inspect()
            change("Hidden");wait(lambda:inspect()==fallback)
            change("Png");wait(lambda:color(inspect())==0xffff0000)
            change("Huge");wait(lambda:inspect()==fallback)
            traces=re.findall(r"Icon apply dpi=168 large=([0-9a-f]+) small=([0-9a-f]+) getLarge=([0-9a-f]+) getSmall=([0-9a-f]+)",Path(log.name).read_text())
            assert len(traces)>=6 and all(a==c and b==d and int(a,16) and int(b,16) for a,b,c,d in traces),traces
            print("PASS: actual viewer PNG/SVG/inherited native icon pixels and unknown/hidden/oversized fallback via Wine's X11 icon metadata; same-process WM_GETICON large/small readback at 175%")

        finally:
            subprocess.run(["wineserver","-k"],env=wine_env,stdout=log,stderr=log,timeout=10)
            for p in reversed(processes):
                if p.poll() is None:
                    p.terminate()
                    try:p.wait(timeout=3)
                    except subprocess.TimeoutExpired:p.kill();p.wait()

if __name__=="__main__":main()
