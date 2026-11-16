#!/usr/bin/env python3
"""Independent XDND peer → real Qt/X11 dropEvent, text/URI and INCR."""
import base64,json,os,shlex,subprocess,tempfile,time
from pathlib import Path
from smoke import ROOT

def main():
 (ROOT/'build/tests').mkdir(parents=True,exist_ok=True)
 flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','Qt6Widgets'],text=True))
 subprocess.run(['c++','-std=c++17','-fPIC','tests/xdnd_target_app.cpp','-o','build/tests/xdnd-target-app',*flags],cwd=ROOT,check=True)
 subprocess.run(['cargo','build','--offline','--example','xdnd-source-probe'],cwd=ROOT,check=True)
 with tempfile.TemporaryDirectory(prefix='waybridge-xdnd-') as temporary:
  root=Path(temporary);log=(root/'test.log').open('w');read,write=os.pipe();children=[]
  x=subprocess.Popen(['Xvfb','-displayfd',str(write),'-screen','0','1280x800x24','-nolisten','tcp','-ac'],pass_fds=[write],stdout=log,stderr=log);children.append(x);os.close(write)
  with os.fdopen(read)as pipe:display=pipe.readline().strip()
  env={**os.environ,'DISPLAY':':'+display,'QT_QPA_PLATFORM':'xcb','QT_QPA_PLATFORMTHEME':'','QT_STYLE_OVERRIDE':'Fusion','XDG_CURRENT_DESKTOP':'Waybridge','DBUS_SESSION_BUS_ADDRESS':'unix:path=/nonexistent','XDG_CONFIG_HOME':str(root/'config'),'XDG_DATA_HOME':str(root/'data')}
  try:
   for label,mime,data in [('text','text/plain;charset=utf-8','hello 世界 λ'.encode()),('incr','text/plain;charset=utf-8',('数据0123456789\n'*12000).encode()),('uri','text/uri-list',None)]:
    resultfile=root/(label+'.jsonl');copy=root/(label+'.copy');payload=root/(label+'.payload')
    if data is None:
     original=root/'original λ.txt';original.write_bytes(b'copy-only original\n');original.chmod(0o444);data=original.as_uri().encode()+b'\r\n'
    payload.write_bytes(data)
    app=subprocess.Popen([str(ROOT/'build/tests/xdnd-target-app'),str(resultfile),str(copy)],env=env,stdout=subprocess.PIPE,stderr=log,text=True);children.append(app)
    xid=app.stdout.readline().strip();assert xid.isdecimal(),(root/'test.log').read_text()
    probe=subprocess.run([str(ROOT/'target/debug/examples/xdnd-source-probe'),display,xid,'200','150',mime,str(payload),label],env=env,cwd=ROOT,capture_output=True,text=True,timeout=20)
    assert probe.returncode==0,probe.stdout+probe.stderr+'\n'+(root/'test.log').read_text()
    events=[json.loads(line)for line in resultfile.read_text().splitlines()];drop=next(e for e in events if e['event']=='drop')
    received=base64.b64decode(drop['data'])
    if label=='uri':assert drop['copied'] and copy.read_bytes()==original.read_bytes() and received==b'copy-only original\n'
    else:assert received==data,(label,len(received),len(data))
    assert (drop['x'],drop['y'])==(80,60),drop
    app.terminate();app.wait(timeout=5)
    print('PASS: real Qt/X11',label,'COPY drop, bytes, root/local coordinates and Finished')
  finally:
   for child in reversed(children):
    if child.poll()is None:child.terminate()
   for child in children:
    try:child.wait(timeout=5)
    except subprocess.TimeoutExpired:child.kill();child.wait()
   log.close()
if __name__=='__main__':main()
