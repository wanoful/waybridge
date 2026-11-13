#!/usr/bin/env python3
"""Real X11 selections/INCR ↔ Wayland and viewer clipboard; private rootless display."""
import base64,io,os,re,socket,subprocess,tempfile,time
from pathlib import Path
from PIL import Image
from smoke import ROOT,Viewer
def wait(check):
    end=time.monotonic()+15
    while time.monotonic()<end:
        value=check()
        if value:return value
        time.sleep(.05)
    raise AssertionError('X11 clipboard timeout; see x11-clipboard.log')
def main():
    (ROOT/'build/tests').mkdir(parents=True,exist_ok=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','Qt6Gui'],text=True).split()
    subprocess.run(['c++','-std=c++17','-fPIC','-no-pie','tests/x11_clipboard.cpp','-o','build/tests/x11-clipboard',*flags],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-x11-clip-') as runtime,open(ROOT/'build/tests/x11-clipboard.log','w') as log:
        env={**os.environ,'XDG_RUNTIME_DIR':runtime,'WAYLAND_DISPLAY':'x11-clip-0'}
        token='x11-clip-'+40*'d';path=Path(runtime)/'token';path.write_text(token);path.chmod(0o600)
        with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
        host=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--xwayland','--socket','x11-clip-0','--listen',f'127.0.0.1:{port}','--token-file',str(path)],env=env,stdout=log,stderr=log)
        processes=[host];viewer=None
        try:
            display=wait(lambda:re.search(r'private rootless Xwayland ready display="(:\d+)"',Path(log.name).read_text()))[1]
            xenv={**env,'DISPLAY':display,'QT_QPA_PLATFORM':'xcb'}
            viewer=Viewer(port,token)
            # Native Wayland must have keyboard focus for regular wl-paste; data-control can read without focus.
            png=io.BytesIO();Image.new('RGBA',(5,3),(23,71,129,128)).save(png,format='PNG')
            for mime,data in [('text/plain;charset=utf-8',('中文🙂 INCR\n'*18000).encode()),('image/png',png.getvalue())]:
                print('X11 clipboard MIME',mime,flush=True)
                source=Path(runtime)/'source';source.write_bytes(data)
                output=Path(runtime)/'output'
                ready=open(Path(runtime)/'copy.log','w')
                copy=subprocess.Popen([str(ROOT/'build/tests/x11-clipboard'),'copy',mime,str(source)],env=xenv,stdout=ready,stderr=log);processes.append(copy)
                wait(lambda:'READY' in Path(ready.name).read_text())
                viewer.until(lambda k,v:k=='control' and v.get('type')=='clipboard' and mime in v.get('items',{}) and base64.b64decode(v['items'][mime])==data)
                def paste_wayland():
                    p=subprocess.run(['wl-paste','--no-newline','--type',mime],env=env,capture_output=True,timeout=5)
                    return p.returncode==0 and p.stdout==data
                wait(paste_wayland)
                # Viewer/Windows → X11, preserving exact bytes and superseding the original source.
                changed=data+('来自 Windows🙂'.encode() if mime.startswith('text') else b'')
                viewer.send(type='clipboard',items={mime:base64.b64encode(changed).decode()});time.sleep(.1)
                subprocess.run([str(ROOT/'build/tests/x11-clipboard'),'paste',mime,str(output)],env=xenv,stdout=log,stderr=log,timeout=10,check=True)
                assert output.read_bytes()==changed,(mime,len(output.read_bytes()),len(changed),output.read_bytes()[-24:])
                # A real native Wayland owner → X11 (not just the viewer path).
                native=subprocess.Popen(['wl-copy','--foreground','--type',mime],env=env,stdin=subprocess.PIPE,stdout=log,stderr=log);processes.append(native)
                native.stdin.write(data);native.stdin.close();time.sleep(.2)
                subprocess.run([str(ROOT/'build/tests/x11-clipboard'),'paste',mime,str(output)],env=xenv,stdout=log,stderr=log,timeout=10,check=True)
                assert output.read_bytes()==data
                ready.close()
            print('PASS: real X11/Wayland/viewer bidirectional Unicode/emoji and PNG bytes, >200KiB INCR clipboard, source replacement/generations')
        finally:
            if viewer:viewer.sock.close()
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try:process.wait(timeout=10)
                    except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
if __name__=='__main__':main()
