#!/usr/bin/python3
"""Real Wayland animated dnd_icon, capture limits and source lifecycle."""
import base64,os,socket,subprocess,tempfile,time
from pathlib import Path
from smoke import Viewer,ROOT
from session_fixture import SessionFixture

class Client(Viewer):
 def __init__(self,port,token,enabled=True):
  self.sock=socket.create_connection(('127.0.0.1',port),timeout=10);self.geometry={}
  self.send(type='hello',version=3,token=token,data_drag_v1=enabled)
  assert self.receive()[1]['data_drag_v1']==enabled
 def visual(self,predicate=lambda _:True):return self.until(lambda k,v:k=='control' and v['type']=='drag_visual' and predicate(v))

def wait(check):
 end=time.monotonic()+10
 while time.monotonic()<end:
  if check():return
  time.sleep(.01)
 raise AssertionError('DND icon fixture timed out')

def main():
 subprocess.run(['bash','tests/build-fixture.sh'],cwd=ROOT,check=True)
 with tempfile.TemporaryDirectory(prefix='wb-dnd-visual-') as d,SessionFixture(Path(d)/'fixture') as f:
  root=Path(d);runtime=Path(f.env['XDG_RUNTIME_DIR']);token='visual-'+('a'*40)
  (runtime/'token').write_text(token);(runtime/'token').chmod(0o600)
  with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
  log=(root/'events.log').open('w+')
  host=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--socket','visual-fixture','--listen',f'127.0.0.1:{port}','--token-file',str(runtime/'token'),'--no-audio','--no-xwayland','--codec','png','--fps','1'],env=f.env,stdout=log,stderr=log)
  app=None;viewer=None
  try:
   wait(lambda:(runtime/'visual-fixture.session.json').exists());viewer=Client(port,token)
   def launch(**extra):
    nonlocal app
    app=subprocess.Popen([str(ROOT/'build/tests/test-app')],env={**f.env,'WAYLAND_DISPLAY':'visual-fixture','WAYBRIDGE_TEST_DRAG_ICON':'1',**extra},stdout=log,stderr=log)
    window=viewer.until(lambda k,v:k=='control' and v['type']=='window')['id']
    viewer.until(lambda k,v:k=='frame' and v[0]==window)
    return window
   def begin(window):
    viewer.send(type='pointer',id=window,x=50,y=70);viewer.send(type='button',id=window,button=272,down=True)
   window=launch();begin(window)
   first=viewer.visual(lambda v:v.get('visible'));assert first['width']==64 and first['height']==32
   assert first['offset_x']==-6 and first['offset_y']==-8 and first['scale']==1
   pixels=base64.b64decode(first['pixels']);assert len(pixels)==64*32*4 and pixels[:4]==bytes(4)
   assert pixels[4:8] in (bytes([128,0,0,128]),bytes([0,128,0,128]))
   updates=[first]
   for _ in range(4):updates.append(viewer.visual(lambda v:v.get('visible')))
   assert len({v['pixels'] for v in updates})==2
   assert all(a['revision']<b['revision'] for a,b in zip(updates,updates[1:]))
   before=time.monotonic();viewer.send(type='button',id=window,button=272,down=False)
   ended=viewer.visual(lambda v:not v['active']);assert ended['generation']==first['generation'] and time.monotonic()-before<.5
   app.terminate();app.wait(timeout=5);app=None
   # Oversized sources are hidden but their real frame callbacks keep running,
   # independent of the session's intentionally slow 1 FPS application stream.
   window=launch(WAYBRIDGE_TEST_DRAG_ICON_EDGE='513');before=Path(log.name).read_text().count('DND_ICON_FRAME');begin(window)
   hidden=viewer.visual(lambda v:v['active'] and not v.get('visible',False));assert 'pixels' not in hidden
   wait(lambda:Path(log.name).read_text().count('DND_ICON_FRAME')>=before+4)
   viewer.send(type='button',id=window,button=272,down=False);viewer.visual(lambda v:not v['active'])
   app.terminate();app.wait(timeout=5);app=None
   window=launch(WAYBRIDGE_TEST_ICON_DESTROY_AFTER='4');begin(window)
   visible=viewer.visual(lambda v:v.get('visible'));destroyed=viewer.visual(lambda v:not v['active']);assert destroyed['generation']==visible['generation']
   viewer.send(type='button',id=window,button=272,down=False)
   app.terminate();app.wait(timeout=5);app=None
   viewer.sock.close();viewer=None
   viewer=Client(port,token,enabled=False)
   window=launch();begin(window)
   before=Path(log.name).read_text().count('DND_ICON_FRAME')
   wait(lambda:Path(log.name).read_text().count('DND_ICON_FRAME')>=before+4)
   viewer.send(type='ping',nonce='no-visual-negotiation')
   while True:
    kind,value=viewer.receive()
    if kind=='control':
     assert value['type']!='drag_visual','visual sent without data_drag_v1'
     if value['type']=='pong':break
   viewer.send(type='button',id=window,button=272,down=False)
   print('PASS: actual animated Wayland dnd_icon RGBA/alpha/offset, commit revisions, independent frame callbacks at 1 FPS, drop/icon-destroy cleanup and oversize rejection')
  finally:
   if viewer:viewer.sock.close()
   if app:app.terminate();app.wait(timeout=5)
   host.terminate();host.wait(timeout=10);log.close()

if __name__=='__main__':main()
