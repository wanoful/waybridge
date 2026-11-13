#!/usr/bin/env python3
"""Real scRGB FP16 GL -> CUDA P010 -> NVENC Main10 streams, metadata and alpha."""
import io,json,math,os,select,socket,struct,subprocess,tempfile,time
from pathlib import Path
import numpy as np
from PIL import Image
from hardware_smoke import ROOT,Viewer
from codecs_smoke import wait_socket

class HdrViewer(Viewer):
    hdr_hello=True
    def send(self,**value):
        if value.get('type')=='hello' and self.hdr_hello:value.update(hdr=True,color_spaces=['bt2020_pq','bt709_sdr'])
        super().send(**value)
    def next(self,kind,ack=True):
        while True:
            k,v=self.receive()
            if k==0:
                if v.get('type')=='window':
                    if not hasattr(self,'activated'):self.activated=set()
                    if v['id'] not in self.activated:self.activated.add(v['id']);self.send(type='color_mode',id=v['id'],hdr=True)
                continue
            id,seq=struct.unpack('>I',v[:4])[0],struct.unpack('>Q',v[12:20])[0]
            if k!=kind or ack:self.send(type='frame_ack',id=id,sequence=seq)
            if k==kind:return v

def parse(body,kind):
    id,w,h,seq,lw,lh,x,y,epoch,cw,ch,key,color,alpha=struct.unpack('>IIIQiiiiIIIIII',body[:60])
    assert len(body)==60+color+alpha
    mask=np.asarray(Image.open(io.BytesIO(body[60+color:]))) if alpha else np.full((h,w),0x3c00)
    assert mask.dtype.itemsize>=2,'HDR alpha must retain raw FP16 bits in a 16-bit PNG'
    assert tuple(int(mask[h//2,x]) for x in [w//6,w//2,w*5//6])==(0x3c00,0x3800,0),np.unique(mask)
    return dict(kind=kind,id=id,w=w,h=h,seq=seq,epoch=epoch,cw=cw,ch=ch,key=key,color=body[60:60+color])

def decode(frames,root):
    codec='hevc' if frames[0]['kind']==7 else 'obu'
    bitstream=b''.join(f['color'] for f in frames)
    filename=root/(f'hdr-{frames[0]["kind"]}-{frames[0]["w"]}x{frames[0]["h"]}.'+codec);filename.write_bytes(bitstream)
    probe=json.loads(subprocess.run(['ffprobe','-v','error','-show_streams','-of','json',str(filename)],capture_output=True,check=True).stdout)['streams'][0]
    assert probe['pix_fmt']=='yuv420p10le' and probe['color_transfer']=='smpte2084' and probe['color_primaries']=='bt2020' and probe['color_space']=='bt2020nc',probe
    data=subprocess.run(['ffmpeg','-v','error','-xerror','-f',codec,'-i','pipe:0','-pix_fmt','yuv420p10le','-fps_mode','passthrough','-f','rawvideo','pipe:1'],input=bitstream,capture_output=True,check=True).stdout
    w,h=frames[0]['cw'],frames[0]['ch'];length=w*h*3
    assert len(data)==len(frames)*length,(len(data),len(frames),length)
    row=np.frombuffer(data[-length:],dtype='<u2',count=w*h).reshape(h,w)[h//2,:frames[-1]['w']*2//3]
    assert len(np.unique(row))>min(256,len(row)*.6),(len(np.unique(row)),row.min(),row.max())
    def pq(c):
        p=(max(c,0)*.008)**(2610/16384)
        return ((3424/4096+2413/128*p)/(1+2392/128*p))**(2523/32)
    for x in [w//6,w//2]:
        expected=round(64+876*pq(100*x/(frames[-1]['w']-1)))
        assert abs(int(row[x])-expected)<=3,(x,int(row[x]),expected)
    return dict(frames=len(frames),bits=10,distinct_luma=int(len(np.unique(row))),color=probe['color_space'])

def run(codec,kind,mode="chain"):
    small=bool(os.getenv('WAYBRIDGE_HDR_SMALL'));initial=(256,256) if small else (960,640);odd=(257,257) if small else (641,481)
    log_path=ROOT/f'build/tests/hdr-video-{kind}-{mode}-{initial[0]}x{initial[1]}.log'
    with tempfile.TemporaryDirectory(prefix='waybridge-hdr-video-') as runtime,open(log_path,'w') as log:
        env={**os.environ,'XDG_RUNTIME_DIR':runtime,'WAYLAND_DISPLAY':'hdr-0','WAYBRIDGE_HDR_RAMP':'1'}
        with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
        token='hdr-test-'+'f'*40;file=Path(runtime)/'token';file.write_text(token);file.chmod(0o600)
        host_env=dict(env)
        if mode=='fault':host_env['LD_PRELOAD']=str(ROOT/'build/tests/gpu-fault.so')
        host=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--listen',f'127.0.0.1:{port}','--socket','hdr-0','--token-file',str(file),'--codec',codec,'--hdr','experimental-scrgb','--stats','--bitrate','50000'],env=host_env,stdout=log,stderr=log)
        app=viewer=None
        try:
            wait_socket(host,Path(runtime)/'hdr-0')
            viewer_class=type('NoHdrHello',(HdrViewer,),{'hdr_hello':False}) if mode=='unnegotiated' else HdrViewer
            viewer=viewer_class(port,token,'hevc_main10_pq_alpha16' if kind==7 else 'av1_main10_pq_alpha16')
            app=subprocess.Popen([str(ROOT/'build/tests/hdr-app')],env=env,stdin=subprocess.PIPE,text=True,stdout=log,stderr=log)
            if mode=='unnegotiated':
                png=viewer.next(1)
                viewer.send(type='color_mode',id=struct.unpack('>I',png[:4])[0],hdr=True)
                viewer.send(type='refresh');png=viewer.next(1)
                assert Image.open(io.BytesIO(png[36:])).size==initial
                print('PASS: Main10 capability without explicit HDR/PQ hello never enables HDR, even with forged color_mode=true')
                return
            if mode=='fault':
                first=parse(viewer.next(kind),kind)
                for _ in range(2):app.stdin.write('frame\n');app.stdin.flush();parse(viewer.next(kind),kind)
                app.stdin.write('frame\n');app.stdin.flush();png=viewer.next(1)
                pixel=Image.open(io.BytesIO(png[36:])).convert('RGBA').getpixel((initial[0]//2,initial[1]//2))
                assert pixel[3]==128 and max(pixel[:3])-min(pixel[:3])<=1 and pixel[0]>250,pixel
                # GPU rejection requests a fresh CPU capture even when source goes idle.
                again=viewer.next(1);assert Image.open(io.BytesIO(again[36:])).convert('RGBA').getpixel((initial[0]//2,initial[1]//2))==pixel
                assert 'TEST injected fourth CUDA-frame rejection' in log_path.read_text()
                print('PASS: real HDR GPU NVENC rejection retains accurately tonemapped current PNG/color/alpha and refreshes CPU without application repaint')
                return
            if mode=='policy':
                first=parse(viewer.next(kind),kind)
                explicit='hevc' if kind==7 else 'av1'
                viewer.send(type='stream_config',request_id=1,codec=explicit,hdr=False,fps=60,bitrate=50000)
                png=viewer.next(1)
                assert Image.open(io.BytesIO(png[36:])).convert('RGBA').getpixel((initial[0]//2,initial[1]//2))[3]==128
                # A client monitor request cannot override the global preference.
                viewer.send(type='color_mode',id=first['id'],hdr=True)
                viewer.next(1)
                viewer.send(type='stream_config',request_id=2,codec=explicit,hdr=True,fps=60,bitrate=50000)
                resumed=parse(viewer.next(kind),kind)
                assert resumed['key'] and resumed['epoch']!=first['epoch']
                print('PASS: HDR-only live policy changes invalidate capture, enforce SDR despite color_mode=true, then resume HDR with a fresh keyframe epoch')
                return
            frames=[]
            for i in range(12):
                if i:app.stdin.write('frame\n');app.stdin.flush()
                frames.append(parse(viewer.next(kind),kind))
            assert frames[0]['key'] and len({f['epoch'] for f in frames})==1 and any(not f['key'] for f in frames[1:])
            assert all(a['seq']<b['seq'] for a,b in zip(frames,frames[1:]))
            # Neither dependent access unit may be dropped while waiting for
            # the exact same two presentation credits used by SDR video.
            held=[]
            for _ in range(2):app.stdin.write('frame\n');app.stdin.flush();held.append(parse(viewer.next(kind,False),kind))
            app.stdin.write('frame\n');app.stdin.flush();assert not select.select([viewer.media],[],[],.15)[0],'HDR exceeded two credits'
            viewer.send(type='ping',nonce=12345);viewer.until(lambda k,v:k==0 and v.get('type')=='pong' and v.get('nonce')==12345)
            viewer.send(type='frame_ack',id=held[-1]['id'],sequence=held[-1]['seq']);held.append(parse(viewer.next(kind),kind));frames+=held
            artifact=ROOT/'build/diagnostics';artifact.mkdir(exist_ok=True)
            report=decode(frames,artifact)
            def sized(w,h):
                while True:
                    frame=parse(viewer.next(kind),kind)
                    if (frame['w'],frame['h'])==(w,h):return frame
            app.stdin.write(f'size {odd[0]} {odd[1]}\n');app.stdin.flush()
            resized=sized(*odd);assert (resized['w'],resized['h'])==odd and resized['key'] and resized['epoch']!=frames[-1]['epoch']
            viewer.send(type='codec_fallback',id=resized['id'],codec='av1_main10_pq_alpha16' if kind==7 else 'hevc_main10_pq_alpha16')
            viewer.send(type='refresh');still=parse(viewer.next(kind),kind);assert still['epoch']==resized['epoch'] and not still['key']
            # Revisit the old size twice: codec-specific idle cache must force
            # a new epoch/IDR even when its old reference chain remains allocated.
            for w,h in [initial,odd]:
                app.stdin.write(f'size {w} {h}\n');app.stdin.flush();warm=sized(w,h)
                assert (warm['w'],warm['h'])==(w,h) and warm['key'] and warm['epoch']!=resized['epoch'],(warm,resized)
                resized=warm
            # An SDR monitor change must stop Main10 and produce correctly tonemapped RGBA.
            viewer.send(type='color_mode',id=frames[0]['id'],hdr=False)
            png=viewer.next(1);image=Image.open(io.BytesIO(png[36:])).convert('RGBA')
            assert image.size==odd and image.getpixel((odd[0]//2,odd[1]//2))[3]==128
            assert all(240<=v<=255 for v in image.getpixel((odd[0]//2,odd[1]//2))[:3])
            viewer.send(type='color_mode',id=resized['id'],hdr=True)
            resumed=parse(viewer.next(kind),kind);assert resumed['key'] and resumed['epoch']!=resized['epoch'],'PNG reset requires a fresh HDR epoch'
            assert host.poll() is None and app.poll() is None
            result={'codec':codec,**report,'width':initial[0],'height':initial[1],'resize_epoch':resized['epoch'],'two_credits':True,'cached_resize':True,'PNG_to_HDR_key_epoch':True,'monitor_fallback':'SDR PNG'}
            (artifact/f'hdr-report-{kind}-{initial[0]}x{initial[1]}.json').write_text(json.dumps(result,indent=2)+'\n')
            print(json.dumps(result))
        finally:
            if viewer:viewer.media.close();viewer.control.close()
            if app:app.terminate();app.wait(timeout=5)
            host.terminate();host.wait(timeout=5)

def native():
    cuda=Path(os.environ.get('WAYBRIDGE_CUDA_ROOT','/opt/cuda'))
    flags=subprocess.check_output(['pkg-config','--cflags','--libs','egl','glesv2','libavutil'],text=True).split()
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-DWB_CUDA_INTEROP=1',f'-I{cuda}/include','tests/hdr_native_smoke.c','native/gpu_interop.c','-o','build/tests/hdr-native-smoke',*flags,'-ldl','-lm'],cwd=ROOT,check=True)
    subprocess.run([str(ROOT/'build/tests/hdr-native-smoke')],cwd=ROOT,check=True)
    flags=subprocess.check_output(['pkg-config','--cflags','libavcodec','libavutil'],text=True).split()
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC','tests/gpu_fault_shim.c','-o','build/tests/gpu-fault.so',*flags,'-ldl'],cwd=ROOT,check=True)

if __name__=='__main__':
    subprocess.run(['bash','tests/build-hdr-fixture.sh'],cwd=ROOT,check=True)
    native()
    run('hevc-nvenc',7);run('av1-nvenc',8)
    run('hevc-nvenc',7,'unnegotiated')
    run('hevc-nvenc',7,'policy')
    run('hevc-nvenc',7,'fault')
