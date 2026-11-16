#!/usr/bin/env python3
"""Production compositor, real Qt Wayland/XCB sources and native MIME targets."""
import argparse,os,re,socket,subprocess,sys,tempfile,time
from pathlib import Path
from smoke import ROOT,Viewer
from drag_out_smoke import wait_for

class MixedViewer(Viewer):
    def __init__(self,port,token,**unused):
        self.sock=socket.create_connection(('127.0.0.1',port),timeout=10);self.geometry={};self.popups={}
        self.send(type='hello',version=3,token=token,dpi=96,window_semantics_v1=True,frame_ack=True,file_drop_out=True,data_drag_v1=True,toplevel_drag_v1=True)
        assert self.receive()[1]['window_semantics_v1']
    def receive(self):
        kind,value=super().receive()
        if kind=='frame':self.send(type='frame_ack',id=value[0],sequence=value[3])
        elif value.get('type')=='popup':self.popups[value['id']]=value
        return kind,value

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--large',action='store_true');parser.add_argument('--cancel-only',action='store_true');args=parser.parse_args()
    exe=ROOT/('build/tests/mixed-drag-widgets'+('-large' if args.large else ''))
    subprocess.run(['c++','-std=c++17','-fPIC','-DWAYBRIDGE_DND_XCB_TRACE','tests/drag_widgets.cpp','-o',str(exe),*subprocess.check_output(['pkg-config','--cflags','--libs','Qt6Widgets','xcb'],text=True).split()],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='wb-mixed-dnd-') as directory:
        root=Path(directory);token=root/'token';token.write_text('mixed-dnd-'+40*'7');token.chmod(0o600)
        env={**os.environ,'XDG_RUNTIME_DIR':directory,'WAYLAND_DISPLAY':'mixed-0','GVFS_DISABLE_FUSE':'1','QT_WAYLAND_DISABLE_WINDOWDECORATION':'1','RUST_LOG':'waybridge=debug,smithay::xwayland::xwm::dnd=trace'}
        if args.large:
            payload=root/'text';payload.write_text('拖放 λ 😀 0123456789\n'*10000)
            env['WAYBRIDGE_DND_FIXTURE_TEXT']=str(payload)
        with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
        logs={side:(root/(side+'.log')).open('w') for side in ['host','W','X']};children=[];viewer=None
        try:
            children.append(subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--socket','mixed-0','--listen',f'127.0.0.1:{port}','--token-file',str(token),'--codec','png','--no-audio'],env=env,stdout=logs['host'],stderr=logs['host']))
            wait_for(lambda:(root/'mixed-0.session.json').exists())
            viewer=MixedViewer(port,token.read_text(),file_drop_out=True,data_drag_v1=True)
            viewer.send(type='hello',file_drop_out=True,data_drag_v1=True,toplevel_drag_v1=True)
            for side,backend in [('W','wayland'),('X','xcb')]:
                files=root/side;(files/'folder').mkdir(parents=True);(files/'source.txt').write_text('source '+side)
                children.append(subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--socket','mixed-0','--launch','--','env','QT_QPA_PLATFORM='+backend,'WAYBRIDGE_DND_FIXTURE_SIDE=-'+side,'WAYBRIDGE_DND_FIXTURE_MOVE=1',str(exe),str(files)],env=env,stdout=logs[side],stderr=logs[side]))
            ids={};infos={}
            while len(ids)<4:
                info=viewer.until(lambda k,v:k=='control' and v.get('type')=='window' and v.get('title','').startswith('Waybridge drag ') and v['title'] not in ids)
                ids[info['title']]=info['id'];infos[info['title']]=info
            def ident(side,kind):return ids['Waybridge drag '+kind+'-'+side]
            def text(side):return (root/(side+'.log')).read_text()
            def motion(ident,x,y):viewer.send(type='pointer',id=ident,x=x,y=y)
            def button(ident,down):viewer.send(type='button',id=ident,button=272,down=down)
            for src,dst in ([] if args.cancel_only else [('W','X'),('X','W'),('X','X'),('W','W')]):
                if args.large:
                    b=ident(dst,'target');viewer.send(type='focus',id=b,focused=True)
                    for key,down in [(29,True),(30,True),(30,False),(29,False),(14,True),(14,False)]:viewer.send(type='key',id=b,key=key,down=down)
                    time.sleep(.2)
                time.sleep(.6);started=text(src).count('TEXT_DRAG_STARTED');before=text(src).count('TEXT_DRAG_ENDED');finished=text(src).count('XFINISHED ');drops=text(dst).count('OTHER ');actions=len(re.findall(r'DROP_ACTION (\d+) 1',text(dst)))
                a,b=ident(src,'source'),ident(dst,'target')
                motion(a,60,40);button(a,True);time.sleep(.05);motion(a,100,40)
                wait_for(lambda:text(src).count('TEXT_DRAG_STARTED')>started)
                if src=='X':
                    viewer.until(lambda k,v:k=='control' and v.get('type')=='popup' and v.get('input_region',{}).get('layers')==[] and not v.get('input_popup'))
                time.sleep(.15);motion(b,100,120);time.sleep(.4);button(b,False)
                wait_for(lambda:text(src).count('TEXT_DRAG_ENDED')>before,8)
                wait_for(lambda:text(dst).count('OTHER ')>drops,8)
                result=re.findall(r'TEXT_DRAG_ENDED (\d+)',text(src))[-1]
                wait_for(lambda:len(re.findall(r'DROP_ACTION (\d+) 1',text(dst)))>actions,8)
                assert result in ('1','2'),(src,dst,result)
                if src=='X':
                    wait_for(lambda:text(src).count('XFINISHED ')>finished,8)
                    result=re.findall(r'XFINISHED 1 (\d+)',text(src))[-1]
                assert result==re.findall(r'DROP_ACTION (\d+) 1',text(dst))[-1],(src,dst,result)
                if args.large:
                    import hashlib
                    expected=hashlib.sha256(payload.read_bytes()).hexdigest()
                    assert re.findall(r'OTHER_SHA256 (\w+)',text(dst))[-1]==expected
                print(f'PASS: native {src}->{dst} text COPY/MOVE, source and target agree'+(' (Unicode/large SHA256)' if args.large else ''),flush=True)
            if args.large:return
            for src,dst in ([] if args.cancel_only else [('W','X'),('X','W'),('X','X')]):
                source=root/src/'source.txt';source.write_text('native file '+src+'->'+dst)
                saved=root/dst/'folder/source.txt';saved.unlink(missing_ok=True)
                time.sleep(.6)
                wait_for(lambda:re.search(r'ITEM source.txt (\d+) (\d+)',text(src)) and re.search(r'ITEM folder (\d+) (\d+)',text(dst)))
                sx,sy=map(int,re.findall(r'ITEM source.txt (\d+) (\d+)',text(src))[0]);tx,ty=map(int,re.findall(r'ITEM folder (\d+) (\d+)',text(dst))[0])
                a,b=ident(src,'source'),ident(dst,'source')
                motion(a,sx,sy);button(a,True);time.sleep(.1);motion(a,sx+35,sy);time.sleep(.25);motion(b,tx,ty);time.sleep(.4);button(b,False)
                wait_for(saved.exists,8);assert not source.exists() and saved.read_text()=='native file '+src+'->'+dst
                print(f'PASS: native {src}->{dst} QFileSystemModel MOVE retains normal writable files',flush=True)
            # Cancellation must leave no data behind and allow a new gesture.
            for src in ('X','W'):
                for reason in ('escape','readonly','disconnect'):
                    time.sleep(.6);started=text(src).count('TEXT_DRAG_STARTED');ended=text(src).count('TEXT_DRAG_ENDED')
                    dst='W' if src=='X' else 'X';drops=text(dst).count('OTHER ')
                    a,b=ident(src,'source'),ident(dst,'target')
                    motion(a,60,40);button(a,True);time.sleep(.05);motion(a,100,40)
                    wait_for(lambda:text(src).count('TEXT_DRAG_STARTED')>started)
                    motion(b,100,120);time.sleep(.3)
                    if reason=='escape':viewer.send(type='toplevel_drag_cancel')
                    elif reason=='readonly':
                        viewer.send(type='stream_config',request_id=99,codec='png',fps=60,bitrate=30000,read_only=True)
                        ack=viewer.until(lambda k,v:k=='control' and v.get('type')=='stream_config' and v.get('request_id')==99)
                        assert ack['ok'] and ack['read_only']
                    else:viewer.sock.close()
                    wait_for(lambda:text(src).count('TEXT_DRAG_ENDED')>ended,8)
                    time.sleep(.2);assert text(dst).count('OTHER ')==drops,(src,reason,'cancel unexpectedly delivered')
                    if reason=='readonly':
                        viewer.send(type='stream_config',request_id=100,codec='png',fps=60,bitrate=30000,read_only=False)
                        viewer.until(lambda k,v:k=='control' and v.get('type')=='stream_config' and v.get('request_id')==100)
                    elif reason=='disconnect':
                        viewer=MixedViewer(port,token.read_text(),file_drop_out=True,data_drag_v1=True)
                        viewer.send(type='hello',file_drop_out=True,data_drag_v1=True,toplevel_drag_v1=True)
                        viewer.until(lambda k,v:k=='frame')
                    time.sleep(.6);started=text(src).count('TEXT_DRAG_STARTED')
                    motion(a,60,40);button(a,True);time.sleep(.05);motion(a,100,40)
                    wait_for(lambda:text(src).count('TEXT_DRAG_STARTED')>started)
                    motion(b,100,120);time.sleep(.4);button(b,False)
                    wait_for(lambda:text(dst).count('OTHER ')>drops,8)
                    print(f'PASS: native {src} {reason} cancels without delivering, next gesture succeeds',flush=True)
            for src,dst in [('W','X'),('X','W')]:
                time.sleep(.6);started=text(src).count('TEXT_DRAG_STARTED');ended=text(src).count('TEXT_DRAG_ENDED')
                a,b=ident(src,'source'),ident(dst,'target');drops=text(dst).count('OTHER ')
                motion(a,60,40);button(a,True);time.sleep(.05);motion(a,100,40)
                wait_for(lambda:text(src).count('TEXT_DRAG_STARTED')>started)
                motion(b,100,120);time.sleep(.3);viewer.send(type='close',id=b)
                viewer.until(lambda k,v:k=='control' and v.get('type')=='window_closed' and v.get('id')==b)
                button(b,False);wait_for(lambda:text(src).count('TEXT_DRAG_ENDED')>ended,8)
                assert text(dst).count('OTHER ')==drops and children[0].poll() is None
                print(f'PASS: native {src}->{dst} target close cancels without delivery',flush=True)
        except BaseException:
            for side in logs:print('\n'+side+'\n'+(root/(side+'.log')).read_text()[-10000:])
            raise
        finally:
            output=ROOT/('build/tests/mixed-dnd-large' if args.large else 'build/tests/mixed-dnd');output.mkdir(parents=True,exist_ok=True)
            for side in logs:(output/(side+'.log')).write_text((root/(side+'.log')).read_text())
            if viewer:viewer.sock.close()
            for p in reversed(children):
                if p.poll() is None:p.terminate()
            for p in children:
                try:p.wait(timeout=10)
                except subprocess.TimeoutExpired:p.kill();p.wait()
            for f in logs.values():f.close()
if __name__=='__main__':
    if os.environ.get('WAYBRIDGE_TEST_SESSION_FIXTURE')!='1':
        raise SystemExit(subprocess.run([sys.executable,str(ROOT/'tests/session_fixture.py'),sys.executable,__file__,*sys.argv[1:]]).returncode)
    main()
