#!/usr/bin/env python3
"""An in-flight wl_output bind must survive another client's window removal."""
import os,socket,subprocess,tempfile,time
from pathlib import Path
from smoke import ROOT,Viewer
from session_fixture import SessionFixture

def wait(check):
    deadline=time.monotonic()+12
    while time.monotonic()<deadline:
        result=check()
        if result:return result
        time.sleep(.02)
    raise AssertionError('output bind fixture timed out')

def main():
    (ROOT/'build/tests/wallet-reliability').mkdir(parents=True,exist_ok=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client'],text=True).split()
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','tests/output_bind_race.c','-o','build/tests/output-bind-race',*flags],cwd=ROOT,check=True)
    subprocess.run(['bash','tests/build-fixture.sh'],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='wb-output-race-') as directory,SessionFixture(Path(directory)/'user') as fixture,open(ROOT/'build/tests/wallet-reliability/output-race.log','w') as log:
        env=fixture.env
        with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
        token=Path(directory)/'token';token.write_text('output-race-'+'a'*40);token.chmod(0o600)
        host=subprocess.Popen([ROOT/'target/debug/waybridge','--socket','output-race','--listen',f'127.0.0.1:{port}','--token-file',token,'--no-xwayland','--no-audio','--codec','png'],env=env,stdout=log,stderr=log)
        viewer=observer=owner=None
        try:
            wait(lambda:(Path(env['XDG_RUNTIME_DIR'])/'output-race.session.json').exists() or host.poll() is not None)
            assert host.poll() is None
            env=dict(env,WAYLAND_DISPLAY='output-race')
            viewer=Viewer(port,token.read_text())
            observer=subprocess.Popen([ROOT/'build/tests/output-bind-race'],env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,text=True)
            assert observer.stdout.readline().strip()=='READY'
            owner=subprocess.Popen([ROOT/'build/tests/test-app'],env=env,stdout=log,stderr=log)
            info=viewer.until(lambda k,v:k=='control' and v.get('type')=='window')
            assert observer.stdout.readline().startswith('QUEUED ')
            viewer.send(type='close',id=info['id'])
            viewer.until(lambda k,v:k=='control' and v.get('type')=='window_closed' and v.get('id')==info['id'])
            count=subprocess.check_output([ROOT/'build/tests/output-bind-race','--count'],env=env,text=True,timeout=5)
            assert count.strip()=='1','retired output was advertised to a new client'
            time.sleep(.2)
            observer.stdin.write('f');observer.stdin.flush()
            assert observer.stdout.readline().strip()=='LATE_BIND_OK','queued output bind was rejected'
            # Bound objects remain valid even after the grace period reclaims
            # their global. Inactive globals are not leaked indefinitely.
            time.sleep(10.2)
            observer.stdin.write('r');observer.stdin.flush()
            output=observer.communicate(timeout=8)[0]
            assert observer.returncode==0 and 'RELEASE_OK' in output,(observer.returncode,output)
            print('PASS: queued wl_output v4 bind survives window removal, new clients do not see the retired global, bound object survives deferred global cleanup')
        finally:
            if viewer:viewer.sock.close()
            for process in (owner,observer,host):
                if process and process.poll() is None:process.terminate();process.wait(timeout=10)

if __name__=='__main__':main()
