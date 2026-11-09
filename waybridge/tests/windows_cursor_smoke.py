#!/usr/bin/env python3
"""Native Windows cursor integration in a private Wine/Xvfb desktop."""
import os
import ctypes
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from cursor_smoke import wait, ROOT

class XCursorImage(ctypes.Structure):
    _fields_=[("x",ctypes.c_short),("y",ctypes.c_short),("width",ctypes.c_ushort),
              ("height",ctypes.c_ushort),("xhot",ctypes.c_ushort),("yhot",ctypes.c_ushort),
              ("serial",ctypes.c_ulong),("pixels",ctypes.POINTER(ctypes.c_ulong)),
              ("atom",ctypes.c_ulong),("name",ctypes.c_char_p)]

def cursor_reader(display):
    x11=ctypes.CDLL("libX11.so.6");fixes=ctypes.CDLL("libXfixes.so.3")
    x11.XOpenDisplay.argtypes=[ctypes.c_char_p];x11.XOpenDisplay.restype=ctypes.c_void_p
    x11.XFree.argtypes=[ctypes.c_void_p]
    connection=x11.XOpenDisplay(display.encode());assert connection
    fixes.XFixesGetCursorImage.argtypes=[ctypes.c_void_p]
    fixes.XFixesGetCursorImage.restype=ctypes.POINTER(XCursorImage)
    def read():
        ptr=fixes.XFixesGetCursorImage(connection);assert ptr
        image=ptr.contents
        result=(image.width,image.height,image.xhot,image.yhot,tuple(p & 0xffffffff for p in image.pixels[:image.width*image.height]))
        x11.XFree(ptr);return result
    return read

def main():
    subprocess.run(["bash","tests/build-fixture.sh"],cwd=ROOT,check=True)
    subprocess.run(["x86_64-w64-mingw32-g++","-std=c++17","-static","-O2","-DUNICODE","-D_UNICODE","-DWINVER=0x0A00","-D_WIN32_WINNT=0x0A00","-Iclient","tests/windows_cursor_probe.cpp","-o","build/tests/windows-cursor-probe.exe","-lcrypt32","-lgdi32"],cwd=ROOT,check=True)
    prefix=ROOT/"build/wine-cursor";prefix.mkdir(mode=0o700,parents=True,exist_ok=True)
    processes=[]
    with tempfile.TemporaryDirectory(prefix="waybridge-native-cursor-") as runtime, open(ROOT/"build/tests/windows-cursor.log","w+") as log:
        read_fd,write_fd=os.pipe()
        xvfb=subprocess.Popen(["Xvfb","-displayfd",str(write_fd),"-screen","0","1920x1200x24","-nolisten","tcp"],pass_fds=[write_fd],stdout=log,stderr=log);processes.append(xvfb);os.close(write_fd)
        with os.fdopen(read_fd) as pipe:display=":"+pipe.readline().strip()
        wine_env={**os.environ,"DISPLAY":display,"WINEPREFIX":str(prefix),"WINEDEBUG":"-all","WINEDLLOVERRIDES":"winemenubuilder.exe=d;mscoree,mshtml=","WAYBRIDGE_GDI":"1","WAYBRIDGE_DPI":"168","WAYBRIDGE_PNG":"1"}
        env={**os.environ,"XDG_RUNTIME_DIR":runtime,"WAYLAND_DISPLAY":"native-cursor-0","WAYBRIDGE_TEST_CURSOR":"1"}
        with socket.socket() as free:free.bind(("127.0.0.1",0));port=free.getsockname()[1]
        token="native-cursor-"+"8"*40;token_path=Path(runtime)/"token";token_path.write_text(token);token_path.chmod(0o600)
        def probe(*args):
            r=subprocess.run(["wine",str(ROOT/"build/tests/windows-cursor-probe.exe"),*args],env=wine_env,capture_output=True,text=True,timeout=20)
            assert r.returncode==0,(args,r.stdout,r.stderr)
            return r
        read_cursor=cursor_reader(display)
        try:
            subprocess.run(["wineboot","-u"],env=wine_env,stdout=log,stderr=log,check=True,timeout=60)
            print(probe("self").stdout.strip())
            references={}
            for name,dpi in (("text",168),("native-resize",96),("ew-resize",168),("default",168)):
                reference=subprocess.Popen(["wine",str(ROOT/"build/tests/windows-cursor-probe.exe"),"reference",name,str(dpi)],env=wine_env,stdout=subprocess.PIPE,text=True)
                processes.append(reference)
                assert reference.stdout.readline().strip()=="READY"
                references[name,dpi]=read_cursor()
                reference.wait(timeout=5)
            assert references["text",168] != references["default",168]
            assert references["ew-resize",168] != references["default",168]

            host=subprocess.Popen([str(ROOT/"target/debug/waybridge"),"--listen",f"127.0.0.1:{port}","--socket","native-cursor-0","--token-file",str(token_path),"--codec","png"],env=env,stdout=log,stderr=log);processes.append(host)
            wait(lambda:(Path(runtime)/"native-cursor-0").exists())
            processes.append(subprocess.Popen([str(ROOT/"build/tests/test-app")],env=env,stdout=log,stderr=log))
            processes.append(subprocess.Popen(["wine",str(ROOT/"build/windows/waybridge-client.exe"),"127.0.0.1",str(port),token],env=wine_env,stdout=log,stderr=log))
            time.sleep(2)
            probe("hover","20","text");assert read_cursor()==references["text",168]
            probe("hover","120","hidden");assert not any(p>>24 for p in read_cursor()[4])
            probe("hover","220","bitmap");image=read_cursor();assert image[:4]==(28,21,5,9),image[:4]
            assert image[4][3*28+2]==0xffff0000 and image[4][3*28+26]==0
            # Wine's X11 cursor conversion premultiplies the already-premultiplied
            # Win32 DIB again (128 -> 64). Keep the documented Windows format.
            assert image[4][3*28+13]==0x80004000,hex(image[4][3*28+13])
            probe("edge");assert read_cursor()==references["native-resize",96]
            probe("hover","320","bitmap");wait(lambda:read_cursor()[:4]==(28,21,5,9))
            probe("hover","420","default");assert read_cursor()==references["default",168]
            probe("hover","520","resize");assert read_cursor()==references["ew-resize",168]
            probe("hover","620","bitmap");assert read_cursor()[:4]==(33,37,0,35)
            print("PASS: actual viewer native cursor at 175%: text, hidden, RGBA bitmap/hotspot, animation, oversized fallback, resize, padded hotspot and native non-client cursor")
        finally:
            subprocess.run(["wineserver","-k"],env=wine_env,stdout=log,stderr=log,timeout=10)
            for p in reversed(processes):
                if p.poll() is None:
                    p.terminate()
                    try:p.wait(timeout=3)
                    except subprocess.TimeoutExpired:p.kill();p.wait()

if __name__=="__main__":main()
