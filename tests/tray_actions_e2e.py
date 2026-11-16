#!/usr/bin/env python3
"""Production Windows client → TCP → compositor worker → actual Qt SNI/QAction."""
import argparse,json,os,shlex,socket,subprocess,tempfile,time
from pathlib import Path
from session_fixture import SessionFixture
from smoke import ROOT

def main():
 p=argparse.ArgumentParser();p.add_argument('--client',type=Path,default=ROOT/'build/windows/waybridge-client.exe');p.add_argument('--single-left',action='store_true');args=p.parse_args()
 (ROOT/'build/tests').mkdir(parents=True,exist_ok=True)
 qt_flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','Qt6Widgets','Qt6DBus'],text=True))
 subprocess.run(['c++','-std=c++17','-fPIC','tests/tray_action_app.cpp','-o','build/tests/tray-action-app',*qt_flags],cwd=ROOT,check=True)
 subprocess.run(['x86_64-w64-mingw32-g++','-std=c++17','-O1','-static','-DUNICODE','-D_UNICODE','tests/windows_tray_driver.cpp','-o','build/tests/windows-tray-driver.exe'],cwd=ROOT,check=True)
 with tempfile.TemporaryDirectory(prefix='waybridge-tray-actions-') as temp, SessionFixture(temp) as fixture:
  root=Path(temp);children=[];log=(root/'run.log').open('w');env={**fixture.env,'GVFS_DISABLE_FUSE':'1','WAYLAND_DISPLAY':'tray-actions-0','RUST_LOG':'waybridge=info,waybridge::desktop_tray=debug'}
  with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
  token='tray-action-'+40*'5';tokenfile=root/'token';tokenfile.write_text(token);tokenfile.chmod(0o600)
  def spawn(args,environment):
   child=subprocess.Popen(args,cwd=ROOT,env=environment,stdout=log,stderr=log);children.append(child);return child
  def wait(predicate,timeout=20):
   deadline=time.monotonic()+timeout
   while time.monotonic()<deadline:
    result=predicate()
    if result:return result
    time.sleep(.08)
   raise AssertionError('condition timed out; application events='+repr(events())+'\n'+(root/'run.log').read_text()[-5000:])
  def events():return [json.loads(line) for line in (root/'actions.jsonl').read_text().splitlines()] if (root/'actions.jsonl').exists() else []
  try:
   server=spawn([str(ROOT/'target/debug/waybridge'),'--socket','tray-actions-0','--listen',f'127.0.0.1:{port}','--token-file',str(tokenfile),'--codec','png','--no-audio'],env)
   wait(lambda:(Path(env['XDG_RUNTIME_DIR'])/'tray-actions-0.session.json').exists())
   app=spawn([str(ROOT/'target/debug/waybridge'),'--launch','--socket','tray-actions-0','--',str(ROOT/'build/tests/tray-action-app'),str(root/'actions.jsonl')],{**env,'QT_QPA_PLATFORM':'xcb'})
   wait(lambda:any(e['event']=='ready' for e in events()))
   read,write=os.pipe();x=subprocess.Popen(['Xvfb','-displayfd',str(write),'-screen','0','1280x800x24','-nolisten','tcp'],pass_fds=[write],stdout=log,stderr=log)
   children.append(x);os.close(write)
   with os.fdopen(read) as pipe:display=':'+pipe.readline().strip()
   wine={**os.environ,'DISPLAY':display,'WINEPREFIX':str(root/'wine'),'WINEDEBUG':'-all','WINEDLLOVERRIDES':'winemenubuilder.exe=d;mscoree,mshtml=','WAYBRIDGE_PNG':'1','WAYBRIDGE_GDI':'1','WAYBRIDGE_TRAY_TRACE':'1'}
   client=spawn(['wine',str(args.client),'127.0.0.1',str(port),token],wine)
   def driver(*args):return subprocess.run(['wine',str(ROOT/'build/tests/windows-tray-driver.exe'),*args],cwd=ROOT,env=wine,stdout=log,stderr=log,timeout=5).returncode==0
   wait(lambda:driver('left' if args.single_left else 'left-sequence'),45);wait(lambda:any(e['event']=='activate' and e['visible'] for e in events()))
   wait(lambda:any(e['event']=='focus' for e in events()))
   wait(lambda:driver('foreground'))
   time.sleep(.2);assert len([e for e in events() if e['event']=='activate'])==1,events()
   assert driver('right-sequence');wait(lambda:driver('choose','i'))
   wait(lambda:any(e['event']=='increment' and e['count']==1 for e in events()))
   assert driver('right-sequence');wait(lambda:driver('choose','q'));wait(lambda:app.poll() is not None)
   assert any(e['event']=='quit' for e in events()),events()
   print('PASS: production Win32 client native tray click/menu → real Qt activated/QAction state changes and normal Quit')
   print(events())
  finally:
   if 'wine' in locals():subprocess.run(['wineserver','-k'],env=wine,stdout=log,stderr=log)
   for c in reversed(children):
    if c.poll() is None:c.terminate()
   for c in children:
    try:c.wait(timeout=8)
    except subprocess.TimeoutExpired:c.kill();c.wait()
   log.close()
   if os.environ.get('WAYBRIDGE_TRAY_KEEP_LOG'):print((root/'run.log').read_text())
if __name__=='__main__':main()
