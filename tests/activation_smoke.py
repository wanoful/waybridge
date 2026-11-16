#!/usr/bin/python3
"""Real Wayland activation tokens, external launch context and connection expiry."""
import json,os,socket,subprocess,tempfile,time
from pathlib import Path
from smoke import Viewer,ROOT
from session_fixture import SessionFixture

class Client(Viewer):
    def __init__(self,port,token):
        self.sock=socket.create_connection(('127.0.0.1',port),timeout=10);self.geometry={}
        self.send(type='hello',version=3,token=token,activation_v1=True,window_semantics_v1=True)
        assert self.receive()[1]['activation_v1']
    def control(self,kind):return self.until(lambda k,v:k=='control' and v['type']==kind)
    def key(self,id,code):
        self.send(type='focus',id=id,focused=True)
        self.send(type='key',id=id,key=code,down=True);self.send(type='key',id=id,key=code,down=False)

def wait(check):
    deadline=time.monotonic()+10
    while time.monotonic()<deadline:
        if check():return
        time.sleep(.02)
    raise AssertionError('activation fixture timed out')

def main():
    subprocess.run(['bash','tests/build-fixture.sh'],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='wb-activate-') as d,SessionFixture(Path(d)/'fixture') as fixture:
        runtime=Path(fixture.env['XDG_RUNTIME_DIR']);token='focus-'+('a'*40)
        (runtime/'token').write_text(token);(runtime/'token').chmod(0o600)
        with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
        log=(Path(d)/'events.log').open('w+')
        host=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--socket','activate-fixture','--listen',f'127.0.0.1:{port}','--token-file',str(runtime/'token'),'--no-audio','--no-xwayland','--codec','png'],env=fixture.env,stdout=log,stderr=log)
        app=None;client=None
        try:
            wait(lambda:(runtime/'activate-fixture.session.json').exists())
            client=Client(port,token)
            app=subprocess.Popen([str(ROOT/'build/tests/test-app'),'two'],env={**fixture.env,'WAYLAND_DISPLAY':'activate-fixture','WAYBRIDGE_TEST_ACTIVATION':'1'},stdout=log,stderr=log)
            infos={}
            while len(infos)<2:
                k,v=client.receive()
                if k=='control' and v['type']=='window':infos[v['id']]=v
            a,b=sorted(infos)
            client.key(a,30)
            first=client.control('window_activate');assert first['id']==b and first['origin']==a and first['request_id'] is None
            client.key(a,46);time.sleep(.12)
            client.send(type='ping',nonce='replay')
            while True:
                k,v=client.receive()
                if k=='control':
                    assert v['type']!='window_activate','consumed Wayland token replayed'
                    if v['type']=='pong':break
            client.key(a,48);time.sleep(.12)
            client.send(type='ping',nonce='invalid-serial')
            while True:
                k,v=client.receive()
                if k=='control':
                    assert v['type']!='window_activate','invalid input serial authorized activation'
                    if v['type']=='pong':break
            request='1'*32
            client.send(type='activation_prepare',request_id=request,app='waybridge.fixture.desktop')
            prepared=client.control('activation_prepared');assert len(prepared['token'])==32
            client.send(type='activation_launch',request_id=request,pid=app.pid)
            explicit=client.control('window_activate');assert explicit['request_id']==request and explicit['id'] in (a,b)
            # Explicit metadata does not become focus until Windows confirms it.
            client.send(type='ping',nonce='focus-order');client.control('pong')
            client.send(type='activation_prepare',request_id='2'*32,app='waybridge.fixture.desktop')
            client.control('activation_prepared');client.sock.close();client=None;time.sleep(.1)
            client=Client(port,token)
            client.send(type='activation_launch',request_id='2'*32,pid=app.pid)
            client.send(type='ping',nonce='old-connection')
            while True:
                k,v=client.receive()
                if k=='control':
                    assert v['type']!='window_activate','old connection launch grant survived'
                    if v['type']=='pong':break
            print('PASS: actual xdg-activation token, recent serial, one-shot replay rejection, invalid serial refusal, external application/PID launch context, disconnected request invalidation')
        finally:
            if client:client.sock.close()
            if app:app.terminate();app.wait(timeout=5)
            host.terminate();host.wait(timeout=10);log.close()

if __name__=='__main__':main()
