#!/usr/bin/env python3
"""Private D-Bus SNI/DBusMenu vertical integration against the real compositor."""
import base64
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
from PIL import Image
from smoke import Viewer, ROOT
from session_fixture import SessionFixture

class TrayViewer(Viewer):
    def __init__(self,port,token,enabled=True):
        self.sock=socket.create_connection(('127.0.0.1',port),timeout=10);self.geometry={}
        self.send(type='hello',version=3,token=token,tray_v1=enabled)
        hello=self.receive()[1];assert hello['tray_v1']==enabled,hello
    def control(self,name,predicate=lambda value:True):
        return self.until(lambda kind,value:kind=='control' and value.get('type')==name and predicate(value))

def main():
    with tempfile.TemporaryDirectory(prefix='waybridge-tray-') as temporary,SessionFixture(temporary) as fixture:
        env={**fixture.env,'GVFS_DISABLE_FUSE':'1','WAYLAND_DISPLAY':'tray-0'}
        root=Path(temporary);log=(root/'host.log').open('w');children=[]
        with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
        token='tray-'+40*'3';token_path=root/'token';token_path.write_text(token);token_path.chmod(0o600)
        def source(*args):
            child=subprocess.Popen(['/usr/bin/python3','tests/tray_fixture.py','--log',str(root/'calls.jsonl'),*args],cwd=ROOT,env=env,stdout=subprocess.PIPE,stderr=log,text=True)
            children.append(child);assert child.stdout.readline().strip()=='ready';return child
        def calls():return [json.loads(line) for line in (root/'calls.jsonl').read_text().splitlines()]
        def rpc(method,*args):
            subprocess.run(['gdbus','call','--session','--dest','org.waybridge.TrayFixture','--object-path','/Control','--method','org.waybridge.TrayFixture.'+method,*args],env=env,stdout=subprocess.DEVNULL,check=True)
        try:
            existing=source('--watcher')
            watcher_owner_before=subprocess.check_output(['gdbus','call','--session','--dest','org.freedesktop.DBus','--object-path','/org/freedesktop/DBus','--method','org.freedesktop.DBus.GetNameOwner','org.kde.StatusNotifierWatcher'],env=env,text=True)
            host=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--listen',f'127.0.0.1:{port}','--socket','tray-0','--token-file',str(token_path),'--no-audio','--codec','png'],env=env,stdout=log,stderr=log);children.append(host)
            deadline=time.monotonic()+20
            while not (Path(env['XDG_RUNTIME_DIR'])/'tray-0').exists():
                assert host.poll() is None,(root/'host.log').read_text();assert time.monotonic()<deadline;time.sleep(.05)
            viewer=TrayViewer(port,token)
            status=viewer.control('tray_status');assert status['available'] and status['mode']=='existing_watcher_host',status
            app=source()
            snap=viewer.control('tray_snapshot',lambda s:len(s['items'])==1);item=snap['items'][0]
            assert 'png' not in item
            icon=viewer.control('tray_icon');assert icon['id']==item['id']
            image=Image.open(io.BytesIO(base64.b64decode(icon['png'])));assert image.size==(128,128);assert image.getpixel((64,64))==(255,0,0,255)
            def action(verb,**args):viewer.send(type='tray_action',id=item['id'],epoch=snap['epoch'],generation=item['generation'],action=verb,**args)
            action('activate');action('secondary');action('scroll',delta=120,orientation='vertical');action('menu')
            menu=viewer.control('tray_menu');nodes=menu['root']['children'];assert nodes[1]['enabled']==False and nodes[2]['checked'] and nodes[3]['children'][0]['toggle']=='radio'
            for forbidden in [2,9,11]:
                action('menu_event',node=forbidden,revision=menu['revision'],menu_token=menu['menu_token']);assert 'disabled' in viewer.control('tray_error')['message']
            action('menu_event',node=5,revision=menu['revision'],menu_token=menu['menu_token'])
            time.sleep(.25);assert any(v['method']=='Event' and v['args'][0]==5 for v in calls()),calls()
            assert all(any(v['method']==method for v in calls()) for method in ['Activate','SecondaryActivate','Scroll','AboutToShow'])
            rpc('SetStatus','NeedsAttention');attention=viewer.control('tray_snapshot',lambda s:s['items'][0]['status']=='NeedsAttention')
            assert attention['items'][0]['icon_revision']==item['icon_revision'] and 'png' not in attention['items'][0]
            viewer.send(type='ping',nonce=909)
            while True:
                kind,value=viewer.receive()
                assert kind!='control' or value['type']!='tray_icon','unchanged icons were retransmitted for status only'
                if kind=='control' and value['type']=='pong' and value['nonce']==909:break
            action('menu');menu=viewer.control('tray_menu');rpc('BumpRevision');action('menu_event',node=6,revision=menu['revision'],menu_token=menu['menu_token']);assert 'changed' in viewer.control('tray_error')['message']
            # Read-only must be checked server-side, independent of the Windows UI.
            before=len([v for v in calls() if v['method']=='Activate'])
            viewer.send(type='stream_config',request_id=1,fps=60,bitrate=30000,codec='png',read_only=True)
            viewer.control('stream_config');action('activate');viewer.send(type='ping',nonce=11);viewer.control('pong');time.sleep(.25)
            assert len([v for v in calls() if v['method']=='Activate'])==before
            viewer.send(type='stream_config',request_id=2,fps=60,bitrate=30000,codec='png',read_only=False);viewer.control('stream_config')
            # Revoke input while the worker is inside a real application's blocking
            # revision RPC. It must check again before Event(clicked), not just before the RPC.
            action('menu');delayed_menu=viewer.control('tray_menu');rpc('SetLayoutDelay','350')
            previous_layouts=len([v for v in calls() if v['method']=='GetLayout' and v['args'][1]==0])
            previous_events=len([v for v in calls() if v['method']=='Event'])
            action('menu_event',node=5,revision=delayed_menu['revision'],menu_token=delayed_menu['menu_token'])
            deadline=time.monotonic()+2
            while len([v for v in calls() if v['method']=='GetLayout' and v['args'][1]==0])==previous_layouts:
                assert time.monotonic()<deadline;time.sleep(.01)
            viewer.send(type='stream_config',request_id=3,fps=60,bitrate=30000,codec='png',read_only=True);viewer.control('stream_config')
            assert 'superseded' in viewer.control('tray_error')['message']
            assert len([v for v in calls() if v['method']=='Event'])==previous_events
            rpc('SetLayoutDelay','0')
            viewer.send(type='stream_config',request_id=4,fps=60,bitrate=30000,codec='png',read_only=False);viewer.control('stream_config')
            replacement=source('--replace','--freedesktop')
            action('activate');assert 'owner' in viewer.control('tray_error')['message']
            fresh=viewer.control('tray_snapshot',lambda s:s['items'][0]['generation']!=item['generation']);assert fresh['items'][0]['id']==item['id']
            old_epoch=snap['epoch'];viewer.sock.close();time.sleep(.3);viewer=TrayViewer(port,token)
            viewer.control('tray_status');current=viewer.control('tray_snapshot',lambda s:len(s['items'])==1);assert current['epoch']!=old_epoch
            viewer.send(type='tray_action',id=item['id'],epoch=old_epoch,generation=item['generation'],action='activate');assert 'stale' in viewer.control('tray_error')['message']
            # Existing watcher owner remains untouched. Then remove it and exercise our standard watcher.
            owner=subprocess.check_output(['gdbus','call','--session','--dest','org.freedesktop.DBus','--object-path','/org/freedesktop/DBus','--method','org.freedesktop.DBus.GetNameOwner','org.kde.StatusNotifierWatcher'],env=env,text=True)
            assert owner==watcher_owner_before,'existing watcher owner was replaced'
            existing.terminate();existing.wait(timeout=3);time.sleep(1)
            viewer.send(type='tray_refresh');status=viewer.control('tray_status',lambda s:s.get('mode')=='watcher_host');assert status['available']
            app2=source('--replace');viewer.control('tray_snapshot',lambda s:len(s['items'])==1)
            # ContextMenu-only is allowed only when an exact app-id has a remote window.
            context_source=source('--replace','--context-only')
            context=viewer.control('tray_snapshot',lambda s:len(s['items'])==1 and not s['items'][0]['menu']);ci=context['items'][0]
            def context_menu():viewer.send(type='tray_action',epoch=context['epoch'],id=ci['id'],generation=ci['generation'],action='menu')
            context_menu();assert 'matching remote window' in viewer.control('tray_error')['message']
            subprocess.run(['bash','tests/build-fixture.sh'],cwd=ROOT,check=True)
            window_app=subprocess.Popen([str(ROOT/'build/tests/test-app')],env={**env,'WAYBRIDGE_TEST_APP_ID':'org.waybridge.TrayFixture'},stdout=log,stderr=log);children.append(window_app)
            viewer.control('window');time.sleep(.1);context_menu()
            assert viewer.control('tray_menu')['fallback']=='remote_window'
            assert any(v['method']=='ContextMenu' for v in calls())
            window_app.terminate();window_app.wait(timeout=5)
            # A real Qt application must work without a bespoke Waybridge adapter.
            if os.path.isfile('/usr/bin/vlc'):
                vlc=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--launch','--socket','tray-0','--','vlc','-vv','--intf','qt','--qt-start-minimized','--no-qt-privacy-ask','--no-one-instance','--no-audio'],
                    env={**env,'QT_QPA_PLATFORM':'xcb','LANG':'C.UTF-8'},stdout=log,stderr=log)
                children.append(vlc)
                vlc_snapshot=viewer.control('tray_snapshot',lambda s:any('vlc' in (i['app_id']+' '+i['title']).lower() for i in s['items']))
                vlc_item=next(i for i in vlc_snapshot['items'] if 'vlc' in (i['app_id']+' '+i['title']).lower())
                viewer.send(type='tray_action',epoch=vlc_snapshot['epoch'],id=vlc_item['id'],generation=vlc_item['generation'],action='activate')
                viewer.control('window')
                viewer.send(type='tray_action',epoch=vlc_snapshot['epoch'],id=vlc_item['id'],generation=vlc_item['generation'],action='menu')
                vlc_menu=viewer.control('tray_menu')
                def leaves(node):
                    yield node
                    for child in node.get('children',[]):yield from leaves(child)
                quit_item=next(n for n in leaves(vlc_menu['root']) if n.get('label','').replace('&','').replace('_','').lower()=='quit')
                viewer.send(type='tray_action',epoch=vlc_snapshot['epoch'],id=vlc_item['id'],generation=vlc_item['generation'],action='menu_event',
                    node=quit_item['id'],revision=vlc_menu['revision'],menu_token=vlc_menu['menu_token'])
                vlc.wait(timeout=10);assert vlc.returncode==0
                print('PASS: actual VLC SNI background item, restore to remote window, native DBusMenu Quit')
            viewer.sock.close();time.sleep(.3);viewer=TrayViewer(port,token,enabled=False)
            before=len([v for v in calls() if v['method']=='ContextMenu'])
            context_menu();viewer.send(type='ping',nonce=42);viewer.control('pong');time.sleep(.2)
            assert len([v for v in calls() if v['method']=='ContextMenu'])==before
            print('PASS: private D-Bus existing watcher, own watcher, ARGB icon, nested/check/disabled menus, actions, read-only, owner/reconnect invalidation')
        finally:
            for child in reversed(children):
                if child.poll() is None:child.terminate()
            for child in children:
                try:child.wait(timeout=8)
                except subprocess.TimeoutExpired:child.kill();child.wait()
            log.close()
            if os.environ.get('WAYBRIDGE_TRAY_KEEP_LOG'):print((root/'host.log').read_text())
if __name__=='__main__':main()
