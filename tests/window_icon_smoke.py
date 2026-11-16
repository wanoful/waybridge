#!/usr/bin/env python3
"""Real xdg-toplevel-icon buffers/names/commit and EWMH/WM_CLASS icon sources."""
import base64
import io
import os
from pathlib import Path
import re
import queue
import threading
import shutil
import socket
import subprocess
import tempfile
import time
from PIL import Image
from app_icon_smoke import fixtures, wait, IconViewer
from smoke import ROOT


def build():
    subprocess.run(['bash','tests/build-fixture.sh'],cwd=ROOT,check=True)
    protocol='/usr/share/wayland-protocols/staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml'
    for mode, name in [('client-header','xdg-toplevel-icon-v1-client-protocol.h'),('private-code','xdg-toplevel-icon-v1-protocol.c')]:
        subprocess.run(['wayland-scanner',mode,protocol,str(ROOT/'build/tests'/name)],check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','wayland-client','x11'],text=True).split()
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Ibuild/tests','tests/window_icon_fixture.c','build/tests/xdg-shell-protocol.c','build/tests/xdg-toplevel-icon-v1-protocol.c','-o','build/tests/window-icon-fixture',*flags],cwd=ROOT,check=True)


def pixel(message):
    assert message['png'] is not None,message
    image=Image.open(io.BytesIO(base64.b64decode(message['png']))).convert('RGBA')
    assert image.size==(128,128)
    return image.getpixel((64,64))


def main():
    build()
    with tempfile.TemporaryDirectory(prefix='waybridge-runtime-icon-') as directory:
        root=Path(directory); env=fixtures(directory);env.update(XDG_RUNTIME_DIR=directory,WAYLAND_DISPLAY='runtime-icons-0',GVFS_DISABLE_FUSE='1')
        applications=root/'data/applications'; icons=root/'data/icons/Test/128x128/apps'
        (applications/'fixture-runtime-class.desktop').write_text('[Desktop Entry]\nIcon=bridge-pixmap\n')
        (applications/'some-launcher.desktop').write_text('[Desktop Entry]\nIcon=bridge-png\nStartupWMClass=fixture-runtime-class\n')
        (root/'system/applications/aaa-system-match.desktop').write_text('[Desktop Entry]\nIcon=bridge-inherit\nStartupWMClass=fixture-runtime-class\n')
        (applications/'fixture-shadow.desktop').write_text('[Desktop Entry]\nHidden=true\n')
        (root/'system/applications/fixture-shadow.desktop').write_text('[Desktop Entry]\nIcon=bridge-inherit\nStartupWMClass=fixture-shadow\n')
        (applications/'native-alias-launcher.desktop').write_text('[Desktop Entry]\nIcon=bridge-png\nStartupWMClass=fixture-native-alias\n')
        (applications/'instance-launcher.desktop').write_text('[Desktop Entry]\nIcon=bridge-inherit\nStartupWMClass=fixture-instance-match\n')
        (applications/'case-desktop.desktop').write_text('[Desktop Entry]\nIcon=bridge-pixmap\n')
        Image.new('RGBA',(128,128),(0,255,255,255)).save(icons/'bridge-pixmap.png')
        token='runtime-icons-'+40*'a'; (root/'token').write_text(token);(root/'token').chmod(0o600)
        with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
        processes=[];viewer=None
        with (root/'log').open('w+') as log:
            try:
                host=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--xwayland','--no-audio','--codec','png','--socket','runtime-icons-0','--listen',f'127.0.0.1:{port}','--token-file',str(root/'token')],env=env,stdout=log,stderr=log);processes.append(host)
                wait(lambda:(root/'runtime-icons-0').exists())
                viewer=IconViewer(port,token)
                def launch(mode):
                    application_env=env.copy()
                    if mode.startswith('x11'):
                        wait(lambda:re.search(r'private rootless Xwayland ready display=":(\d+)"',(root/'log').read_text()))
                        number=re.search(r'private rootless Xwayland ready display=":(\d+)"',(root/'log').read_text())[1]
                        application_env['DISPLAY']=':'+number
                    app=subprocess.Popen([str(ROOT/'build/tests/window-icon-fixture'),mode],env=application_env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=log,text=True);processes.append(app)
                    app.lines=queue.Queue()
                    def collect():
                        for line in app.stdout: app.lines.put(line.rstrip('\n'))
                        app.lines.put(None)
                    threading.Thread(target=collect,daemon=True).start()
                    assert app.lines.get(timeout=10)=='READY'
                    info=viewer.until(lambda k,v:k=='control' and v['type']=='window' and v.get('title')==('X11' if mode.startswith('x11') else 'Wayland')+' runtime icon fixture')
                    assert pixel(viewer.icon(info['id'],info['icon_revision']))==((0,255,0,255) if mode.startswith('x11-') else (255,0,0,255))
                    return app,info['id'],info['icon_revision']
                def command(app,value):
                    app.stdin.write(value+'\n');app.stdin.flush()
                    result=app.lines.get(timeout=10)
                    assert result=='ACK '+value,(value,result,app.poll())
                def changed(app,id,revision,value,expected):
                    command(app,value)
                    info=viewer.until(lambda k,v:k=='control' and v['type']=='window' and v['id']==id and v['icon_revision']>revision)
                    icon=viewer.icon(id,info['icon_revision'])
                    if expected is None: assert icon['png'] is None,icon
                    else: assert pixel(icon)==expected,(value,pixel(icon))
                    return info['icon_revision']
                app,id,revision=launch('wayland')
                command(app,'stage')
                viewer.send(type='ping',nonce='stage')
                while True:
                    kind,value=viewer.receive()
                    assert kind!='control' or value['type']!='window_icon','pending icon applied before commit'
                    if kind=='control' and value.get('type')=='pong':break
                revision=changed(app,id,revision,'commit',(0,255,0,255))
                for value,expected in [('blue',(0,0,255,255)),('alpha',(128,0,0,128)),('named',(0,0,255,255)),('both',(0,255,0,255)),('clear',(255,0,0,255)),('huge',(255,0,0,255)),('empty',(255,0,0,255)),('identity',(255,0,0,255)),('caseidentity',(0,255,255,255)),('canonicalidentity',(0,255,255,255))]:
                    revision=changed(app,id,revision,value,expected)
                command(app,'slow');wait(lambda:(root/'converter.started').exists())
                command(app,'green')
                info=viewer.until(lambda k,v:k=='control' and v['type']=='window' and v['id']==id and v['app_id']=='org.waybridge.Slow')
                while True:
                    kind,value=viewer.receive()
                    if kind=='control' and value['type']=='window_icon' and value['id']==id:
                        assert pixel(value)==(0,255,0,255),'stale desktop result overwrote runtime icon';break
                viewer.sock.close();time.sleep(.15);viewer=IconViewer(port,token)
                info=viewer.until(lambda k,v:k=='control' and v['type']=='window' and v['id']==id)
                assert pixel(viewer.icon(id,info['icon_revision']))==(0,255,0,255)
                app.stdin.write('quit\n');app.stdin.flush();assert app.wait(timeout=5)==0
                viewer.until(lambda k,v:k=='control' and v['type']=='window_closed' and v['id']==id)
                app,id,revision=launch('x11')
                app.stdin.write('stall\n');app.stdin.flush()
                assert app.lines.get(timeout=10)=='STALLED'
                stalled_info=None;stalled_icon=None
                start=time.monotonic();viewer.send(type='ping',nonce='x11-icon-stall')
                while True:
                    kind,value=viewer.receive()
                    if kind=='control' and value['type']=='window' and value['id']==id and value['icon_revision']>revision:stalled_info=value
                    if kind=='control' and value['type']=='window_icon' and value['id']==id:stalled_icon=value
                    if kind=='control' and value.get('type')=='pong' and value.get('nonce')=='x11-icon-stall':break
                assert time.monotonic()-start<.5,'icon query blocked compositor on a grabbed X server'
                assert app.lines.get(timeout=10)=='ACK stall'
                if stalled_info is None:stalled_info=viewer.until(lambda k,v:k=='control' and v['type']=='window' and v['id']==id and v['icon_revision']>revision)
                revision=stalled_info['icon_revision']
                if stalled_icon is None or stalled_icon['revision']!=revision:stalled_icon=viewer.icon(id,revision)
                assert pixel(stalled_icon) in [(255,0,0,255),(0,255,0,255)] # Xwayland may defer property events until ungrab.
                for value,expected in [('green',(0,255,0,255)),('blue',(0,0,255,255)),('bad',(255,0,0,255)),('huge',(255,0,0,255)),('clear',(255,0,0,255)),('instance',(0,255,0,255)),('case',(0,255,255,255)),('shadow',None)]:
                    revision=changed(app,id,revision,value,expected)
                app.stdin.write('quit\n');app.stdin.flush();assert app.wait(timeout=5)==0
                viewer.until(lambda k,v:k=='control' and v['type']=='window_closed' and v['id']==id)
                app,id,revision=launch('x11-initial')
                revision=changed(app,id,revision,'clear',(255,0,0,255))
                app.stdin.write('quit\n');app.stdin.flush();assert app.wait(timeout=5)==0
                viewer.until(lambda k,v:k=='control' and v['type']=='window_closed' and v['id']==id)
                app,id,revision=launch('x11-empty')
                revision=changed(app,id,revision,'clear',None)
                app.stdin.write('quit\n');app.stdin.flush();assert app.wait(timeout=5)==0
                viewer.until(lambda k,v:k=='control' and v['type']=='window_closed' and v['id']==id)
                app,id,revision=launch('wayland')
                app.stdin.write('bad-destroy\n');app.stdin.flush();assert app.wait(timeout=5)==0
                viewer.until(lambda k,v:k=='control' and v['type']=='window_closed' and v['id']==id)
                assert host.poll() is None,'malformed icon client stopped compositor'
                print('PASS: real Wayland runtime buffer/name/priority/alpha/commit/destroy/reset/size limits, stale desktop rejection/reconnect; real X11 EWMH updates/deletion/bad/oversized fallback and WM_CLASS class/instance/case desktop matching')
            except BaseException:
                print((root/'log').read_text());raise
            finally:
                if viewer:viewer.sock.close()
                for process in reversed(processes):
                    if process.poll() is None:process.terminate();process.wait(timeout=10)
                # The private bus can activate GVFS with this shorter runtime;
                # detach only this test-owned mount before removing its directory.
                unmount=shutil.which('fusermount3') or shutil.which('fusermount')
                if unmount and (root/'gvfs').is_mount():
                    subprocess.run([unmount,'-u','-z','--',str(root/'gvfs')],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)


if __name__=='__main__':main()
