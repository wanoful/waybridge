#!/usr/bin/env python3
"""Actual Linux NVENC: reference continuity, alpha, odd sizes, split transport and PNG fallback."""
import io,json,os,re,select,socket,struct,subprocess,tempfile,time
from pathlib import Path
from PIL import Image
from smoke import ROOT

class Viewer:
    def __init__(self,port,token):
        self.control=socket.create_connection(('127.0.0.1',port),timeout=10)
        self.send(type='hello',version=3,token=token,split_media=True,frame_ack=True,codecs=['png_rgba','h264_alpha'])
        kind,hello=self.packet(self.control)
        assert kind==0 and hello['codec']=='mixed'
        # An authenticated connection with the wrong media capability must not steal the media socket.
        bad=socket.create_connection(('127.0.0.1',port),timeout=2)
        self.write(bad,dict(type='hello',version=3,token=token,channel='media',media_key='wrong'))
        assert bad.recv(1)==b'';bad.close()
        self.media=socket.create_connection(('127.0.0.1',port),timeout=10)
        self.write(self.media,dict(type='hello',version=3,token=token,channel='media',media_key=hello['media_key']))
        assert self.packet(self.media)[1]['type']=='media_ready'
    @staticmethod
    def write(sock,value):
        body=b'\0'+json.dumps(value).encode();sock.sendall(struct.pack('>I',len(body))+body)
    def send(self,**value):self.write(self.control,value)
    @staticmethod
    def exact(sock,size):
        out=bytearray()
        while len(out)<size:
            part=sock.recv(size-len(out))
            if not part:raise EOFError('media disconnected')
            out.extend(part)
        return out
    def packet(self,sock):
        size,=struct.unpack('>I',self.exact(sock,4));assert 0<size<=72*1024*1024
        data=self.exact(sock,size)
        return (0,json.loads(data[1:])) if data[0]==0 else (data[0],bytes(data[1:]))
    def receive(self):
        ready,_,_=select.select([self.control,self.media],[],[],10)
        assert ready,'no media/control response'
        return self.packet(ready[0])
    def until(self,check):
        end=time.monotonic()+15
        while time.monotonic()<end:
            kind,value=self.receive()
            if check(kind,value):return value
        raise AssertionError('missing NVENC packet; see hardware-host.log')
    def video(self,ack=True):
        body=self.until(lambda k,v:k==2)
        id,w,h,seq,lw,lh,x,y,epoch,cw,ch,key,color,alpha=struct.unpack('>IIIQiiiiIIIIII',body[:60])
        assert len(body)==60+color+alpha
        if ack:self.send(type='frame_ack',id=id,sequence=seq)
        mask=Image.open(io.BytesIO(body[60+color:])).convert('L') if alpha else Image.new('L',(w,h),255)
        return dict(id=id,w=w,h=h,seq=seq,epoch=epoch,cw=cw,ch=ch,key=key,color=body[60:60+color],alpha=mask)

def decoded(frames):
    w,h=frames[0]['cw'],frames[0]['ch']
    data=subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-f','h264','-i','pipe:0','-f','rawvideo','-pix_fmt','rgba','pipe:1'],input=b''.join(f['color'] for f in frames),capture_output=True,check=True,timeout=15).stdout
    assert len(data)==len(frames)*w*h*4,(len(data),len(frames),w,h)
    image=Image.frombytes('RGBA',(w,h),data[-w*h*4:]).crop((0,0,frames[-1]['w'],frames[-1]['h']))
    image.putalpha(frames[-1]['alpha']);return image

def main():
    subprocess.run(['bash','tests/build-fixture.sh'],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-hardware-') as runtime,open(ROOT/'build/tests/hardware-host.log','w') as log:
        env={**os.environ,'XDG_RUNTIME_DIR':runtime,'WAYLAND_DISPLAY':'hw-0','WAYBRIDGE_FIXTURE_ALPHA':'1'}
        with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
        token='hardware-test-'+40*'4';token_path=Path(runtime)/'token';token_path.write_text(token);token_path.chmod(0o600)
        host=subprocess.Popen([os.environ.get('WAYBRIDGE_HOST_BINARY',str(ROOT/'target/debug/waybridge')),'--listen',f'127.0.0.1:{port}','--socket','hw-0','--token-file',str(token_path),'--codec','h264-nvenc','--stats'],env=env,stdout=log,stderr=log)
        app=None
        try:
            end=time.monotonic()+10
            while not (Path(runtime)/'hw-0').exists():
                assert host.poll() is None and time.monotonic()<end;time.sleep(.05)
            viewer=Viewer(port,token)
            app=subprocess.Popen([str(ROOT/'build/tests/test-app')],env=env,stdout=log,stderr=log)
            first=viewer.video();assert first['key']==1
            frames=[first]
            for _ in range(5):
                viewer.send(type='refresh');frames.append(viewer.video())
            assert len({f['epoch'] for f in frames})==1
            assert any(not f['key'] for f in frames[1:]),'must exercise actual P frames'
            for frame in frames:
                nals=[frame['color'][m.end()]&31 for m in re.finditer(rb'\x00\x00\x00?\x01',frame['color']) if m.end()<len(frame['color'])]
                assert nals and nals[0]==9,('each access unit must start with AUD for streaming decoders',nals)
                assert 12 not in nals,('filler NALs must not delay the final decoded frame',nals)
            image=decoded(frames);w,h=image.size
            red=image.getpixel((w//6,h//2));green=image.getpixel((w//2,h//2));clear=image.getpixel((w*5//6,h//2))
            assert red[0]>225 and red[1]<25 and red[3]==255,red
            assert green[1]>225 and green[0]<25 and green[3]==128,green
            assert clear[3]==0,clear
            image.save(ROOT/'build/tests/hardware-alpha.png')
            (ROOT/'build/tests/hardware.h264').write_bytes(b''.join(f['color'] for f in frames))
            viewer.send(type='refresh');a=viewer.video(ack=False)
            viewer.send(type='refresh');b=viewer.video(ack=False)
            viewer.send(type='refresh');time.sleep(.1)
            assert not select.select([viewer.media],[],[],.15)[0],'media must stop after two unacknowledged frames'
            viewer.send(type='ping',nonce=456)
            viewer.control.settimeout(2)
            while True:
                kind,value=viewer.packet(viewer.control)
                if kind==0 and value['type']=='pong':assert value['nonce']==456;break
            viewer.send(type='frame_ack',id=first['id'],sequence=b['seq'])
            c=viewer.video()
            assert c['seq']>b['seq']
            decoded(frames+[a,b,c])
            viewer.send(type='resize',id=first['id'],width=641,height=481,dpi=96)
            odd=viewer.video()
            while (odd['w'],odd['h'])!=(641,481):odd=viewer.video()
            assert (odd['cw'],odd['ch'])==(642,482) and odd['key'] and odd['epoch']!=first['epoch']
            assert decoded([odd]).getpixel((320,240))[3]==128
            # Control replies use a separate socket from the media stream.
            start=time.monotonic();viewer.send(type='ping',nonce=123)
            viewer.control.settimeout(2)
            while True:
                k,v=viewer.packet(viewer.control)
                if k==0 and v['type']=='pong':assert v['nonce']==123;break
            assert time.monotonic()-start<1
            viewer.send(type='codec_fallback',id=first['id'])
            png=viewer.until(lambda k,v:k==1)
            viewer.send(type='frame_ack',id=first['id'],sequence=struct.unpack('>Q',png[12:20])[0])
            image=Image.open(io.BytesIO(png[36:])).convert('RGBA')
            assert image.size==(641,481) and image.getpixel((320,240))==(0,255,0,128)
            time.sleep(2.1)
            timing=(ROOT/'build/tests/hardware-host.log').read_text()
            assert 'pipeline timing' in timing and 'display_roundtrip' in timing and 'nvenc' in timing
            viewer.media.close();viewer.control.close()
            print('PASS: real NVENC H264 IDR/P reference chain with AUD boundaries, decoded colors and exact alpha, odd-size epoch reset, authenticated split media/control, two-frame credit backpressure, reference continuity after resume, control ping, live PNG fallback')
        finally:
            if app:app.terminate();app.wait(timeout=5)
            host.terminate();host.wait(timeout=5)
if __name__=='__main__':main()
