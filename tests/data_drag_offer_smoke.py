#!/usr/bin/env python3
"""Inactive-target negotiation and genuinely delayed old-source receive/finish."""
import base64
import os
from pathlib import Path
import queue
import socket
import subprocess
import tempfile
import threading
from smoke import ROOT, Viewer
from drag_out_smoke import wait_for


def main():
    subprocess.run(['bash','tests/build-fixture.sh'],cwd=ROOT,check=True)
    executable=ROOT/'build/tests/delayed-drag-offer'
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client'],text=True).split()
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Ibuild/tests','tests/delayed_drag_offer.c',
                    'build/tests/xdg-shell-protocol.c','-o',str(executable),*flags],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-delayed-offer-') as directory:
        root=Path(directory);token=root/'token';token.write_text('delayed-offer-'+40*'7');token.chmod(0o600)
        env={**os.environ,'XDG_RUNTIME_DIR':directory,'WAYLAND_DISPLAY':'delayed-offer-0','GVFS_DISABLE_FUSE':'1'}
        with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
        with (root/'host.log').open('w+') as log:
            host=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--socket','delayed-offer-0','--listen',f'127.0.0.1:{port}',
                '--token-file',str(token),'--codec','png','--no-audio','--no-xwayland'],env=env,stdout=log,stderr=log)
            app=viewer=None
            try:
                wait_for(lambda:(root/'delayed-offer-0').exists())
                viewer=Viewer(port,token.read_text(),file_drop_out=True,data_drag_v1=True)
                app=subprocess.Popen([str(executable)],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,text=True)
                lines=queue.Queue();observed=[]
                def collect():
                    for line in app.stdout:
                        observed.append(line.rstrip('\n'));lines.put(observed[-1])
                    lines.put(None)
                threading.Thread(target=collect,daemon=True).start()
                def line(prefix):
                    while True:
                        value=lines.get(timeout=8)
                        assert value is not None,('fixture exited',app.poll())
                        if value.startswith(prefix):return value
                line('READY')
                ids={}
                for _ in range(2):
                    info=viewer.until(lambda k,v:k=='control' and v.get('type')=='window' and v.get('title','').startswith('Delayed offer') and v['title'] not in ids)
                    ids[info['title']]=info['id']
                first,second=ids['Delayed offer A'],ids['Delayed offer B']
                def status(drag,accepted,epoch=None):
                    viewer.until(lambda k,v:k=='control' and v.get('type')=='text_drag_status' and v.get('drag')==drag and v.get('accepted')==accepted and (epoch is None or v.get('target_epoch')==epoch))
                def command(value):
                    app.stdin.write(value+'\n');app.stdin.flush()
                    return line('ACK '+value)
                def enter(drag,payload):
                    viewer.send(type='text_drag_enter',drag=drag,id=first,x=20,y=20,data=base64.b64encode(payload).decode())
                    status(drag,True)
                def unchanged(drag,accepted):
                    viewer.send(type='ping',nonce='negotiation')
                    while True:
                        kind,value=viewer.receive()
                        if kind=='control' and value.get('type')=='text_drag_status' and value.get('drag')==drag:
                            assert value['accepted']==accepted,('inactive offer changed current target',value)
                        if kind=='control' and value.get('type')=='pong' and value.get('nonce')=='negotiation':break
                enter(100,b'negotiation payload');line('ENTER 1 1')
                viewer.send(type='text_drag_motion',drag=100,id=second,x=20,y=20)
                line('ENTER 2 2');status(100,False)
                command('accept 1');unchanged(100,False)
                command('accept 2');status(100,True)
                command('noactions 1');unchanged(100,True)
                viewer.send(type='text_drag_cancel',drag=100)
                viewer.send(type='ping',nonce='cancel');viewer.until(lambda k,v:k=='control' and v.get('nonce')=='cancel')
                old='旧的 payload 🚀'.encode();new='新的 payload ✓'.encode()
                enter(200,old);line('ENTER 3 1')
                viewer.send(type='text_drag_drop',drag=200,id=first,x=20,y=20);line('DROP 3')
                enter(201,new);line('ENTER 4 1')
                app.stdin.write('receive 3\n');app.stdin.flush()
                assert line('READ 3 ')=='READ 3 '+old.hex()
                line('ACK receive 3')
                command('finish 3')
                # Old finish must not clear the new gesture/source or its data.
                viewer.send(type='text_drag_drop',drag=201,id=first,x=20,y=20);line('DROP 4')
                app.stdin.write('receive 4\n');app.stdin.flush()
                assert line('READ 4 ')=='READ 4 '+new.hex()
                line('ACK receive 4');command('finish 4')
                # Target revision changes invalidate the old native offer while
                # retaining one gesture and its immutable representation.
                viewer.send(type='text_drag_enter',drag=300,target_epoch=10,id=first,x=20,y=20,data=base64.b64encode(new).decode())
                line('ENTER 5 1');status(300,True,10)
                viewer.send(type='text_drag_retarget',drag=300,target_epoch=11,id=second,x=20,y=20)
                line('ENTER 6 2');status(300,False,11)
                command('accept 5');unchanged(300,False)
                command('accept 6');status(300,True,11)
                viewer.send(type='text_drag_motion',drag=300,target_epoch=10,id=first,x=20,y=20)
                unchanged(300,True)
                viewer.send(type='text_drag_drop',drag=300,target_epoch=11,id=second,x=20,y=20)
                line('DROP 6')
                viewer.send(type='text_drag_drop',drag=300,target_epoch=11,id=second,x=20,y=20)
                viewer.send(type='ping',nonce='duplicate-drop')
                viewer.until(lambda k,v:k=='control' and v.get('nonce')=='duplicate-drop')
                app.stdin.write('receive 6\n');app.stdin.flush()
                assert line('READ 6 ')=='READ 6 '+new.hex()
                line('ACK receive 6');command('finish 6')
                viewer.until(lambda k,v:k=='control' and v.get('type')=='text_drag_finished' and v.get('drag')==300 and v.get('target_epoch')==11 and v.get('success') is True)
                assert observed.count('DROP 6')==1,'duplicate drop redelivered the gesture'
                assert host.poll() is None and app.poll() is None
                print('PASS: inactive offer Accept/SetActions cannot alter new target; delayed old Receive/Finish preserves snapshots and new gesture; retarget keeps payload, ignores stale revision and acknowledges actual Finish')
            except BaseException:
                print((root/'host.log').read_text());raise
            finally:
                if viewer:viewer.sock.close()
                if app and app.poll() is None:app.terminate();app.wait(timeout=5)
                host.terminate();host.wait(timeout=10)


if __name__=='__main__':main()
