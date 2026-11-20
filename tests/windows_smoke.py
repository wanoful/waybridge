#!/usr/bin/env python3
"""Run the actual PE viewer in an isolated Wine + Xvfb desktop, never the user's desktop."""
import os
import re
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from urllib.parse import unquote, urlparse

ROOT=Path(__file__).resolve().parents[1]


def wait_for(check, seconds=15):
    deadline=time.monotonic()+seconds
    while time.monotonic()<deadline:
        result=check()
        if result:return result
        time.sleep(0.1)
    raise AssertionError("timed out waiting for native Windows operation")


def main():
    csd="--csd" in sys.argv
    terminal="--terminal" in sys.argv
    x11="--x11" in sys.argv
    ime="--ime" in sys.argv
    rich_clipboard="--rich-clipboard" in sys.argv
    qt="--qt" in sys.argv or ime
    udp="--udp" in sys.argv
    proxy=None
    subprocess.run(["bash","tests/build-fixture.sh"],cwd=ROOT,check=True)
    subprocess.run(["x86_64-w64-mingw32-g++","-municode","-static","-O2","tests/windows_probe.cpp","-o","build/tests/windows-probe.exe","-lgdi32","-lshell32","-lole32","-luuid"],cwd=ROOT,check=True)
    prefix=ROOT/"build/wine-smoke";prefix.mkdir(mode=0o700,parents=True,exist_ok=True)
    processes=[]
    with tempfile.TemporaryDirectory(prefix="waybridge-native-") as runtime, open(ROOT/"build/tests/native.log","w+") as log:
        read_fd,write_fd=os.pipe()
        xvfb=subprocess.Popen(["Xvfb","-displayfd",str(write_fd),"-screen","0","1280x1024x24","-nolisten","tcp"],pass_fds=[write_fd],stdout=log,stderr=log)
        processes.append(xvfb);os.close(write_fd)
        with os.fdopen(read_fd) as pipe:display=":"+pipe.readline().strip()
        wine_env={**os.environ,"DISPLAY":display,"WINEPREFIX":str(prefix),"WINEDEBUG":"-all","WINEDLLOVERRIDES":"winemenubuilder.exe=d;mscoree,mshtml=", "WAYBRIDGE_GDI":"1"}
        if qt:
            wine_env.update(WAYBRIDGE_DPI="168", WAYBRIDGE_TEST_WINDOW="Waybridge Qt fixture")
        env={**os.environ,"XDG_RUNTIME_DIR":runtime,"WAYLAND_DISPLAY":"native-0","WAYBRIDGE_TEST_DROP":str(Path(runtime)/"drop.txt")}
        if csd:env["WAYBRIDGE_FIXTURE_CSD"]="1"
        else:env.pop("WAYBRIDGE_FIXTURE_CSD",None)
        with socket.socket() as free:free.bind(("127.0.0.1",0));port=free.getsockname()[1]
        if udp:
            from udp_proxy import Proxy
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as free:free.bind(("127.0.0.1",0));udpport=free.getsockname()[1]
            proxy=Proxy(("127.0.0.1",udpport),faults=True)
            wine_env.update(WAYBRIDGE_UDP_HOST="127.0.0.1",WAYBRIDGE_UDP_PORT=str(proxy.port))
        token="native-smoke-"+"8"*40;token_file=Path(runtime)/"token";token_file.write_text(token);token_file.chmod(0o600)
        def probe(*args,check=True):
            return subprocess.run(["wine",str(ROOT/"build/tests/windows-probe.exe"),*args],env=wine_env,capture_output=True,text=True,timeout=20,check=check)
        def pasted(mime):
            result=subprocess.run(["wl-paste","--no-newline","--type",mime],env=env,capture_output=True,timeout=3)
            return result.stdout if result.returncode==0 else b""
        def copy(data,mime):
            p=subprocess.Popen(["wl-copy","--foreground","--type",mime],env=env,stdin=subprocess.PIPE,stdout=log,stderr=log)
            processes.append(p);p.stdin.write(data);p.stdin.close()
        try:
            subprocess.run(["wineboot","-u"],env=wine_env,stdout=log,stderr=log,check=True,timeout=60)
            version=subprocess.run(["wine",str(ROOT/"build/windows/waybridge-client.exe"),"--version"],env=wine_env,capture_output=True,text=True,timeout=15,check=True)
            assert "0.3.0" in version.stdout,version.stdout
            subprocess.run(["wine",str(ROOT/"build/windows/waybridge-client.exe"),"--check-shaders"],env=wine_env,stdout=log,stderr=log,timeout=15,check=True)
            host=subprocess.Popen([str(ROOT/"target/debug/waybridge"),"--socket","native-0","--listen",f"127.0.0.1:{port}","--token-file",str(token_file),"--codec",os.environ.get("WAYBRIDGE_TEST_CODEC","auto"),*(["--xwayland"] if x11 else []),*(["--udp-listen",f"127.0.0.1:{udpport}"] if udp else [])],env=env,stdout=log,stderr=log)
            processes.append(host);wait_for(lambda:(Path(runtime)/"native-0").exists())
            if x11:
                display=wait_for(lambda:re.search(r'private rootless Xwayland ready display="(:\d+)"',Path(log.name).read_text()))[1]
                env["DISPLAY"]=display
                env["WAYBRIDGE_INPUT_TRACE"]="1"
                client=subprocess.Popen(["wine",str(ROOT/"build/windows/waybridge-client.exe"),"127.0.0.1",str(port),token],env=wine_env,stdout=log,stderr=log)
                processes.append(client);time.sleep(.3)

            app_log=open(ROOT/"build/tests/native-app.log","w+")
            if qt:
                flags=subprocess.check_output(["pkg-config","--cflags","--libs","Qt6Widgets"],text=True).split()
                subprocess.run(["c++","-std=c++17","-fPIC","-no-pie","tests/qt_app.cpp","-o","build/tests/qt-app",*flags],cwd=ROOT,check=True)
                env["QT_QPA_PLATFORM"]="xcb" if x11 else "wayland"
                for name in ("QT_SCALE_FACTOR","QT_SCREEN_SCALE_FACTORS","QT_WAYLAND_DISABLE_WINDOWDECORATION"):env.pop(name,None)
                if ime:
                    env.pop('QT_IM_MODULE',None);env.pop('GTK_IM_MODULE',None)
                    env['WAYLAND_DEBUG']='1'
            app=subprocess.Popen([str(ROOT/"build/tests/qt-app")] if qt else (["weston-terminal"] if terminal else [str(ROOT/"build/tests/test-app")]),env=env,stdout=app_log,stderr=app_log);processes.append(app)
            if not x11:
                client=subprocess.Popen(["wine",str(ROOT/"build/windows/waybridge-client.exe"),"127.0.0.1",str(port),token],env=wine_env,stdout=log,stderr=log);processes.append(client)
            if qt:
                wait_for(lambda:"DPR 1.75" in Path(app_log.name).read_text())
                wait_for(lambda:probe("frame","server",check=False).returncode==0)
                probe("click","140","70");probe("type","a")
                wait_for(lambda:"TEXT a\n" in Path(app_log.name).read_text())
                if ime:
                    probe('unicode','你好🙂')
                    wait_for(lambda:'TEXT a你好🙂\n' in Path(app_log.name).read_text())
                    probe('type','b')
                    wait_for(lambda:'TEXT a你好🙂b\n' in Path(app_log.name).read_text())
                    caret=probe('caret').stdout
                    coords=list(map(int,re.search(r'CARET (-?\d+) (-?\d+) (-?\d+) (-?\d+)',caret).groups()))
                    assert coords[0]>20 and coords[1]>=20 and coords[3]>coords[1],coords
                    remote_rects=re.findall(r'set_cursor_rectangle\((-?\d+), (-?\d+), (-?\d+), (-?\d+)\)',Path(app_log.name).read_text())
                    assert remote_rects
                    remote=list(map(int,remote_rects[-1]))
                    assert abs(coords[0]-remote[0]*1.75)<=2 and abs(coords[1]-remote[1]*1.75)<=2,(coords,remote)
                    probe('shortcut','A');probe('unicode','中文输入')
                    wait_for(lambda:'TEXT 中文输入\n' in Path(app_log.name).read_text())
                    probe('shortcut',' ')
                    wait_for(lambda:'CTRL_SPACE\n' in Path(app_log.name).read_text())
                    assert re.findall(r'^TEXT (.*)$',Path(app_log.name).read_text(),re.M)[-1]=='中文输入','shortcut must not also commit a translated space'
                    probe('clipboard','IME copy baseline')
                    wait_for(lambda:pasted('text/plain;charset=utf-8')==b'IME copy baseline')
                    probe('shortcut','A');probe('shortcut','V')
                    wait_for(lambda:'TEXT IME copy baseline\n' in Path(app_log.name).read_text())
                    probe('shortcut','A');probe('unicode','中文输入')
                    wait_for(lambda:re.findall(r'^TEXT (.*)$',Path(app_log.name).read_text(),re.M)[-1]=='中文输入')
                    probe('shortcut','A');probe('ime-held-copy')
                    wait_for(lambda:pasted('text/plain;charset=utf-8')=='中文输入'.encode())
                    probe('unicode','保留🙂');probe('type','c')
                    wait_for(lambda:'TEXT 保留🙂c\n' in Path(app_log.name).read_text())
                    print('PASS: actual Win32 Unicode/emoji and English -> real Qt text-input-v3 at 175%; Ctrl+A, Ctrl+Space once, native clipboard Ctrl+V paste, held Ctrl+C after controlled composition, no stuck Ctrl, native caret (Wine; Microsoft IME engine/candidate UI still needs physical Windows)')
                    return
                probe("click","140","175")
                menu=wait_for(lambda:re.search(r"^MENU (-?\d+) (-?\d+) (\d+) (\d+)$",Path(app_log.name).read_text(),re.M))
                x,y,w,h=map(int,menu.groups());probe("click",str(round((x+w/2)*1.75)),str(round((y+h/2)*1.75)))
                wait_for(lambda:"ACTION\n" in Path(app_log.name).read_text())
                probe("resize")
                before=probe("geometry").stdout
                probe("shortcut","O")
                popup=wait_for(lambda:(lambda r:r.stdout if r.returncode==0 else None)(probe("popup",check=False)))
                assert probe("geometry").stdout==before
                probe("offset","-20","20")
                after=probe("popup").stdout
                old=list(map(int,re.search(r"POPUP (-?\d+) (-?\d+) (-?\d+) (-?\d+)",popup).groups()))
                moved=list(map(int,re.search(r"POPUP (-?\d+) (-?\d+) (-?\d+) (-?\d+)",after).groups()))
                assert moved[0]-old[0]==-20 and moved[1]-old[1]==20,(old,moved)
                probe("popup-click-stale-leave","21")
                wait_for(lambda:"OVERFLOW_ACTION\n" in Path(app_log.name).read_text())
                wait_for(lambda:probe("popup","none",check=False).returncode==0)
                probe("shortcut","O")
                wait_for(lambda:probe("popup",check=False).returncode==0)
                probe("popup-click","-21")
                wait_for(lambda:probe("popup","nested",check=False).returncode==0)
                probe("popup-click","21")
                wait_for(lambda:"NESTED_ACTION\n" in Path(app_log.name).read_text())
                wait_for(lambda:probe("popup","none",check=False).returncode==0)
                probe("click","438","175")
                wine_env["WAYBRIDGE_TEST_WINDOW"]="Waybridge Qt dialog"
                probe("owner");probe("click","140","70");probe("type","c")
                wait_for(lambda:"DIALOG_TEXT c\n" in Path(app_log.name).read_text())
                print("PASS: native Windows frame -> actual Qt at 175%; mouse and keyboard reach text field, popup action, independent overflow/nested popups, owner movement, unchanged parent geometry, mouse capture and owned modal dialog (Wine buffered fallback)")
                app_log.close();return
            if terminal:
                probe("frame","client")
                probe("move")
                subprocess.run(["ffmpeg","-hide_banner","-loglevel","error","-f","x11grab","-video_size","1280x1024","-i",display,"-frames:v","1","-y",str(ROOT/"build/tests/weston-terminal-window.png")],stdout=log,stderr=log,timeout=10,check=True)
                print("PASS: actual weston-terminal has no outer Windows title bar; native frame equals client area; its own title bar moves the Windows window")
                app_log.close()
                return
            result=probe("inspect");assert "TOP 254 0 0" in result.stdout or "TOP 255 0 0" in result.stdout,result.stdout
            probe("frame","client" if csd else "server")
            probe("resize")
            wait_for(lambda:"700 500" in Path(app_log.name).read_text())
            if csd:
                probe("move")
                probe("click","-60","16")
                wait_for(lambda:probe("state","maximized",check=False).returncode==0)
                probe("click","-60","16")
                wait_for(lambda:probe("state","restored",check=False).returncode==0)
                probe("click","-100","16")
                wait_for(lambda:probe("state","minimized",check=False).returncode==0)
                probe("restore")
            probe("clipboard","Windows 你好 ✓")
            wait_for(lambda:pasted("text/plain;charset=utf-8")=="Windows 你好 ✓".encode())
            copy("Linux 你好 ✓".encode(),"text/plain;charset=utf-8")
            wait_for(lambda:(lambda r:r.returncode==0 and "Linux 你好 ✓" in r.stdout)(probe("clipboard",check=False)))
            if rich_clipboard:
                probe("rich-clipboard","set")
                wait_for(lambda:b"<b>"+ "你好🙂".encode() + b"</b>" in pasted("text/html"))
                wait_for(lambda:pasted("application/rtf")==br"{\rtf1\ansi Windows \b bold\b0}")
                wait_for(lambda:pasted("text/plain;charset=utf-8")==b"Windows plain fallback")
                markup = "<p><em>Linux 你好🙂</em></p>"
                copy(markup.encode(),"text/html")
                wait_for(lambda:markup in probe("rich-clipboard","html",check=False).stdout)
                rtf = r"{\rtf1\ansi Linux \i italic\i0}"
                copy(rtf.encode(),"application/rtf")
                wait_for(lambda:probe("rich-clipboard","rtf",check=False).stdout==rtf)
                print("PASS: native CF_HTML and CF_RTF round-trip with Wayland MIME, UTF-8 byte offsets and plain fallback")
                app_log.close()
                return
            data=os.urandom(180_000);source=Path(runtime)/"native-file.bin";source.write_bytes(data)
            win_source="Z:"+str(source).replace("/","\\")
            probe("copyfile",win_source)
            uri=wait_for(lambda:pasted("text/uri-list"))
            uploaded=Path(unquote(urlparse(uri.decode().strip()).path));assert uploaded.read_bytes()==data
            exported=Path(runtime)/"export-file.bin";exported.write_bytes(data)
            from windows_readonly import cache_roots, assert_refused
            previous=cache_roots(prefix)
            copy((exported.as_uri()+"\r\n").encode(),"text/uri-list")
            assert_refused(prefix,previous,log,probe,"export-file.bin")
            probe("drop",win_source)
            drop=Path(runtime)/"drop.txt"
            wait_for(lambda:drop.exists() and drop.stat().st_size)
            dropped=Path(unquote(urlparse(drop.read_text().strip()).path));assert dropped.read_bytes()==data
            if csd:
                probe("click","-20","16")
                wait_for(lambda:app.poll() is not None)
            print("PASS: native Win32 window paints correct pixels; native resize reaches Wayland; Windows/Linux Unicode clipboard; Windows→Linux file clipboard; Wine rejects unsupported read-only Linux→Windows cache without publishing/leaking; OLE file drop reaches Wayland with exact bytes (Wine/Xvfb)")
            print("PASS: single decoration owner ("+("CSD fallback; move, maximize, restore, minimize, close" if csd else "negotiated native Windows frame")+")")
            app_log.close()
        finally:
            if proxy:
                proxy.close()
            subprocess.run(["wineserver","-k"],env=wine_env,stdout=log,stderr=log,timeout=10)
            for p in reversed(processes):
                if p.poll() is None:
                    p.terminate()
                    try:p.wait(timeout=3)
                    except subprocess.TimeoutExpired:p.kill();p.wait()
            if proxy:
                assert proxy.received>0,"test must carry actual UDP shards"
                log.flush()
                assert "UDP ordered media active" in Path(log.name).read_text()


if __name__=="__main__":main()
