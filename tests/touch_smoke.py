#!/usr/bin/env python3
"""Real wl_touch contacts, per-focus leases, batching, cancellation and legacy gating."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from smoke import ROOT, Viewer


def wait(check, timeout=8):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if check(): return
        time.sleep(.02)
    raise AssertionError('touch fixture timeout')


class TouchViewer(Viewer):
    def __init__(self, port, token, enabled=True, preference=None):
        self.geometry = {}
        self.state = {}
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=8)
        hello = dict(type='hello', version=3, token=token, touch_v1=enabled, window_semantics_v1=True, dpi=168)
        if preference is not None: hello['touch_enabled'] = preference
        self.send(**hello)
        hello = self.receive()[1]
        assert hello['type'] == 'hello' and hello.get('touch_v1', False) == enabled
    def receive(self):
        kind, value = super().receive()
        if kind == 'control' and value.get('type') == 'touch_state': self.state = value
        return kind, value
    def barrier(self):
        nonce = time.monotonic_ns()
        self.send(type='ping', nonce=nonce)
        self.until(lambda k, v: k == 'control' and v.get('type') == 'pong' and v['nonce'] == nonce)
        time.sleep(.08)
    def lease(self, window):
        self.send(type='focus', id=window, focused=True)
        self.barrier()
        if not (self.state.get('enabled') and self.state.get('id') == window):
            self.until(lambda k, v: k == 'control' and v.get('type') == 'touch_state' and v.get('enabled') and v.get('id') == window)
        assert self.state.get('enabled') and self.state.get('id') == window, self.state
        return dict(self.state)
    def touch(self, lease, points=None, **changes):
        self.send(**dict(type='touch', id=lease['id'], generation=lease['generation'],
                         points=points or [], **changes))


def point(slot, phase='down', x=100.25, y=80.75):
    return dict(slot=slot, phase=phase, x=x, y=y)


def main():
    subprocess.run(['bash', 'tests/build-touch-fixture.sh'], cwd=ROOT, check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-touch-') as runtime, \
            open(ROOT/'build/tests/touch-host.log','w') as host_log, \
            open(ROOT/'build/tests/touch-app.log','w') as app_log:
        env = {**os.environ, 'XDG_RUNTIME_DIR':runtime, 'WAYLAND_DISPLAY':'touch-0'}
        with socket.socket() as free:
            free.bind(('127.0.0.1',0)); port=free.getsockname()[1]
        token='touch-test-'+'d'*40
        token_file=Path(runtime)/'token';token_file.write_text(token);token_file.chmod(0o600)
        host=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--codec','png','--no-audio',
                               '--no-xwayland','--listen',f'127.0.0.1:{port}','--socket','touch-0',
                               '--token-file',str(token_file)],env=env,stdout=host_log,stderr=host_log)
        app=viewer=None
        log=lambda:Path(app_log.name).read_text()
        def command(text):
            before=log().count('COMMAND\n');app.stdin.write(text+'\n');app.stdin.flush()
            wait(lambda:log().count('COMMAND\n')>before)
        def event_count(): return log().count('TOUCH_')
        try:
            wait(lambda:(Path(runtime)/'touch-0').exists() or host.poll() is not None)
            assert host.poll() is None,Path(host_log.name).read_text()
            viewer=TouchViewer(port,token,False)
            app=subprocess.Popen([str(ROOT/'build/tests/touch-app')],stdin=subprocess.PIPE,text=True,
                                 env=env,stdout=app_log,stderr=app_log)
            ids={}
            while len(ids)<2:
                kind,value=viewer.receive()
                if kind=='control' and value.get('type')=='window': ids[value['title']]=value['id']
            first,other=ids['Touch fixture first'],ids['Touch fixture other']
            viewer.send(type='focus',id=first,focused=True);viewer.barrier()
            viewer.send(type='touch_enabled',enabled=True)
            viewer.send(type='hello',touch_v1=True,touch_enabled=True)
            viewer.send(type='touch',id=first,generation=1,points=[point(0)])
            viewer.barrier();assert event_count()==0,log()
            assert not viewer.state, 'legacy connection upgraded touch capability after hello'
            viewer.sock.close();time.sleep(.25)
            viewer=TouchViewer(port,token,preference=False)
            viewer.until(lambda k,v:k=='control' and v.get('type')=='window' and v['id']==other)
            viewer.send(type='focus',id=first,focused=True);viewer.barrier()
            assert not viewer.state.get('enabled') and viewer.state.get('requested') is False, viewer.state
            viewer.send(type='touch',id=first,generation=viewer.state['generation'],points=[point(0)])
            viewer.barrier();assert event_count()==0,log()
            viewer.send(type='touch_enabled',enabled=True)
            lease=viewer.lease(first)
            viewer.touch(lease,[point(0),point(1,x=200.5,y=160.25)])
            wait(lambda:'TOUCH_DOWN 0 1 100.250 80.750' in log() and 'TOUCH_DOWN 1 1 200.500 160.250' in log())
            before=log().count('TOUCH_FRAME\n')
            viewer.touch(lease,[point(0,'motion',120.5,90.25),point(1,'motion',220.25,180.5)])
            wait(lambda:log().count('TOUCH_FRAME\n')>before)
            assert 'TOUCH_MOTION 0 120.500 90.250' in log()
            # Validate a whole batch before sending any Wayland event.
            before=event_count()
            for invalid in ([point(0,'motion'),point(1)], [point(10)], [point(2,x='NaN')],
                            [point(2),point(2,'motion')], [point(2,x=1e20)]):
                viewer.touch(lease,invalid)
            viewer.send(type='touch',id=other,generation=lease['generation'],points=[point(2)])
            viewer.send(type='touch',id=first,generation=lease['generation']+100,points=[point(2)])
            viewer.barrier();assert event_count()==before,log()
            viewer.touch(lease,[point(0,'up'),point(1,'up')])
            wait(lambda:'TOUCH_UP 0' in log() and 'TOUCH_UP 1' in log())
            viewer.touch(lease,[point(i,x=20+i*10,y=30+i*5) for i in range(10)])
            wait(lambda:log().count('TOUCH_DOWN ')==12)
            before=log().count('TOUCH_CANCEL\n')
            changed=viewer.lease(other)
            wait(lambda:log().count('TOUCH_CANCEL\n')>before)
            assert log().count('TOUCH_CANCEL\n') == before + 1, 'one gesture cancellation per client'
            assert changed['generation']>lease['generation']
            before=event_count();viewer.touch(lease,[point(0,'motion')]);viewer.barrier()
            assert event_count()==before,log()
            lease=viewer.lease(first);viewer.touch(lease,[point(0)])
            wait(lambda:log().count('TOUCH_DOWN ')==13)
            before=log().count('TOUCH_CANCEL\n')
            viewer.send(type='stream_config',request_id=81,codec='png',fps=60,bitrate=30000,read_only=True)
            viewer.barrier();wait(lambda:log().count('TOUCH_CANCEL\n')>before)
            before=event_count();viewer.touch(lease,[point(0)]);viewer.barrier();assert event_count()==before
            viewer.send(type='stream_config',request_id=82,codec='png',fps=60,bitrate=30000,read_only=False)
            viewer.barrier();lease=viewer.lease(first)
            viewer.touch(lease,[point(0)]);wait(lambda:log().count('TOUCH_DOWN ')==14)
            before=log().count('TOUCH_CANCEL\n');command('hide')
            viewer.barrier();wait(lambda:log().count('TOUCH_CANCEL\n')>before)
            before=event_count();viewer.touch(lease,[point(0,'motion')]);viewer.barrier();assert event_count()==before
            command('show');viewer.barrier();lease=viewer.lease(first)
            viewer.touch(lease,[point(0)]);wait(lambda:log().count('TOUCH_DOWN ')==15)
            before=log().count('TOUCH_CANCEL\n');viewer.sock.close()
            wait(lambda:log().count('TOUCH_CANCEL\n')>before);time.sleep(.2)
            viewer=TouchViewer(port,token);lease=viewer.lease(first)
            viewer.touch(lease,[point(0)]);wait(lambda:log().count('TOUCH_DOWN ')==16)
            before=log().count('TOUCH_CANCEL\n');command('destroy')
            wait(lambda:log().count('TOUCH_CANCEL\n')>before)
            viewer.barrier();assert not viewer.state.get('enabled'),viewer.state
            lease=viewer.lease(other)
            viewer.touch(lease,[point(0)])
            wait(lambda:log().count('TOUCH_DOWN ')==17)
            before=log().count('TOUCH_CANCEL\n')
            viewer.send(type='touch_enabled',enabled=False)
            viewer.barrier();wait(lambda:log().count('TOUCH_CANCEL\n')>before)
            assert viewer.state.get('requested') is False and not viewer.state.get('enabled'),viewer.state
            before=event_count();viewer.touch(lease,[point(0,'motion')]);viewer.barrier()
            assert event_count()==before,log()
            viewer.send(type='touch_enabled',enabled=True)
            renewed=viewer.lease(other)
            assert renewed['generation']>lease['generation']
            before=event_count();viewer.touch(lease,[point(0,'motion')]);viewer.barrier()
            assert event_count()==before,'preference reenable accepted stale contact'
            viewer.touch(renewed,[point(0)]);wait(lambda:log().count('TOUCH_DOWN ')==18)
            viewer.touch(renewed,[point(0,'up')]);viewer.barrier()
            viewer.send(type='touch_enabled',enabled='false');viewer.barrier()
            assert viewer.state.get('requested') is True,'malformed preference altered state'
            assert app.poll() is None and host.poll() is None
            print('PASS: initial touch preference off, live on/off/cancel/fresh lease, immutable capability and legacy preference compatibility; actual wl_touch 175% two/ten contacts and frames, atomic malformed-batch rejection, focus/stale generation, read-only, unmap/remap, disconnect/reconnect and surface-death cancellation; legacy opt-out')
        finally:
            if viewer: viewer.sock.close()
            if app and app.poll() is None:app.terminate();app.wait(timeout=5)
            if host.poll() is None:host.terminate();host.wait(timeout=5)


if __name__=='__main__':main()
