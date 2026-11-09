#!/usr/bin/env python3
"""Real accelerated GLX rootless window, native Wayland coexistence, reconnect and teardown."""
import os,re,socket,subprocess,tempfile,time
from pathlib import Path
from smoke import ROOT,Viewer
def wait(check):
    end=time.monotonic()+15
    while time.monotonic()<end:
        value=check()
        if value:return value
        time.sleep(.03)
    raise AssertionError('X11 GLX smoke timeout')
class DesktopViewer(Viewer):
    def __init__(self,port,token):
        self.sock=socket.create_connection(("127.0.0.1",port),timeout=8);self.geometry={}
        self.send(type="hello",version=3,token=token,dpi=96,window_semantics_v1=True,cursor_v1=True,window_icon_v1=True,pointer_lock=True)
        hello=self.receive()[1]
        assert hello["window_semantics_v1"] and hello["cursor_v1"] and hello["window_icon_v1"]

def main():
    subprocess.run(['bash','tests/build-fixture.sh'],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-x11-') as runtime,open(ROOT/'build/tests/x11-glx.log','w') as log:
        env={**os.environ,'XDG_RUNTIME_DIR':runtime,'WAYLAND_DISPLAY':'x11-glx-0'}
        token='x11-glx-'+40*'e';path=Path(runtime)/'token';path.write_text(token);path.chmod(0o600)
        with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
        host=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--xwayland','--socket','x11-glx-0','--listen',f'127.0.0.1:{port}','--token-file',str(path),'--','glxgears','-geometry','400x300'],env=env,stdout=log,stderr=log)
        native=None;viewer=None;socket_file=None
        try:
            number=int(wait(lambda:re.search(r'private rootless Xwayland ready display=":(\d+)"',Path(log.name).read_text()))[1])
            assert 1000<=number<4096
            socket_file=Path(f'/tmp/.X11-unix/X{number}');assert socket_file.stat().st_mode&0o077==0
            viewer=DesktopViewer(port,token)
            info=viewer.until(lambda k,v:k=='control' and v.get('type')=='window' and v.get('title')=='glxgears')
            id=info['id'];assert info['x11'] is True and info['decoration']=='server'
            images=[viewer.until(lambda k,v:k=='frame' and v[0]==id)[-1] for _ in range(4)]
            assert len({im.tobytes() for im in images})>1
            assert sum(images[-1].convert('RGB').getextrema()[n][1] for n in range(3))>500
            assert info['min_size']==[0,0] and info['max_size']==[0,0] and not info['modal']
            viewer.send(type='resize',id=id,width=600,height=400,dpi=96,fullscreen=True,fullscreen_epoch=info['fullscreen_epoch'])
            fullscreen=viewer.until(lambda k,v:k=='control' and v.get('type')=='window' and v['id']==id and v.get('fullscreen'))
            viewer.until(lambda k,v:k=='frame' and v[0]==id and v[1:3]==(600,400))
            viewer.send(type='window_state',id=id,visible=False,minimized=True,mapping_epoch=fullscreen['mapping_epoch'])
            viewer.send(type='ping',nonce='x11-state');viewer.until(lambda k,v:k=='control' and v.get('nonce')=='x11-state')
            viewer.send(type='window_state',id=id,visible=True,minimized=False,mapping_epoch=fullscreen['mapping_epoch'])
            viewer.send(type='resize',id=id,width=600,height=400,dpi=96,fullscreen=False,fullscreen_epoch=fullscreen['fullscreen_epoch'])
            viewer.until(lambda k,v:k=='control' and v.get('type')=='window' and v['id']==id and not v.get('fullscreen'))
            viewer.sock.close();time.sleep(.15)
            viewer=Viewer(port,token)
            viewer.until(lambda k,v:k=='frame' and v[0]==id and v[1:3]==(600,400))
            native=subprocess.Popen([str(ROOT/'build/tests/test-app')],env=env,stdout=log,stderr=log)
            nid=viewer.until(lambda k,v:k=='control' and v.get('type')=='window' and not v.get('x11'))['id']
            viewer.until(lambda k,v:k=='frame' and v[0]==nid)
            viewer.send(type='close',id=id)
            viewer.until(lambda k,v:k=='control' and v.get('type')=='window_closed' and v.get('id')==id)
            viewer.send(type='refresh')
            viewer.until(lambda k,v:k=='frame' and v[0]==nid)
            print('PASS: real accelerated GLX animated rootless window, GPU import/capture, native decoration, negotiated cursor/icons/semantics, 600x400 resize/fullscreen/suspension with native Wayland focus coexistence, persistent reconnect, independent native Wayland window survives X11 close, private 0700 filesystem socket')
        finally:
            if viewer:viewer.sock.close()
            if native and native.poll() is None:native.terminate();native.wait(timeout=5)
            host.terminate();host.wait(timeout=10)
            if socket_file:wait(lambda:not socket_file.exists())
if __name__=='__main__':main()
