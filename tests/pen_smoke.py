#!/usr/bin/env python3
"""Actual tablet-v2 tool events, resource-based native eligibility and stroke leases."""
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from smoke import ROOT, Viewer
from touch_smoke import wait

class PenViewer(Viewer):
    def __init__(self,port,token,capability=True,preference=True):
        self.sock=socket.create_connection(('127.0.0.1',port),timeout=8)
        self.geometry={};self.state={};self.controls=[]
        self.send(type='hello',version=3,token=token,dpi=168,window_semantics_v1=True,pen_v1=capability,pen_enabled=preference)
        hello=self.receive()[1];assert hello.get('pen_v1',False)==capability,hello
    def receive(self):
        kind,value=super().receive()
        if kind=='control':
            self.controls.append(value)
            if value.get('type')=='pen_state':self.state=value
        return kind,value
    def control(self,kind,**matches):return self.until(lambda k,v:k=='control' and v.get('type')==kind and all(v.get(n)==x for n,x in matches.items()))
    def barrier(self):
        nonce=time.monotonic_ns();self.send(type='ping',nonce=nonce);self.control('pong',nonce=nonce);time.sleep(.08)
    def lease(self,window):
        self.send(type='focus',id=window,focused=True);self.barrier()
        if not(self.state.get('enabled') and self.state.get('id')==window):
            self.until(lambda k,v:k=='control' and v.get('type')=='pen_state' and v.get('enabled') and v.get('id')==window)
        return dict(self.state)
    def pen(self,lease,gesture,sequence,phase='motion',**extra):
        value=dict(type='pen',id=lease['id'],target=lease['id'],generation=lease['generation'],gesture=gesture,sequence=sequence,phase=phase,tool=7,eraser=False,tip=False,barrel=False,x=100.25,y=80.75,capabilities=15)
        value.update(extra);self.send(**value)

def main():
    subprocess.run(['bash','tests/build-pen-fixture.sh'],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-pen-') as directory:
        root=Path(directory);env={**os.environ,'XDG_RUNTIME_DIR':directory,'WAYLAND_DISPLAY':'pen-0'}
        token='native-pen-'+'a'*40;secret=root/'token';secret.write_text(token);secret.chmod(0o600)
        with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
        with open(ROOT/'build/tests/pen-host.log','w') as host_log,open(ROOT/'build/tests/pen-app.log','w') as app_log:
            host=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--no-audio','--no-xwayland','--codec','png','--socket','pen-0','--listen',f'127.0.0.1:{port}','--token-file',str(secret)],env=env,stdout=host_log,stderr=host_log)
            app=viewer=None;log=lambda:Path(app_log.name).read_text()
            def command(text):
                before=log().count('COMMAND\n');app.stdin.write(text+'\n');app.stdin.flush();wait(lambda:log().count('COMMAND\n')>before)
            def count():return sum(log().count(n) for n in ('PEN_IN','PEN_OUT','PEN_DOWN','PEN_UP','PEN_MOTION','PEN_PRESSURE','PEN_TILT','PEN_ROTATION','PEN_BUTTON','PEN_FRAME'))
            try:
                wait(lambda:(root/'pen-0').exists() or host.poll() is not None)
                assert host.poll() is None,Path(host_log.name).read_text()
                viewer=PenViewer(port,token,False)
                app=subprocess.Popen([str(ROOT/'build/tests/pen-fixture/pen-app')],stdin=subprocess.PIPE,text=True,env=env,stdout=app_log,stderr=app_log)
                ids={}
                while len(ids)<2:
                    k,v=viewer.receive()
                    if k=='control' and v.get('type')=='window':ids[v['title']]=v['id'];assert not v['pen_input'],v
                first,other=ids['Pen fixture first'],ids['Pen fixture other']
                command('bind');wait(lambda:'TABLET_DONE' in log())
                assert 'PEN_IN' not in log(),'tablet advertised only after first proximity'
                viewer.until(lambda k,v:k=='control' and v.get('type')=='window' and v['id']==first and v['pen_input'])
                viewer.send(type='pen_enabled',enabled=True);viewer.send(type='hello',pen_v1=True,pen_enabled=True)
                viewer.pen(dict(id=first,generation=1),1,1,'in',tip=True,pressure=.5);viewer.barrier();assert count()==0,log()
                viewer.sock.close();time.sleep(.25)
                viewer=PenViewer(port,token,preference=False)
                viewer.control('window',id=other);viewer.send(type='focus',id=first,focused=True);viewer.barrier()
                assert viewer.state.get('requested') is False and not viewer.state.get('enabled'),viewer.state
                viewer.send(type='pen_enabled',enabled=True);lease=viewer.lease(first)
                viewer.pen(lease,1,1,'in');wait(lambda:'PEN_IN 1' in log())
                assert 'PEN_PRESSURE' not in log() and 'PEN_TILT' not in log() and 'PEN_ROTATION' not in log(),'fabricated axes'
                viewer.pen(lease,1,2,pressure=.25,tilt_x=-20.5,tilt_y=30.25,rotation=270.)
                wait(lambda:'PEN_PRESSURE 16384' in log() and 'PEN_TILT -20.500 30.250' in log() and 'PEN_ROTATION 270.000' in log())
                assert 'PEN_MOTION 100.250 80.750' in log(),log()
                viewer.pen(lease,1,3,tip=True,barrel=True,pressure=1.);wait(lambda:'PEN_DOWN' in log() and 'PEN_BUTTON 331 1' in log());viewer.barrier()
                before=count()
                for extra in (dict(pressure=2),dict(tilt_x='NaN'),dict(rotation=361),dict(tip='true'),dict(target=other),dict(generation=lease['generation']+10)):
                    viewer.pen(lease,1,4,**extra)
                viewer.pen(lease,1,2);viewer.pen(lease,99,4);viewer.barrier();assert count()==before,log()
                before_up=log().count('PEN_UP');before_out=log().count('PEN_OUT');before_button=log().count('PEN_BUTTON 331 0')
                viewer.lease(other);wait(lambda:log().count('PEN_UP')>before_up and log().count('PEN_OUT')>before_out and log().count('PEN_BUTTON 331 0')>before_button)
                before=count();viewer.pen(lease,1,4,tip=True);viewer.barrier();assert count()==before,'stale focus stroke'
                lease=viewer.lease(first);viewer.pen(lease,2,1,'in',eraser=True,tip=True,barrel=True)
                wait(lambda:'PEN_TYPE 321' in log());before_out=log().count('PEN_OUT')
                viewer.send(type='pen_enabled',enabled=False);viewer.barrier();wait(lambda:log().count('PEN_OUT')>before_out)
                assert viewer.state.get('requested') is False and not viewer.state.get('enabled'),viewer.state
                viewer.send(type='pen_enabled',enabled=True);renewed=viewer.lease(first)
                assert renewed['generation']>lease['generation'];before=count();viewer.pen(lease,2,2,eraser=True,tip=True);viewer.barrier();assert count()==before
                viewer.pen(renewed,3,1,'in',tip=True,barrel=True);wait(lambda:log().count('PEN_IN')>=3)
                before_out=log().count('PEN_OUT');command('unbind')
                viewer.until(lambda k,v:k=='control' and v.get('type')=='window' and v['id']==first and not v['pen_input'])
                wait(lambda:log().count('PEN_OUT')>before_out);viewer.barrier();assert not viewer.state.get('enabled'),viewer.state
                before_tablet=log().count('TABLET_DONE');command('bind');wait(lambda:log().count('TABLET_DONE')>before_tablet)
                viewer.until(lambda k,v:k=='control' and v.get('type')=='window' and v['id']==first and v['pen_input'])
                lease=viewer.lease(first);viewer.pen(lease,4,1,'in',tip=True,barrel=True);viewer.barrier()
                before_out=log().count('PEN_OUT');viewer.send(type='stream_config',request_id=91,codec='png',fps=60,bitrate=30000,read_only=True)
                viewer.barrier();wait(lambda:log().count('PEN_OUT')>before_out)
                viewer.send(type='stream_config',request_id=92,codec='png',fps=60,bitrate=30000,read_only=False)
                lease=viewer.lease(first);viewer.pen(lease,5,1,'in',tip=True,barrel=True);viewer.barrier()
                before_out=log().count('PEN_OUT');command('hide');wait(lambda:log().count('PEN_OUT')>before_out)
                command('show');lease=viewer.lease(first);viewer.pen(lease,6,1,'in',tip=True,barrel=True);viewer.barrier()
                before_out=log().count('PEN_OUT');viewer.pen(lease,6,2,'cancel');wait(lambda:log().count('PEN_OUT')>before_out)
                lease=viewer.lease(first);viewer.pen(lease,7,1,'in',tip=True,barrel=True);viewer.barrier()
                before_out=log().count('PEN_OUT');viewer.sock.close();wait(lambda:log().count('PEN_OUT')>before_out)
                time.sleep(.2);viewer=PenViewer(port,token);lease=viewer.lease(first);before_in=log().count('PEN_IN')
                before_tilt=log().count('PEN_TILT')
                viewer.pen(lease,1,1,'in',tilt_x=10.);wait(lambda:log().count('PEN_IN')>before_in);viewer.barrier()
                assert log().count('PEN_TILT')==before_tilt,'missing tilt axis was fabricated'
                viewer.pen(lease,1,2,tilt_y=20.);wait(lambda:'PEN_TILT 10.000 20.000' in log())
                viewer.pen(lease,1,3,'out');viewer.barrier()
                # A pressure-less device still gets usable tip events without a Pressure cap.
                before_caps=log().count('PEN_CAP');before_down=log().count('PEN_DOWN')
                viewer.pen(lease,2,1,'in',tool=50,capabilities=0,tip=True);viewer.barrier()
                assert log().count('PEN_CAP')==before_caps and log().count('PEN_DOWN')==before_down+1
                before_ready=log().count('PEN_READY');before_pressure=log().count('PEN_PRESSURE')
                viewer.pen(lease,2,2,tool=50,capabilities=0,tip=True,pressure=.5,tilt_x=5,tilt_y=10,rotation=90);viewer.barrier()
                assert log().count('PEN_READY')==before_ready and log().count('PEN_PRESSURE')==before_pressure,'mid-stroke capability replacement'
                viewer.pen(lease,2,3,'out',tool=50,capabilities=0)
                viewer.pen(lease,3,1,'in',tool=50,capabilities=0,tip=True);viewer.barrier()
                assert log().count('PEN_READY')==before_ready+1 and log().count('PEN_CAP')==before_caps+3,'observed capability upgrade missing'
                viewer.pen(lease,3,2,tool=50,pressure=.5);wait(lambda:log().count('PEN_PRESSURE')>before_pressure)
                viewer.pen(lease,3,3,'out',tool=50)
                # Real tablet tip serials authorize only their owning held tip/toplevel.
                command('invalid_move');before_actions=len(viewer.controls)
                viewer.pen(lease,4,1,'in',tip=True);viewer.barrier()
                assert not any(v.get('input')=='pen' and v.get('type')=='window_action' for v in viewer.controls[before_actions:])
                viewer.pen(lease,4,2,tip=False);viewer.barrier();command('stale_move');viewer.barrier()
                assert not any(v.get('input')=='pen' and v.get('type')=='window_action' for v in viewer.controls[before_actions:])
                viewer.pen(lease,4,3,'out')
                for gesture,action in ((5,'move'),(6,'resize')):
                    command(action);lease=viewer.lease(first);before_out=log().count('PEN_OUT')
                    viewer.pen(lease,gesture,1,'in',tip=True,barrel=True)
                    request=viewer.control('window_action',input='pen',action=action)
                    assert request['pen_generation']==lease['generation'] and request['pen_gesture']==gesture and request['pen_tool']==7 and request['pen_eraser'] is False,request
                    if action=='resize':assert request['edge']==10,request
                    wait(lambda:log().count('PEN_OUT')>before_out);viewer.barrier()
                    assert not viewer.state['enabled'] and viewer.state['window_operation']==request['pen_operation'],viewer.state
                    ending=dict(type='pen_window_end',operation=request['pen_operation'],generation=request['pen_generation'],gesture=gesture,tool=7,eraser=False,cancelled=False)
                    viewer.send(**{**ending,'gesture':gesture+100});viewer.barrier()
                    assert viewer.state.get('window_operation')==request['pen_operation'],'stale end finished new operation'
                    viewer.send(**ending);lease=viewer.lease(first)
                command('move');viewer.pen(lease,7,1,'in',tip=True)
                request=viewer.control('window_action',input='pen',action='move');viewer.barrier()
                viewer.send(type='stream_config',request_id=93,codec='png',fps=60,bitrate=30000,read_only=True);viewer.barrier()
                assert not viewer.state.get('window_operation') and not viewer.state['enabled'],'policy preserved pen window ownership'
                viewer.send(type='stream_config',request_id=94,codec='png',fps=60,bitrate=30000,read_only=False);lease=viewer.lease(first);command('none')
                # A trusted connection still has a hard tool-resource bound.
                for identity in range(8,38):
                    gesture=identity+10
                    viewer.pen(lease,gesture,1,'in',tool=identity)
                    viewer.pen(lease,gesture,2,'out',tool=identity)
                viewer.barrier();before_in=log().count('PEN_IN')
                viewer.pen(lease,50,1,'in',tool=39);viewer.barrier()
                assert log().count('PEN_IN')==before_in,'server tool resource cache exceeded 32'
                assert host.poll() is None and app.poll() is None
                print('PASS: actual tablet-v2 early device/resource bind, automatic lease bind/release, hover/motion/valid optional axes, pressure/tilt/rotation, tip/barrel/eraser, malformed/stale rejection, focus/live settings/read-only/unmap/captureloss/disconnect releases, reconnect recovery, held-tip CSD move/resize and policy/stale rejection, pressure-less tools and between-proximity capability upgrades, bounded tool resources')
            finally:
                if viewer:viewer.sock.close()
                if app and app.poll() is None:app.terminate();app.wait(timeout=5)
                if host.poll() is None:host.terminate();host.wait(timeout=5)
if __name__=='__main__':main()
