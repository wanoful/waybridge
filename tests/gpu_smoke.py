#!/usr/bin/env python3
"""Real EGL/CUDA/NVENC, deterministic fault recovery, optional release timings."""
import argparse
import io
import json
import os
from pathlib import Path
import re
import socket
import statistics
import struct
import subprocess
import tempfile
import time
from PIL import Image
from hardware_smoke import ROOT, Viewer, decoded
from codecs_smoke import check_alpha, wait_socket


def native():
    cuda = Path(os.environ.get('WAYBRIDGE_CUDA_ROOT', '/opt/cuda'))
    flags = subprocess.check_output(['pkg-config','--cflags','--libs','egl','glesv2','libavutil','libavcodec'],text=True).split()
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-DWB_CUDA_INTEROP=1',f'-I{cuda}/include','tests/gpu_native_smoke.c','native/gpu_interop.c','native/nvenc.c','-pthread','-o','build/tests/gpu-native-smoke',*flags,'-ldl'],cwd=ROOT,check=True)
    subprocess.run([str(ROOT/'build/tests/gpu-native-smoke')],cwd=ROOT,check=True)
    for kind,extension in ((2,'h264'),(4,'hevc'),(5,'obu')):
        output=subprocess.check_output(['ffmpeg','-hide_banner','-loglevel','error','-f',extension,'-i',str(ROOT/f'build/tests/native-gpu.{extension}'),'-f','rawvideo','-pix_fmt','rgba','pipe:1'])
        assert len(output)==4*642*482*4,(kind,len(output))
    flags=subprocess.check_output(['pkg-config','--cflags','libavcodec','libavutil'],text=True).split()
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-shared','-fPIC','tests/gpu_fault_shim.c','-o','build/tests/gpu-fault.so',*flags,'-ldl'],cwd=ROOT,check=True)


def session(mode,alpha,binary,log_path,task):
    with tempfile.TemporaryDirectory(prefix='waybridge-gpu-') as runtime,log_path.open('w') as log:
        env={**os.environ,'XDG_RUNTIME_DIR':runtime,'WAYLAND_DISPLAY':'gpu-0'}
        env.pop('WAYBRIDGE_FIXTURE_ALPHA',None)
        if alpha:env['WAYBRIDGE_FIXTURE_ALPHA']='1'
        host_env=dict(env)
        if task=='fault':host_env['LD_PRELOAD']=str(ROOT/'build/tests/gpu-fault.so')
        with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
        token='gpu-input-test-'+'d'*40
        file=Path(runtime)/'token';file.write_text(token);file.chmod(0o600)
        host=subprocess.Popen([str(binary),'--listen',f'127.0.0.1:{port}','--socket','gpu-0','--token-file',str(file),'--codec','h264-nvenc','--gpu-input',mode,'--fps','120','--stats'],env=host_env,stdout=log,stderr=log)
        app=viewer=None
        try:
            wait_socket(host,Path(runtime)/'gpu-0')
            viewer=Viewer(port,token)
            app=subprocess.Popen([str(ROOT/'build/tests/test-app')],env=env,stdout=log,stderr=log)
            first=viewer.video()
            if task=='fault':
                chain=[first]
                viewer.send(type='refresh')
                while True:
                    kind,value=viewer.receive()
                    if kind==1:
                        png=value
                        break
                    if kind==2:
                        chain.append(viewer.parse_video(value))
                        viewer.send(type='refresh')
                assert 1<=len(chain)<=3,[(f['seq'],f['epoch']) for f in chain]
                check_alpha(chain)
                image=Image.open(io.BytesIO(png[36:])).convert('RGBA')
                assert image.getpixel((480,320))==(0,255,0,128),image.getpixel((480,320))
                viewer.send(type='frame_ack',id=first['id'],sequence=struct.unpack('>Q',png[12:20])[0])
                recovered=viewer.video()
                assert recovered['key'] and recovered['epoch']!=first['epoch']
                check_alpha([recovered])
                viewer.send(type='refresh');following=viewer.video()
                assert following['epoch']==recovered['epoch'] and not following['key']
                check_alpha([recovered,following])
                text=log_path.read_text()
                assert 'TEST injected fourth CUDA-frame rejection' in text and 'input="cuda"' in text and 'input="cpu"' in text,text
                print('PASS: test-injected CUDA FFmpeg encode rejection after valid references preserves exact PNG alpha/color, triggers a fresh CPU capture without an application repaint, then resumes CPU NVENC with an independent epoch')
                return None
            width,height=task
            viewer.send(type='resize',id=first['id'],width=width,height=height,dpi=96)
            frame=viewer.video()
            while (frame['w'],frame['h'])!=(width,height):frame=viewer.video()
            chain=[frame]
            for _ in range(2):viewer.send(type='refresh');chain.append(viewer.video())
            if alpha:check_alpha(chain)
            else:
                image=decoded(chain)
                red=image.getpixel((width//2,height//4));blue=image.getpixel((width//2,height*3//4))
                assert red[0]>225 and red[2]<25 and blue[2]>225 and blue[0]<25,(red,blue)
            times=[];deadline=time.monotonic()+3.2
            while time.monotonic()<deadline:
                start=time.monotonic();viewer.send(type='refresh');viewer.video();times.append((time.monotonic()-start)*1000)
            time.sleep(.12)
            text=log_path.read_text()
            if mode=='cuda':
                assert 'input="cuda"' in text and 'stage="gpu_capture"' in text and 'GPU readback fallback failed' not in text,text
                assert 'stage="readback"' not in text and 'stage="gpu_fallback_readback"' not in text,text
            else:assert 'input="cpu"' in text and 'stage="readback"' in text,text
            stages={}
            for name,count,average in re.findall(r'stage="([^"]+)" samples=(\d+) avg_ms=([\d.eE+-]+)',text):
                count=int(count);entry=stages.setdefault(name,{'samples':0,'total':0.})
                entry['samples']+=count;entry['total']+=count*float(average)
            stages={name:entry['total']/entry['samples'] for name,entry in stages.items()}
            color=sum(stages.get(name,0) for name in ('readback','gpu_capture','unpremultiply','nvenc'))
            return {'mode':mode,'alpha':alpha,'width':width,'height':height,'frames':len(times),'stage_avg_ms':stages,'color_input_encode_ms':color,'refresh_receive_avg_ms':statistics.mean(times),'refresh_receive_median_ms':statistics.median(times)}
        finally:
            if viewer:viewer.media.close();viewer.control.close()
            if app:app.terminate();app.wait(timeout=5)
            host.terminate();host.wait(timeout=5)


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--benchmark',action='store_true')
    parser.add_argument('--binary',type=Path,default=ROOT/'target/release/waybridge')
    arguments=parser.parse_args()
    subprocess.run(['bash','tests/build-fixture.sh'],cwd=ROOT,check=True)
    native()
    output=ROOT/'build/diagnostics';output.mkdir(parents=True,exist_ok=True)
    session('cuda',True,arguments.binary,output/'gpu-fault.log','fault')
    if arguments.benchmark:
        results=[]
        for width,height in ((1920,1080),(2560,1440)):
            for alpha in (False,True):
                for mode in ('cpu','cuda'):
                    result=session(mode,alpha,arguments.binary,output/f'gpu-{mode}-{width}-{int(alpha)}.log',(width,height))
                    results.append(result)
                    print(f"MEASURE {mode} {width}x{height} alpha={alpha}: color input+encode {result['color_input_encode_ms']:.3f} ms, loopback refresh-to-receive {result['refresh_receive_avg_ms']:.3f} ms")
        (output/'gpu-input-benchmark.json').write_text(json.dumps(results,indent=2)+'\n')
        print('Measurements are same-host synthetic refresh/capture/encode stages, not physical input-to-photon latency. CUDA includes GPU color work and optional lossless alpha readback; it is not a zero-copy pipeline.')


if __name__=='__main__':main()
