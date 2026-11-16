#!/usr/bin/python3
"""Real native worker and private host IPC under Wine, against a mock peer.
The Windows App SDK toast layer is separately tested on real Windows.
"""
import base64
import io
import json
import os
from pathlib import Path
import queue
import socket
import struct
import subprocess
import tempfile
import threading
import time
from PIL import Image

ROOT=Path(__file__).resolve().parents[1]

def main():
    (ROOT/'build/tests').mkdir(parents=True,exist_ok=True)
    probe=ROOT/'build/tests/windows-control-probe.exe'
    subprocess.run(['x86_64-w64-mingw32-g++','-municode','-static','-O2','tests/windows_control_probe.cpp','-o',str(probe)],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='wb-notify-native-') as directory, socket.socket() as listener:
        root=Path(directory)
        log=(root/'wine.log').open('w+')
        readfd,writefd=os.pipe()
        xvfb=subprocess.Popen(['Xvfb','-displayfd',str(writefd),'-screen','0','1280x1024x24','-nolisten','tcp'],pass_fds=[writefd],stdout=log,stderr=log)
        os.close(writefd)
        with os.fdopen(readfd) as pipe: display=':'+pipe.readline().strip()
        env={**os.environ,'DISPLAY':display,'WINEPREFIX':str(root/'wine'),'WINEDEBUG':'-all','WINEDLLOVERRIDES':'winemenubuilder.exe=d;mscoree,mshtml=','WAYBRIDGE_GDI':'1'}
        native=None
        try:
            subprocess.run(['wineboot','-u'],env=env,stdout=log,stderr=log,check=True,timeout=60)
            listener.bind(('127.0.0.1',0));listener.listen();listener.settimeout(60)
            native=subprocess.Popen(['wine',str(probe),'Z:'+str(ROOT/'build/windows/waybridge-client.exe'),'Z:'+str(root/'worker.log')],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,text=True)
            events=queue.Queue()
            def read():
                for line in native.stdout: events.put(json.loads(line))
            threading.Thread(target=read,daemon=True).start()
            def command(**value): native.stdin.write(json.dumps(value)+'\n');native.stdin.flush()
            def until(kind):
                deadline=time.monotonic()+15
                while time.monotonic()<deadline:
                    item=events.get(timeout=15)
                    if item['type']==kind: return item
                raise AssertionError('missing IPC event '+kind)
            settings=dict(codec='png',audio=False,notifications=True)
            command(type='start',version=1,host='127.0.0.1',port=listener.getsockname()[1],token='fixture-secret-0123456789abcdef',settings=settings)
            connection,_=listener.accept();connection.settimeout(10)
            def exact(size):
                result=b''
                while len(result)<size:
                    part=connection.recv(size-len(result));assert part;result+=part
                return result
            def receive():
                size=struct.unpack('>I',exact(4))[0];packet=exact(size);assert packet[0]==0;return json.loads(packet[1:])
            def server(**value):
                payload=b'\0'+json.dumps(value).encode();connection.sendall(struct.pack('>I',len(payload))+payload)
            def incoming(kind):
                for _ in range(20):
                    item=receive()
                    if item['type']==kind: return item
                raise AssertionError('missing server feedback '+kind)
            hello=receive();assert hello['notifications_v1'] and hello['notifications_enabled']
            server(type='hello',version=3,codec='mixed',notifications_v1=True,features=['notifications_v1'])
            until('connected');assert incoming('notifications_enabled')['enabled']
            record=dict(type='notification',instance='a'*32,binding='b'*32,id=17,revision=1,app_name='测试应用',summary='中文标题',body='只存在于内存的内容',timeout_ms=10000,actions=[dict(key='default',label='打开')],png=None)
            server(**record)
            received=until('notification');assert received['summary']==record['summary']
            icon=Image.open(io.BytesIO(base64.b64decode(received['png']))).convert('RGBA')
            assert icon.size==(64,64) and icon.getbbox(), 'native worker must supply badged placeholder icon'
            command(**{**record,'type':'notification_feedback','action':'default'})
            action=incoming('notification_feedback');assert action['id']==17 and action['revision']==1 and action['action']=='default'
            record['revision']=2;server(**record);until('notification')
            # Old action cannot cross a replacement; the only feedback below is
            # from the current revision, even with an otherwise matching token.
            command(**{**record,'type':'notification_feedback','revision':1,'action':'default'})
            command(**{**record,'type':'notification_feedback','action':'default'})
            assert incoming('notification_feedback')['revision']==2
            settings['notifications']=False;command(type='settings',settings=settings)
            assert incoming('notifications_enabled')['enabled'] is False
            command(type='disconnect');connection.shutdown(socket.SHUT_RDWR);connection.close()
            native.stdin.close();native.wait(timeout=15);assert native.returncode==0, native.returncode
            assert '只存在于内存的内容' not in (root/'worker.log').read_text()
            print('PASS: real PE notifications_v1 negotiation, native badged placeholder PNG, private IPC notification/action round-trip, stale replacement rejection, live opt-out, no body logging')
        except BaseException:
            print('Native worker failure:', (root/'worker.log').read_text() if (root/'worker.log').exists() else 'no worker log')
            raise
        finally:
            if native and native.poll() is None: native.terminate();native.wait(timeout=8)
            subprocess.run(['wineserver','-k'],env=env,stdout=log,stderr=log,timeout=8)
            xvfb.terminate();xvfb.wait(timeout=5);log.close()

if __name__=='__main__': main()
