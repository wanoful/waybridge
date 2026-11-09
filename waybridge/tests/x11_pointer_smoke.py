#!/usr/bin/env python3
"""Real GLFW X11 client pixels after resize/DPI and game pointer-grab teardown."""
import os,re,socket,subprocess,tempfile,time
from pathlib import Path
from smoke import ROOT,Viewer

class LockedViewer(Viewer):
    def __init__(self,*args,**kwargs):
        self.lock_active=False
        super().__init__(*args,**kwargs)
    def receive(self):
        packet=super().receive()
        kind,value=packet
        if kind=='control' and value.get('type')=='pointer_lock':
            self.lock_active=value['active']
            if value['active']:
                self.send(type='pointer_lock_ack',id=value['id'],generation=value['generation'],active=True)
        return packet

def wait(check,seconds=12):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        value=check()
        if value:return value
        time.sleep(.02)
    raise AssertionError('X11 pointer fixture timed out')

def main():
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','glfw3','gl'],text=True).split()
    subprocess.run(['cc','tests/x11_pointer_app.c','-o','build/tests/x11-pointer-app',*flags],cwd=ROOT,check=True)
    subprocess.run(['bash','tests/build-fixture.sh'],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-x11-pointer-') as directory,open(ROOT/'build/tests/x11-pointer-host.log','w') as log,open(ROOT/'build/tests/x11-pointer-app.log','w') as app_log:
        env={**os.environ,'XDG_RUNTIME_DIR':directory}
        token='pointer-'+40*'a';path=Path(directory)/'token';path.write_text(token);path.chmod(0o600)
        with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
        host=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--socket','pointer-0','--listen',f'127.0.0.1:{port}','--token-file',str(path),'--codec','png','--no-audio'],env=env,stdout=log,stderr=log)
        app=viewer=native=None
        try:
            wait(lambda:(Path(directory)/'pointer-0.session.json').exists())
            viewer=LockedViewer(port,token,dpi=168)
            viewer.send(type='hello',dpi=168,pointer_lock=True)
            viewer.send(type='ping',nonce=123);viewer.until(lambda k,v:k=='control' and v.get('type')=='pong')
            app=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--socket','pointer-0','--launch','--',str(ROOT/'build/tests/x11-pointer-app')],env=env,stdin=subprocess.PIPE,stdout=app_log,stderr=app_log)
            def window(title):return viewer.until(lambda k,v:k=='control' and v.get('type')=='window' and v.get('title')==title)['id']
            launcher=window('Waybridge GLFW launcher')
            def topology():
                result=subprocess.check_output([str(ROOT/'target/debug/waybridge'),'--socket','pointer-0','--launch','--','xrandr','--listmonitors'],env=env,text=True,stderr=subprocess.DEVNULL)
                assert result.startswith('Monitors: 1\n'),result
                return result
            original=topology()
            def frame(id,width=None,height=None):
                return viewer.until(lambda k,v:k=='frame' and v[0]==id and (width is None or v[1:3]==(width,height)))
            def click(id,frame,x,y,name):
                before=len(re.findall(r'^CLICK ',Path(app_log.name).read_text(),re.M))
                viewer.send(type='focus',id=id,focused=True)
                viewer.send(type='pointer',id=id,x=x,y=y)
                for down in (True,False):viewer.send(type='button',id=id,button=272,down=down,x=x,y=y)
                text=wait(lambda:Path(app_log.name).read_text() if len(re.findall(r'^CLICK ',Path(app_log.name).read_text(),re.M))>before else None)
                match=re.findall(r'^CLICK '+name+r' ([\d.-]+) ([\d.-]+) (\d+) (\d+) (\d+) (\d+)',text,re.M)[-1]
                cx,cy,w,h,fw,fh=map(float,match)
                expected=(x*fw/viewer.geometry[id][0],y*fh/viewer.geometry[id][1])
                assert abs(cx-expected[0])<2 and abs(cy-expected[1])<2,(match,expected,viewer.geometry[id])
            initial=frame(launcher)
            click(launcher,initial,40,50,'Waybridge GLFW launcher')
            app.stdin.write(b'g');app.stdin.flush();game=window('Waybridge GLFW game');current=frame(game)
            assert topology()==original
            native=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--socket','pointer-0','--launch','--',str(ROOT/'build/tests/test-app')],env=env,stdout=app_log,stderr=app_log)
            time.sleep(.3)
            assert topology()==original,'Wayland-only per-window outputs leaked to Xwayland'
            for width,height,dpi in [(900,600,168),(500,700,168),(700,450,120),(960,640,168)]:
                viewer.send(type='resize',id=game,width=width,height=height,dpi=dpi)
                current=frame(game,int(width*dpi/96+.5),int(height*dpi/96+.5))
                for x,y in [(30,40),(width*.5,height*.5),(width-35,height-45)]:click(game,current,x,y,'Waybridge GLFW game')
                assert topology()==original,'X11 window resize/DPI must not change shared monitors'
            app.stdin.write(b'l');app.stdin.flush();wait(lambda:'LOCKED' in Path(app_log.name).read_text())
            viewer.until(lambda k,v:k=='control' and v.get('type')=='pointer_lock' and v.get('active'))
            viewer.send(type='ping',nonce=456);viewer.until(lambda k,v:k=='control' and v.get('type')=='pong')
            assert viewer.lock_active
            time.sleep(.2);app.stdin.write(b'c');app.stdin.flush()
            viewer.until(lambda k,v:k=='control' and v.get('type')=='window_closed' and v.get('id')==game)
            assert not viewer.lock_active,'closing the X11 game must release the negotiated viewer lock'
            viewer.send(type='focus',id=launcher,focused=True)
            for x,y in [(20,30),(180,140),(300,200)]:click(launcher,initial,x,y,'Waybridge GLFW launcher')
            assert topology()==original
            print('PASS: actual GLFW X11 cursor/button pixels across resize and 175%/125% DPI, launcher clicks restored after disabled-cursor game closes')
        finally:
            if native and native.poll() is None:native.terminate();native.wait(timeout=5)
            if app and app.poll() is None:app.terminate();app.wait(timeout=5)
            if viewer:viewer.sock.close()
            host.terminate();host.wait(timeout=8)

if __name__=='__main__':main()
