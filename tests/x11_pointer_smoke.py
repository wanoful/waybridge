#!/usr/bin/env python3
"""Real GLFW X11 client pixels after resize/DPI and game pointer-grab teardown."""
import os,re,socket,subprocess,sys,tempfile,time
from pathlib import Path
from smoke import ROOT,Viewer

class LockedViewer(Viewer):
    def __init__(self,*args,**kwargs):
        self.lock_active=False
        self.actions=[]
        self.infos={}
        port,token=args
        self.sock=socket.create_connection(('127.0.0.1',port),timeout=10)
        self.geometry={}
        self.send(type='hello',version=3,token=token,dpi=kwargs.get('dpi',96),window_semantics_v1=True,pointer_lock=True)
        assert self.receive()[1]['type']=='hello'
    def receive(self):
        packet=super().receive()
        kind,value=packet
        if kind=='control' and value.get('type')=='window':self.infos[value['id']]=value
        if kind=='control' and value.get('type')=='window_action':self.actions.append(value)
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
    fractional = '--fractional-screen' in sys.argv
    bounded = '--bounded-screen' in sys.argv or fractional
    screen_dpi = 168 if fractional else 120
    screen_width,screen_height,work_height = (2560,1600,1488) if fractional else (1920,1200,1080)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','glfw3','gl','x11'],text=True).split()
    subprocess.run(['cc','tests/x11_pointer_app.c','-o','build/tests/x11-pointer-app',*flags],cwd=ROOT,check=True)
    subprocess.run(['bash','tests/build-fixture.sh'],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-x11-pointer-') as directory,open(ROOT/'build/tests/x11-pointer-host.log','w') as log,open(ROOT/'build/tests/x11-pointer-app.log','w') as app_log:
        env={**os.environ,'XDG_RUNTIME_DIR':directory,'WAYBRIDGE_TEST_CSD':'1'}
        token='pointer-'+40*'a';path=Path(directory)/'token';path.write_text(token);path.chmod(0o600)
        with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
        host=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--socket','pointer-0','--listen',f'127.0.0.1:{port}','--token-file',str(path),'--codec','png','--no-audio'],env=env,stdout=log,stderr=log)
        app=viewer=native=None
        try:
            wait(lambda:(Path(directory)/'pointer-0.session.json').exists())
            viewer=LockedViewer(port,token,dpi=screen_dpi if bounded else 168)
            viewer.send(type='hello',dpi=screen_dpi if bounded else 168,pointer_lock=True,
                        **(dict(work_width=screen_width,work_height=work_height,monitor_width=screen_width,monitor_height=screen_height) if bounded else {}))
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
            # JavaFX and Wine CSDs can use XMoveWindow rather than EWMH move.
            # A held same-window gesture hands off to native movement once.
            before_press=Path(app_log.name).read_text().count('CLICK ')
            viewer.send(type='pointer',id=launcher,x=250,y=20)
            viewer.send(type='button',id=launcher,button=272,down=True,x=250,y=20)
            wait(lambda:Path(app_log.name).read_text().count('CLICK ')>before_press)
            viewer.send(type='pointer',id=launcher,x=258,y=24)
            move=viewer.until(lambda k,v:k=='control' and v.get('type')=='window_action' and v.get('action')=='move')
            assert move['id']==launcher,move
            viewer.send(type='button',id=launcher,button=272,down=False,x=258,y=24)
            if bounded:
                before=len(viewer.actions)
                app.stdin.write(b'o');app.stdin.flush();wait(lambda:'OFFSCREEN_REQUEST' in Path(app_log.name).read_text());time.sleep(.15)
                viewer.send(type='ping',nonce='no-gesture');viewer.until(lambda k,v:k=='control' and v.get('type')=='pong')
                assert len(viewer.actions)==before,'application placement without a held gesture must not move the native window'
                lw,lh=viewer.geometry[launcher][:2]
                for x,y in [(20,20),(lw/2,lh/2),(lw-30,lh-30)]:click(launcher,initial,x,y,'Waybridge GLFW launcher')
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
            if bounded:
                def metadata(fullscreen):
                    return viewer.until(lambda k,v:k=='control' and v.get('type')=='window' and v.get('id')==game and v.get('fullscreen') is fullscreen)
                # Cyberpunk-style borderless mode has no EWMH fullscreen bit.
                # A normally decorated screen-sized window must stay normal.
                app.stdin.write(b'd');app.stdin.flush();metadata(False)
                app.stdin.write(b'b');app.stdin.flush();borderless=metadata(True)
                logical_width=int(screen_width*96/screen_dpi+.5)
                logical_height=int(screen_height*96/screen_dpi+.5)
                viewer.send(type='resize',id=game,width=logical_width,height=logical_height,dpi=screen_dpi,
                            fullscreen=True,fullscreen_epoch=borderless['fullscreen_epoch'])
                current=frame(game,int(logical_width*screen_dpi/96+.5),int(logical_height*screen_dpi/96+.5))
                app.stdin.write(b's');app.stdin.flush();wait(lambda:'EWMH_FULLSCREEN 0' in Path(app_log.name).read_text())
                for x,y in [(25,30),(logical_width-36,logical_height-30)]:click(game,current,x,y,'Waybridge GLFW game')
                app.stdin.write(b'n');app.stdin.flush();normal=metadata(False)
                assert normal['fullscreen_epoch']>borderless['fullscreen_epoch']
                for command in (b'm',b'f'):
                    app.stdin.write(command);app.stdin.flush()
                    request=viewer.until(lambda k,v:k=='control' and v.get('type')=='window_action' and v.get('id')==game)
                    logical_width=int(screen_width*96/screen_dpi+.5)
                    logical_height=int((screen_height if command==b'f' else work_height)*96/screen_dpi+.5)
                    viewer.send(type='resize',id=game,width=logical_width,height=logical_height,dpi=screen_dpi,
                                fullscreen=command==b'f',fullscreen_epoch=request.get('fullscreen_epoch',normal['fullscreen_epoch']),maximized=command==b'm')
                    current=frame(game,int(logical_width*screen_dpi/96+.5),int(logical_height*screen_dpi/96+.5))
                    for x,y in [(25,30),(logical_width/2,400),(logical_width-36,logical_height-30)]:click(game,current,x,y,'Waybridge GLFW game')
                    assert topology()==original,'maximizing/fullscreen must not resize the shared monitor'
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
            print('PASS: X11 ConfigureRequest CSD gesture hands off native movement without leaking independent root placement')
            if bounded:
                print(f'PASS: negative placement, {screen_width}x{screen_height} monitor vs {screen_width}x{work_height} work area at DPI {screen_dpi}, borderless fullscreen without EWMH, decorated/normal restore, maximized/fullscreen edge coordinates and stable RandR topology')
        finally:
            if native and native.poll() is None:native.terminate();native.wait(timeout=5)
            if app and app.poll() is None:app.terminate();app.wait(timeout=5)
            if viewer:viewer.sock.close()
            host.terminate();host.wait(timeout=8)

if __name__=='__main__':main()
