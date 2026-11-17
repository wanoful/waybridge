#!/usr/bin/env python3
"""Actual wl_touch subscriptions, xdg requests/popups and touch-owned data-device DND."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from smoke import ROOT, Viewer
from touch_smoke import point, wait


class InteractionViewer(Viewer):
    def __init__(self, port, token):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=8)
        self.geometry = {}; self.state = {}; self.controls = []; self.windows = {}
        self.send(type='hello', version=3, token=token, dpi=96, window_semantics_v1=True,
                  touch_v1=True, touch_enabled=True, touch_interaction_v1=True,
                  file_drop_out=True, data_drag_v1=True, toplevel_drag_v1=True)
        hello = self.receive()[1]
        assert hello.get('touch_interaction_v1') is True, hello
    def receive(self):
        kind, value = super().receive()
        if kind == 'control':
            self.controls.append(value)
            if value.get('type') == 'touch_state': self.state = value
            if value.get('type') == 'window': self.windows[value['title']] = value
        return kind, value
    def control(self, message_type, **matches):
        return self.until(lambda k,v:k=='control' and v.get('type')==message_type and all(v.get(n)==x for n,x in matches.items()))
    def barrier(self):
        nonce=time.monotonic_ns();self.send(type='ping',nonce=nonce);self.control('pong',nonce=nonce);time.sleep(.08)
    def lease(self, window):
        self.send(type='focus', id=window, focused=True);self.barrier()
        if not (self.state.get('enabled') and self.state.get('id')==window):
            self.until(lambda k,v:k=='control' and v.get('type')=='touch_state' and v.get('enabled') and v.get('id')==window)
        return dict(self.state)
    def touch(self, lease, gesture, points, **extra):
        self.send(type='touch',id=lease['id'],generation=lease['generation'],gesture=gesture,points=points,**extra)
    def begin(self, lease, gesture, kind='dnd'):
        self.touch(lease,gesture,[point(0,x=20,y=25)])
        op=self.control('touch_interaction',kind=kind)
        assert (op['id'],op['generation'],op['gesture'],op['slot'])==(lease['id'],lease['generation'],gesture,0),op
        return op
    def ack(self, op, accepted=True):self.send(type='touch_interaction_ack',operation=op['operation'],accepted=accepted)
    def sample(self, op, phase='motion', target=0, x=30, y=35, **extra):
        data=dict(type='touch_interaction_input',operation=op['operation'],gesture=op['gesture'],slot=op['slot'],phase=phase,id=target,x=x,y=y)
        data.update(extra);self.send(**data)
    def ended(self, op, cancelled=False):
        return self.control('touch_interaction',operation=op['operation'],kind='end',cancelled=cancelled)


def main():
    subprocess.run(['bash','tests/build-touch-interaction-fixture.sh'],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-touch-interaction-') as runtime:
        root=Path(runtime);env={**os.environ,'XDG_RUNTIME_DIR':runtime,'WAYLAND_DISPLAY':'touch-interaction-0'}
        token='touch-interaction-'+'f'*40;secret=root/'token';secret.write_text(token);secret.chmod(0o600)
        with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
        logs=[];apps=[];viewer=None
        hostlog=open(ROOT/'build/tests/touch-interaction-host.log','w');logs.append(hostlog)
        host=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--codec','png','--no-audio','--no-xwayland','--listen',f'127.0.0.1:{port}','--socket','touch-interaction-0','--token-file',str(secret)],env=env,stdout=hostlog,stderr=hostlog)
        def start(role,mime,payload):
            path=ROOT/f'build/tests/touch-interaction-{role}-{len(apps)}.log';logfile=open(path,'w');logs.append(logfile)
            local={**env,'WAYBRIDGE_TOUCH_MIME':mime,'WAYBRIDGE_TEST_DRAG_URIS':str(payload),'WAYBRIDGE_TEST_DROP':str(root/'received')}
            if role=='receiver':local['WAYBRIDGE_TOUCH_RECEIVER']='1'
            app=subprocess.Popen([str(ROOT/'build/tests/touch-interaction-app')],stdin=subprocess.PIPE,text=True,env=local,stdout=logfile,stderr=logfile)
            apps.append(app)
            return app,lambda:path.read_text()
        def command(app,log,text):
            before=log().count('COMMAND\n');app.stdin.write(text+'\n');app.stdin.flush();wait(lambda:log().count('COMMAND\n')>before)
        try:
            wait(lambda:(root/'touch-interaction-0').exists() or host.poll() is not None)
            assert host.poll() is None,Path(hostlog.name).read_text()
            viewer=InteractionViewer(port,token)
            gesture=0
            exported=root/'native-touch-test.txt';exported.write_bytes(b'file touch payload')
            for mime,payload_bytes in [('text/uri-list',(exported.as_uri()+'\r\n').encode()),('text/plain;charset=utf-8','native touch text 你好'.encode())]:
                payload=root/'payload';payload.write_bytes(payload_bytes)
                source,slog=start('source',mime,payload);target,tlog=start('receiver',mime,payload)
                names=['Touch source 1','Touch source 2','Touch receiver 1','Touch receiver 2']
                # Fresh metadata after restarting fixture clients supersedes the previous ids.
                viewer.windows.clear()
                while not all(n in viewer.windows for n in names):viewer.receive()
                first=viewer.windows[names[0]]['id'];other=viewer.windows[names[1]]['id'];receiver=viewer.windows[names[2]]['id']
                lease=viewer.lease(first)
                if mime=='text/uri-list':
                    assert viewer.windows[names[0]]['touch_input'] is True
                    command(source,slog,'touch_off')
                    viewer.until(lambda k,v:k=='control' and v.get('type')=='window' and v.get('id')==first and v.get('touch_input') is False)
                    viewer.barrier();assert not viewer.state.get('enabled'),viewer.state
                    command(source,slog,'touch_on')
                    viewer.until(lambda k,v:k=='control' and v.get('type')=='window' and v.get('id')==first and v.get('touch_input') is True)
                    lease=viewer.lease(first)
                    # An implicit touch serial may operate only its owning toplevel.
                    command(source,slog,'invalid_move');gesture+=1;before=len(viewer.controls)
                    viewer.touch(lease,gesture,[point(0)]);wait(lambda:'TOUCH_DOWN 0 1' in slog());viewer.barrier()
                    assert not any(v.get('type')=='touch_interaction' for v in viewer.controls[before:])
                    viewer.touch(lease,gesture,[point(0,'up')]);viewer.barrier()
                    for action in ('move','resize'):
                        command(source,slog,action);lease=viewer.lease(first);gesture+=1;op=viewer.begin(lease,gesture,action)
                        if action=='resize':assert op['edge']==10,op
                        viewer.ack(op);viewer.barrier();assert viewer.state['generation']==lease['generation'] and viewer.state['enabled']
                        # A stale gesture or a secondary finger cannot finish the operation.
                        viewer.sample(op,'up',gesture=gesture-1);viewer.sample(op,'up',slot=1);viewer.barrier()
                        assert not any(v.get('kind')=='end' and v.get('operation')==op['operation'] for v in viewer.controls)
                        viewer.sample(op,'up');viewer.ended(op);viewer.barrier()
                        assert viewer.state['generation']>lease['generation']
                    # Owner UP before ACK rejects takeover; late ACK cannot resurrect it.
                    lease=viewer.lease(first);gesture+=1;op=viewer.begin(lease,gesture,'resize')
                    viewer.touch(lease,gesture,[point(0,'up')]);viewer.ended(op,True);viewer.ack(op);viewer.barrier()
                    assert viewer.state['generation']>lease['generation']
                    # A rejected ACK cancels the ordinary touch stream and takes a fresh lease.
                    lease=viewer.lease(first);gesture+=1;op=viewer.begin(lease,gesture,'resize');viewer.ack(op,False);viewer.ended(op,True)
                    viewer.barrier();assert viewer.state['generation']>lease['generation']
                    # Releasing the last subscription also cancels an active operation.
                    command(source,slog,'move');lease=viewer.lease(first);gesture+=1;op=viewer.begin(lease,gesture,'move');viewer.ack(op);viewer.barrier()
                    command(source,slog,'touch_off');viewer.ended(op,True)
                    viewer.until(lambda k,v:k=='control' and v.get('type')=='window' and v.get('id')==first and v.get('touch_input') is False)
                    command(source,slog,'touch_on')
                    viewer.until(lambda k,v:k=='control' and v.get('type')=='window' and v.get('id')==first and v.get('touch_input') is True)
                    # Popup leases remain rooted at the toplevel; hit coordinates target the popup.
                    command(source,slog,'menu');lease=viewer.lease(first);gesture+=1
                    viewer.touch(lease,gesture,[point(0,x=25,y=30)])
                    popup=viewer.control('popup',root=first)
                    viewer.touch(lease,gesture,[point(0,'up',25,30)])
                    wait(lambda:'TOUCH_UP 0' in slog());viewer.barrier()
                    assert viewer.state['enabled'] and viewer.state['generation']==lease['generation'],viewer.state
                    gesture+=1;viewer.touch(lease,gesture,[point(0,x=15,y=20)],target=popup['id'])
                    wait(lambda:'TOUCH_DOWN 0 3 15.000 20.000' in slog())
                    viewer.touch(lease,gesture,[point(0,'up',15,20)],target=popup['id']);viewer.barrier()
                    gesture+=1;viewer.touch(lease,gesture,[point(0,x=250,y=200)])
                    wait(lambda:'MENU_DONE' in slog());viewer.barrier()
                # Real data-device targets in a different client receive each supported payload.
                command(source,slog,'dnd');lease=viewer.lease(first);gesture+=1;op=viewer.begin(lease,gesture);viewer.ack(op)
                viewer.sample(op,target=receiver,x=40,y=45);wait(lambda:'DND_ENTER 1' in tlog())
                viewer.send(type='focus',id=receiver,focused=True);viewer.barrier()
                assert viewer.state['id']==first and viewer.state['generation']==lease['generation'],viewer.state
                viewer.sample(op,'up',target=receiver,x=40,y=45);viewer.ended(op)
                wait(lambda:(root/'received').exists() and (root/'received').read_bytes()==payload_bytes)
                wait(lambda:'SOURCE_FINISHED' in slog());assert 'SOURCE_CANCELLED' not in slog(),slog()
                (root/'received').unlink()
                if mime=='text/uri-list':
                    # Invalid coordinates on release must cancel, never drop on the last good target.
                    before_drop=tlog().count('DND_DROP');before_enter=tlog().count('DND_ENTER')
                    lease=viewer.lease(first);gesture+=1;op=viewer.begin(lease,gesture);viewer.ack(op)
                    viewer.sample(op,target=receiver,x=40,y=45);wait(lambda:tlog().count('DND_ENTER')>before_enter)
                    viewer.sample(op,'up',target=receiver,x=1e20);viewer.ended(op,True)
                    wait(lambda:'SOURCE_CANCELLED' in slog());assert tlog().count('DND_DROP')==before_drop
                    # Leaving every remote HWND preserves the live source for native Windows OLE.
                    lease=viewer.lease(first);gesture+=1;op=viewer.begin(lease,gesture);viewer.ack(op)
                    offer=viewer.control('drag_offer')
                    assert offer['input']=='touch' and offer['touch_operation']==op['operation'] and offer['touch_gesture']==gesture and offer['touch_slot']==0,offer
                    viewer.sample(op,target=0);viewer.barrier()
                    assert not any(v.get('kind')=='end' and v.get('operation')==op['operation'] for v in viewer.controls)
                    owner={k:offer[k] for k in ('touch_operation','touch_generation','touch_gesture','touch_slot')}
                    viewer.send(type='drag_claim',drag=offer['drag'],touch_operation=op['operation']+1)
                    viewer.barrier()
                    assert not any(v.get('type')=='drag_ready' and v.get('drag')==offer['drag'] for v in viewer.controls)
                    viewer.send(type='drag_claim',drag=offer['drag'],**owner);viewer.control('drag_ready',drag=offer['drag'])
                    viewer.send(type='drag_return',drag=offer['drag'],id=receiver,x=40,y=45,return_token=1,**owner)
                    returned=viewer.control('drag_return_ready',drag=offer['drag'],return_token=1);assert returned['accepted'],returned
                    viewer.sample(op,'up',target=receiver,x=40,y=45);viewer.ended(op)
                    wait(lambda:(root/'received').exists() and (root/'received').read_bytes()==payload_bytes)
                    (root/'received').unlink()
                    # Explicitly delay real target negotiation to cover every return/UP order.
                    for order in ('up-return-settle','return-up-settle','return-settle-up'):
                        command(target,tlog,'hold_feedback')
                        before_enter=tlog().count('DND_ENTER');before_drop=tlog().count('DND_DROP')
                        before_cancel=slog().count('SOURCE_CANCELLED')
                        lease=viewer.lease(first);gesture+=1;op=viewer.begin(lease,gesture);viewer.ack(op);offer=viewer.control('drag_offer')
                        viewer.send(type='drag_claim',drag=offer['drag']);viewer.control('drag_ready',drag=offer['drag'])
                        if order=='up-return-settle':viewer.sample(op,'up');viewer.barrier()
                        viewer.send(type='drag_return',drag=offer['drag'],id=receiver,x=40,y=45,return_token=1)
                        wait(lambda:tlog().count('DND_ENTER')>before_enter)
                        if order=='return-up-settle':viewer.sample(op,'up',target=receiver,x=40,y=45)
                        viewer.barrier()
                        assert not any(v.get('kind')=='end' and v.get('operation')==op['operation'] for v in viewer.controls),order
                        assert not any(v.get('type')=='drag_return_ready' and v.get('drag')==offer['drag'] for v in viewer.controls),order
                        assert tlog().count('DND_DROP')==before_drop and slog().count('SOURCE_CANCELLED')==before_cancel,order
                        command(target,tlog,'settle')
                        viewer.control('drag_return_ready',drag=offer['drag'],return_token=1,accepted=True)
                        if order=='return-settle-up':
                            viewer.barrier()
                            assert not any(v.get('kind')=='end' and v.get('operation')==op['operation'] for v in viewer.controls),order
                            viewer.sample(op,'up',target=receiver,x=40,y=45)
                        viewer.ended(op)
                        wait(lambda:(root/'received').exists() and (root/'received').read_bytes()==payload_bytes)
                        assert tlog().count('DND_DROP')==before_drop+1,order
                        assert slog().count('SOURCE_CANCELLED')==before_cancel,order
                        (root/'received').unlink()
                        command(target,tlog,'auto_feedback')
                    # The original touch grab drops only after OLE completion, even if owner is up.
                    lease=viewer.lease(first);gesture+=1;op=viewer.begin(lease,gesture);viewer.ack(op);offer=viewer.control('drag_offer')
                    viewer.send(type='drag_claim',drag=offer['drag']);viewer.control('drag_ready',drag=offer['drag'])
                    viewer.sample(op,'up');viewer.barrier()
                    assert not any(v.get('kind')=='end' and v.get('operation')==op['operation'] for v in viewer.controls)
                    viewer.send(type='drag_drop',drag=offer['drag']);viewer.ended(op)
                    # Payload remains readable after the global touch interaction ends.
                    owner={k:offer[k] for k in ('touch_operation','touch_generation','touch_gesture','touch_slot')}
                    viewer.send(type='drag_read',drag=offer['drag'],request=42,file=0,offset=0,length=18,**owner)
                    data=viewer.control('drag_data',drag=offer['drag'],request=42)
                    import base64
                    assert base64.b64decode(data['data'])==b'file touch payload',data
                    viewer.send(type='drag_result',drag=offer['drag'],success=True,**owner);viewer.control('drag_end',drag=offer['drag'],success=True)
                source.terminate();target.terminate();source.wait(timeout=5);target.wait(timeout=5);viewer.barrier()
            assert host.poll() is None,Path(hostlog.name).read_text()
            print('PASS: automatic live wl_touch bind/release metadata and leases; owned xdg move/resize, stale gesture/secondary-finger/late ACK rejection; popup coordinates and outside dismissal; actual URI/text touch DND receive+finish, source focus preservation, HWND exit, sink claim/return (all three UP/negotiation orders), deferred OLE drop and retained file ranges')
        finally:
            if viewer:viewer.sock.close()
            for app in apps:
                if app.poll() is None:app.terminate();app.wait(timeout=5)
            if host.poll() is None:host.terminate();host.wait(timeout=5)
            for log in logs:log.close()


if __name__=='__main__':main()
