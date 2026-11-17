#!/usr/bin/env python3
"""Production hosted PE worker: startup preference, live toggles and old-server gating."""
import json
import os
from pathlib import Path
import queue
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
from gui_session_smoke import Rpc, wait, stop_process
from udp_flow_smoke import DelayProxy, read_packet
from smoke import ROOT


class TouchRelay(DelayProxy):
    def __init__(self, address, legacy):
        self.legacy = legacy
        self.hello = None
        self.toggles = []
        super().__init__(address)
    def forward(self, source, target, upstream):
        try:
            while not self.closed.is_set():
                body = read_packet(source)
                if body[0] == 0:
                    value = json.loads(body[1:])
                    if upstream and value.get('type') == 'hello' and not value.get('channel'):
                        self.hello = {key: value.get(key) for key in ('touch_v1', 'touch_enabled')}
                    if upstream and value.get('type') == 'touch_enabled':
                        self.toggles.append(value.get('enabled'))
                    if not upstream and value.get('type') == 'hello' and value.get('codec') == 'mixed' and self.legacy:
                        value.pop('touch_settings_v1', None)
                        body = b'\0' + json.dumps(value).encode()
                target.sendall(struct.pack('>I', len(body)) + body)
        except (EOFError, OSError):
            pass
        except Exception as error:
            self.errors.append(error)
        finally:
            try: target.shutdown(socket.SHUT_WR)
            except OSError: pass


def run_worker(session, wine, legacy, log):
    relay = TouchRelay(('127.0.0.1', session['port']), legacy)
    worker = None
    try:
        native_log = ROOT/f'build/tests/touch-settings-worker-{int(legacy)}.log'
        worker = subprocess.Popen(['wine', str(ROOT/'build/tests/windows-control-probe.exe'),
                                   'Z:'+str(ROOT/'build/windows/waybridge-client.exe'), 'Z:'+str(native_log)],
                                  env=wine, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=log, text=True)
        events, history = queue.Queue(), []
        def read():
            for line in worker.stdout:
                value = json.loads(line)
                history.append(value); events.put(value)
        threading.Thread(target=read, daemon=True).start()
        def send(**value): worker.stdin.write(json.dumps(value)+'\n'); worker.stdin.flush()
        def until(check):
            for _ in range(120):
                value = events.get(timeout=20)
                if check(value): return value
            raise AssertionError('expected hosted touch event missing')
        def requested(enabled, after=0):
            matches = [e for e in history[after:] if e.get('type') == 'touch_state' and e.get('requested') is enabled]
            return matches[-1] if matches else None
        # Missing saved field must default OFF in the native worker as well.
        settings = dict(codec='png', fps=60, bitrate=30000, audio=False, files=False)
        send(type='start', version=1, host='127.0.0.1', port=relay.port,
             token=session['token'], settings=settings)
        until(lambda e:e['type']=='connected')
        status=until(lambda e:e['type']=='status' and e.get('windows'))
        assert status['touch_supported'] and status['touch_live_supported'] is not legacy
        assert status['touch_enabled'] is False
        wait(lambda:relay.hello is not None)
        assert relay.hello==dict(touch_v1=True,touch_enabled=False),relay.hello
        wait(lambda:requested(False))
        start=len(history)
        settings['touch']=True;send(type='settings',settings=settings)
        if legacy:
            warning=until(lambda e:e['type']=='settings_warning')
            assert '重新连接' in warning['message']
            applied=until(lambda e:e['type']=='local_settings_applied')
            assert applied['settings']['touch'] is True and applied['touch_enabled'] is False
            time.sleep(.2)
            assert relay.toggles==[],relay.toggles
        else:
            applied=until(lambda e:e['type']=='local_settings_applied')
            assert applied['touch_enabled'] and applied['touch_live_supported']
            wait(lambda:requested(True,start))
            assert relay.toggles==[True],relay.toggles
            start=len(history)
            settings['touch']=False;send(type='settings',settings=settings)
            applied=until(lambda e:e['type']=='local_settings_applied')
            assert applied['touch_enabled'] is False
            wait(lambda:requested(False,start))
            assert relay.toggles==[True,False],relay.toggles
            settings['touch']='yes';send(type='settings',settings=settings)
            error=until(lambda e:e['type']=='error')
            assert 'boolean' in error['message']
            assert relay.toggles==[True,False]
        send(type='disconnect');worker.stdin.close();worker.wait(timeout=15)
        assert worker.returncode==0,native_log.read_text()
        assert not relay.errors,relay.errors
        assert session['token'] not in native_log.read_text()
    finally:
        relay.close()
        if worker and worker.poll() is None:
            worker.terminate();worker.wait(timeout=8)


def main():
    subprocess.run(['bash','tests/build-fixture.sh'],cwd=ROOT,check=True)
    subprocess.run(['x86_64-w64-mingw32-g++','-municode','-static','-O2',
                    'tests/windows_control_probe.cpp','-o','build/tests/windows-control-probe.exe'],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-touch-settings-') as directory, \
            open(ROOT/'build/tests/touch-settings.log','w') as log:
        read_fd,write_fd=os.pipe()
        xvfb=subprocess.Popen(['Xvfb','-displayfd',str(write_fd),'-screen','0','1280x1024x24',
                               '-nolisten','tcp'],pass_fds=[write_fd],stdout=log,stderr=log)
        os.close(write_fd)
        with os.fdopen(read_fd) as pipe: display=':'+pipe.readline().strip()
        wine={**os.environ,'DISPLAY':display,'WINEPREFIX':str(Path(directory)/'wine'),
              'WINEDEBUG':'-all','WINEDLLOVERRIDES':'winemenubuilder.exe=d;mscoree,mshtml=', 'WAYBRIDGE_GDI':'1'}
        rpc=Rpc({**os.environ,'XDG_RUNTIME_DIR':directory})
        server_pid=app_pid=None
        try:
            subprocess.run(['wineboot','-u'],env=wine,stdout=log,stderr=log,check=True,timeout=60)
            session=rpc.ok(op='ensure',session='touch-settings-0',binary=str(ROOT/'target/debug/waybridge'),
                           port=0,codec='png',audio=False,xwayland=False)
            server_pid=session['pid']
            app=rpc.ok(op='launch',session='touch-settings-0',binary=str(ROOT/'target/debug/waybridge'),
                       argv=[str(ROOT/'build/tests/test-app')]);app_pid=app['pid']
            run_worker(session,wine,False,log)
            time.sleep(.3)
            run_worker(session,wine,True,log)
            os.kill(server_pid,0)
            rpc.ok(op='end',session='touch-settings-0');stop_process(server_pid);server_pid=None
            print('PASS: real hosted PE startup touch OFF, live ON/OFF server preference and active-status feedback, boolean validation; old-server live switch warns/requires reconnect and sends no unsupported toggle')
        finally:
            rpc.close()
            for pid in (app_pid,server_pid):
                if pid: stop_process(pid)
            subprocess.run(['wineserver','-k'],env=wine,stdout=log,stderr=log,timeout=10)
            if xvfb.poll() is None:xvfb.terminate();xvfb.wait(timeout=5)


if __name__=='__main__':main()
