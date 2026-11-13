#!/usr/bin/env python3
"""Real Qt text-input-v3: preedit/commit, Unicode and stale-focus rejection."""
import os,socket,subprocess,tempfile,time
from pathlib import Path
from smoke import ROOT,Viewer

def main():
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','Qt6Widgets'],text=True).split()
    subprocess.run(['c++','-std=c++17','-fPIC','-no-pie','tests/qt_app.cpp','-o','build/tests/qt-app',*flags],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-ime-') as runtime:
        env={**os.environ,'XDG_RUNTIME_DIR':runtime,'WAYLAND_DISPLAY':'ime-0','QT_QPA_PLATFORM':'wayland','WAYLAND_DEBUG':'1'}
        for name in ('QT_IM_MODULE','GTK_IM_MODULE','QT_SCALE_FACTOR','QT_SCREEN_SCALE_FACTORS'):env.pop(name,None)
        with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
        token='ime-test-'+40*'9';token_file=Path(runtime)/'token';token_file.write_text(token);token_file.chmod(0o600)
        with open(ROOT/'build/tests/ime-host.log','w') as hostlog,open(ROOT/'build/tests/ime-app.log','w') as applog:
            host=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--socket','ime-0','--listen',f'127.0.0.1:{port}','--token-file',str(token_file)],env=env,stdout=hostlog,stderr=hostlog)
            app=None
            try:
                end=time.monotonic()+10
                while not (Path(runtime)/'ime-0').exists():
                    assert host.poll() is None and time.monotonic()<end;time.sleep(.02)
                viewer=Viewer(port,token,dpi=168)
                app=subprocess.Popen([str(ROOT/'build/tests/qt-app')],env=env,stdout=applog,stderr=applog)
                infos={};states=[]
                def until(check):
                    end=time.monotonic()+12
                    while time.monotonic()<end:
                        result=check()
                        if result:return result
                        kind,value=viewer.receive()
                        if kind=='control':
                            if value['type']=='window':infos[value['id']]=value
                            if value['type']=='ime_state':states.append(value)
                    raise AssertionError('IME condition timed out; see ime-app.log')
                window=until(lambda:next((id for id,v in infos.items() if v['title']=='Waybridge Qt fixture'),None))
                viewer.send(type='focus',id=window,focused=True)
                viewer.send(type='pointer',id=window,x=80,y=40)
                for down in (True,False):viewer.send(type='button',id=window,button=272,down=down,x=80,y=40)
                state=until(lambda:next((s for s in reversed(states) if s.get('enabled') and s['id']==window),None))
                assert state['rectangle'][3]>0
                def text():return (ROOT/'build/tests/ime-app.log').read_text()
                viewer.send(type='ime',id=window,generation=state['generation'],preedit='nihao',cursor_begin=5,cursor_end=5)
                until(lambda:'PREEDIT nihao\n' in text())
                assert 'TEXT nihao\n' not in text(),'preedit must not commit'
                viewer.send(type='ime',id=window,generation=state['generation'],preedit='',commit='你好🙂')
                until(lambda:'TEXT 你好🙂\n' in text())
                # UTF-8 cursor offsets cannot split a multi-byte codepoint.
                viewer.send(type='ime',id=window,generation=state['generation'],preedit='中',cursor_begin=1,cursor_end=1)
                viewer.send(type='ime',id=window,generation=state['generation'],preedit='',commit='！')
                until(lambda:'TEXT 你好🙂！\n' in text())
                assert 'PREEDIT 中\n' not in text()
                viewer.send(type='ime',id=window,generation=state['generation'],preedit='cancel',cursor_begin=6,cursor_end=6)
                until(lambda:'PREEDIT cancel\n' in text())
                viewer.send(type='focus',id=window,focused=False)
                until(lambda:states and not states[-1]['enabled'])
                viewer.send(type='ime',id=window,generation=state['generation'],commit='STALE')
                viewer.send(type='focus',id=window,focused=True)
                newer=until(lambda:states[-1] if states[-1].get('enabled') and states[-1]['generation']!=state['generation'] else None)
                viewer.send(type='ime',id=window,generation=state['generation'],commit='STALE')
                viewer.send(type='ime',id=window,generation=newer['generation'],preedit='',commit='新')
                until(lambda:'TEXT 你好🙂！新\n' in text())
                assert 'STALE' not in text()
                viewer.sock.close()
                print('PASS: real Qt 175% text-input-v3 preedit, Chinese/emoji commits, caret rectangle, invalid UTF-8 cursor and stale focus generation rejected')
            finally:
                if app:app.terminate();app.wait(timeout=5)
                host.terminate();host.wait(timeout=5)

if __name__=='__main__':main()
