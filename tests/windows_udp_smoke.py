#!/usr/bin/env python3
"""Actual PE/CNG decrypt + ordered XOR/TCP repair; real NVENC P chain and lossless alpha."""
import io,os,socket,struct,subprocess,tempfile,time
from pathlib import Path
from PIL import Image
from smoke import ROOT
from hardware_smoke import Viewer,decoded
from types import SimpleNamespace
def parse_video(body,kind):return Viewer.parse_video(SimpleNamespace(kind=kind),body,ack=False)
from udp_proxy import Proxy

def wait(check):
    end=time.monotonic()+15
    while time.monotonic()<end:
        if check():return
        time.sleep(.05)
    raise AssertionError('UDP fixture timed out')
def main():
    subprocess.run(['bash','tests/build-fixture.sh'],cwd=ROOT,check=True)
    subprocess.run(['bash','tests/build-hdr-fixture.sh'],cwd=ROOT,check=True)
    subprocess.run(['x86_64-w64-mingw32-g++','-municode','-static','-O2','-std=c++17','-D_WIN32_WINNT=0x0A00','-Iclient','tests/windows_udp_probe.cpp','-o','build/windows/waybridge-udp-probe.exe','-lws2_32','-lcrypt32','-lbcrypt'],cwd=ROOT,check=True)
    prefix=ROOT/'build/wine-udp';prefix.mkdir(parents=True,exist_ok=True)
    with open(ROOT/'build/tests/windows-udp.log','w') as log:
        r,w=os.pipe();xvfb=subprocess.Popen(['Xvfb','-displayfd',str(w),'-screen','0','1280x1024x24','-nolisten','tcp'],pass_fds=[w],stdout=log,stderr=log);os.close(w)
        with os.fdopen(r) as p:display=':'+p.readline().strip()
        wine={**os.environ,'DISPLAY':display,'WINEPREFIX':str(prefix),'WINEDEBUG':'-all','WINEDLLOVERRIDES':'winemenubuilder.exe=d;mscoree,mshtml=','WAYBRIDGE_UDP_HOST':'127.0.0.1'}
        try:
            subprocess.run(['wineboot','-u'],env=wine,stdout=log,stderr=log,timeout=60,check=True)
            cases=[('png',False,None,1),('png',True,None,1),('h264-nvenc',True,None,2),('h264-nvenc',True,50,2),('hevc-nvenc',True,None,4),('av1-nvenc',True,None,5),('hevc-nvenc',True,None,7),('av1-nvenc',True,None,8)]
            selected={int(kind) for kind in os.environ.get('WAYBRIDGE_UDP_TEST_KINDS','1,2,4,5,7,8').split(',')}
            for codec,faults,blackhole,expected_kind in cases:
                if expected_kind not in selected:continue
                hdr=expected_kind in (7,8)
                with tempfile.TemporaryDirectory(prefix='waybridge-udp-') as runtime:
                    env={**os.environ,'XDG_RUNTIME_DIR':runtime,'WAYLAND_DISPLAY':'udp-0','WAYBRIDGE_FIXTURE_ALPHA':'1','WAYBRIDGE_HDR_SMALL':'1','WAYBRIDGE_HDR_RAMP':'1'}
                    token='udp-'+40*'a';path=Path(runtime)/'token';path.write_text(token);path.chmod(0o600)
                    with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
                    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as free:free.bind(('127.0.0.1',0));udpport=free.getsockname()[1]
                    proxy=Proxy(('127.0.0.1',udpport),faults=faults,blackhole_after=blackhole);host=app=None
                    try:
                        host=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--listen',f'127.0.0.1:{port}','--udp-listen',f'127.0.0.1:{udpport}','--socket','udp-0','--token-file',str(path),'--codec',codec]+(['--hdr','experimental-scrgb','--bitrate','50000'] if hdr else []),env=env,stdout=log,stderr=log)
                        wait(lambda:(Path(runtime)/'udp-0').exists())
                        app=subprocess.Popen([str(ROOT/'build/tests/hdr-app' if hdr else ROOT/'build/tests/test-app')],env=env,stdin=subprocess.PIPE,stdout=log,stderr=log)
                        output=ROOT/f'build/tests/udp-{expected_kind}-{faults}-{blackhole}.bin'
                        probe_env={**wine,'WAYBRIDGE_UDP_PORT':str(proxy.port)}
                        if hdr:probe_env['WAYBRIDGE_UDP_TEST_HDR']='1'
                        result=subprocess.run(['wine',str(ROOT/'build/windows/waybridge-udp-probe.exe'),str(port),token,str(output), '24'],env=probe_env,capture_output=True,text=True,timeout=45)
                        assert result.returncode==0,(result.returncode,result.stdout,result.stderr)
                        assert proxy.received>5 and 'UDP ordered media active' in result.stdout,(proxy.received,result.stdout)
                        data=output.read_bytes();packets=[]
                        while data:
                            n,=struct.unpack('>I',data[:4]);assert len(data)>=4+n
                            packets.append((data[4],data[5:4+n]));data=data[4+n:]
                        assert len(packets)>=24
                        seq=[struct.unpack('>Q',p[12:20])[0] for _,p in packets];assert seq==sorted(set(seq)),seq
                        if codec=='png':
                            for kind,p in packets:
                                assert kind==1
                                image=Image.open(io.BytesIO(p[36:])).convert('RGBA');w,h=image.size
                                assert image.getpixel((w//2,h//2))==(0,255,0,128)
                        elif not hdr:
                            frames=[parse_video(p,kind) for kind,p in packets if kind==expected_kind]
                            assert len(frames)==len(packets) and frames[0]['key']==1 and any(not f['key'] for f in frames[1:])
                            image=decoded(frames);w,h=image.size
                            r=image.getpixel((w//6,h//2));g=image.getpixel((w//2,h//2));a=image.getpixel((w*5//6,h//2))
                            assert r[0]>225 and r[3]==255 and g[1]>225 and g[3]==128 and a[3]==0,(r,g,a)
                        else:
                            from hdr_video_smoke import parse,decode
                            frames=[parse(p,kind) for kind,p in packets if kind==expected_kind]
                            assert len(frames)>=20 and frames[0]['key'] and any(not f['key'] for f in frames[1:]),(expected_kind,len(frames))
                            decode(frames,Path(runtime))
                        if faults:assert proxy.dropped and proxy.corrupted and proxy.replayed,(proxy.dropped,proxy.corrupted,proxy.replayed)
                        if blackhole:assert 'UDP frame repaired over TCP' in result.stdout
                        print(f'PASS: PE/CNG {codec} kind={expected_kind} faults={faults} blackhole={blackhole}, {len(packets)} exact ordered frames, shards={proxy.received}, lost={proxy.dropped}, corrupt={proxy.corrupted}, replay={proxy.replayed}; {result.stdout.strip()}')
                    finally:
                        proxy.close()
                        for process in [app,host]:
                            if process and process.poll() is None:process.terminate();process.wait(timeout=10)
        finally:
            subprocess.run(['wineserver','-k'],env=wine,stdout=log,stderr=log,timeout=10)
            xvfb.terminate();xvfb.wait(timeout=5)
if __name__=='__main__':main()
