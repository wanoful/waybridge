#!/usr/bin/env python3
"""Actual Ghostty GTK context menu outside the terminal's unchanged client geometry."""
import os,signal,socket,subprocess,tempfile,time,sys
from pathlib import Path
from smoke import ROOT,Viewer

def main():
    ime='--ime' in sys.argv
    with tempfile.TemporaryDirectory(prefix='waybridge-ghostty-') as runtime,open(ROOT/'build/tests/ghostty-host.log','w') as log,open(ROOT/'build/tests/ghostty-app.log','w') as app_log:
        env={**os.environ,'XDG_RUNTIME_DIR':runtime,'WAYLAND_DISPLAY':'ghostty-0','GDK_BACKEND':'wayland','XDG_CONFIG_HOME':str(Path(runtime)/'config'),'XDG_CACHE_HOME':str(Path(runtime)/'cache'),'XDG_DATA_HOME':str(Path(runtime)/'data')}
        if ime:
            env.pop('GTK_IM_MODULE',None)
            env['WAYLAND_DEBUG']='1'
        with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
        token='ghostty-test-'+40*'8';token_path=Path(runtime)/'token';token_path.write_text(token);token_path.chmod(0o600)
        host=subprocess.Popen([os.environ.get('WAYBRIDGE_BINARY',str(ROOT/'target/debug/waybridge')),'--listen',f'127.0.0.1:{port}','--socket','ghostty-0','--token-file',str(token_path),'--codec','png'],env=env,stdout=log,stderr=log)
        app=None
        try:
            end=time.monotonic()+10
            while not (Path(runtime)/'ghostty-0').exists():
                assert host.poll() is None and time.monotonic()<end;time.sleep(.05)
            viewer=Viewer(port,token,dpi=168)
            states=[];original_receive=viewer.receive
            def receive():
                k,v=original_receive()
                if k=='control' and v['type']=='ime_state':states.append(v)
                return k,v
            viewer.receive=receive
            received=Path(runtime)/'received'
            command=['cat']
            if ime:
                reader='import os,sys,tty;tty.setraw(0);open(sys.argv[1],"wb").close()\nwhile True:\n data=os.read(0,4096)\n with open(sys.argv[1],"ab") as f:f.write(data)\n'
                command=['python','-c',reader,str(received)]
            app=subprocess.Popen(['dbus-run-session','--','ghostty','--gtk-single-instance=false','--config-default-files=false','--confirm-close-surface=false','--title=Waybridge Ghostty','-e',*command],env=env,stdout=app_log,stderr=app_log,start_new_session=True)
            info=viewer.until(lambda k,v:k=='control' and v['type']=='window' and 'ghostty' in v['app_id'].lower())
            id=info['id'];first=viewer.until(lambda k,v:k=='frame' and v[0]==id)
            first[-1].save(ROOT/'build/tests/ghostty-175.png')
            geometry=viewer.geometry[id][:2]
            viewer.send(type='focus',id=id,focused=True)
            if ime:
                viewer.send(type='pointer',id=id,x=100,y=100)
                for down in (True,False):viewer.send(type='button',id=id,button=272,down=down,x=100,y=100)
                end=time.monotonic()+12
                while time.monotonic()<end:
                    if received.exists() and states and states[-1].get('enabled'):break
                    viewer.receive()
                assert received.exists() and states[-1].get('enabled'),states
                state=states[-1];assert state['id']==id and state['rectangle'][3]>0
                viewer.send(type='ime',id=id,generation=state['generation'],preedit='nihao',cursor_begin=5,cursor_end=5)
                viewer.send(type='ime',id=id,generation=state['generation'],preedit='',commit='你好🙂')
                viewer.send(type='ime',id=id,generation=state['generation'],preedit='zhong',cursor_begin=5,cursor_end=5)
                viewer.send(type='ime',id=id,generation=state['generation'],preedit='')
                viewer.send(type='ime',id=id,generation=state['generation'],preedit='',commit='中文')
                end=time.monotonic()+10
                while time.monotonic()<end and received.read_bytes()!='你好🙂中文'.encode():time.sleep(.02)
                assert received.read_bytes()=='你好🙂中文'.encode(),received.read_bytes()
                for code in (14,28):
                    for down in (True,False):viewer.send(type='key',id=id,key=code,down=down)
                expected='你好🙂中文'.encode()+b'\x7f\r'
                end=time.monotonic()+5
                while time.monotonic()<end and received.read_bytes()!=expected:time.sleep(.02)
                assert received.read_bytes()==expected,received.read_bytes()
                print('PASS: actual Ghostty GTK text-input-v3 at 175%, Chinese/emoji commit once, preedit/cancel never reach PTY; Backspace and Return arrive immediately after commit without refocusing')
                return
            viewer.send(type='pointer',id=id,x=geometry[0]-25,y=geometry[1]-25)
            viewer.send(type='button',id=id,button=273,down=True)
            viewer.send(type='button',id=id,button=273,down=False)
            end=time.monotonic()+12;popup=None
            while time.monotonic()<end:
                k,v=viewer.receive()
                if k=='control' and v['type']=='window' and v['id']==id:
                    assert (v['width'],v['height'])==geometry,'Ghostty parent must not expand for the menu'
                if k=='control' and v['type']=='popup':popup=v;break
            assert popup and popup['root']==id
            assert popup['x']+popup['width']>geometry[0] or popup['y']+popup['height']>geometry[1],popup
            menu=viewer.until(lambda k,v:k=='frame' and v[0]==popup['id'])
            menu[-1].save(ROOT/'build/tests/ghostty-menu.png')
            assert viewer.geometry[id][:2]==geometry
            viewer.send(type='key',id=id,key=1,down=True);viewer.send(type='key',id=id,key=1,down=False)
            viewer.until(lambda k,v:k=='control' and v['type']=='window_closed' and v['id']==popup['id'])
            viewer.sock.close()
            print('PASS: actual Ghostty at 175% creates an independent overflowing GTK menu; terminal geometry stays unchanged; Escape dismisses the popup')
        finally:
            if app and app.poll() is None:
                os.killpg(app.pid,signal.SIGTERM)
                try:app.wait(timeout=5)
                except subprocess.TimeoutExpired:os.killpg(app.pid,signal.SIGKILL);app.wait()
            host.terminate();host.wait(timeout=5)
if __name__=='__main__':main()
