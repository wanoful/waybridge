#!/usr/bin/env python3
"""Real tagged FP16 Wayland source, mixed SDR composition and honest SDR fallback."""
import io,json,math,os,socket,struct,subprocess,tempfile,time
from pathlib import Path
from PIL import Image
from hardware_smoke import ROOT,Viewer
from codecs_smoke import wait_socket

def main():
    subprocess.run(['bash','tests/build-hdr-fixture.sh'],cwd=ROOT,check=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-hdr-') as runtime,open(ROOT/'build/tests/hdr-host.log','w') as log:
        env={**os.environ,'XDG_RUNTIME_DIR':runtime,'WAYLAND_DISPLAY':'hdr-0','WAYBRIDGE_HDR_MIXED':'1'}
        with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
        token='hdr-test-'+'e'*40;file=Path(runtime)/'token';file.write_text(token);file.chmod(0o600)
        host=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--listen',f'127.0.0.1:{port}','--socket','hdr-0','--token-file',str(file),'--codec','png','--hdr','experimental-scrgb'],env=env,stdout=log,stderr=log)
        app=viewer=None
        try:
            wait_socket(host,Path(runtime)/'hdr-0')
            viewer=Viewer(port,token,[])
            app=subprocess.Popen([str(ROOT/'build/tests/hdr-app')],env=env,stdin=subprocess.PIPE,text=True,stdout=log,stderr=log)
            png=viewer.until(lambda k,v:k==1)
            viewer.send(type='frame_ack',id=struct.unpack('>I',png[:4])[0],sequence=struct.unpack('>Q',png[12:20])[0])
            image=Image.open(io.BytesIO(png[36:])).convert('RGBA');width,height=image.size
            def gamma(value):return value*12.92 if value<=.0031308 else 1.055*value**(1/2.4)-.055
            x=width//2;lum=.25+4*x/(width-1)
            expected=round(gamma(lum/(1+lum))*255)
            actual=image.getpixel((x,height//2));assert max(abs(actual[i]-expected) for i in range(3))<=1 and actual[3]==128,(actual,expected)
            # Untagged SDR green at 50% alpha is linearized before compositing.
            x,y=48,32;lum=.25+4*x/(width-1)
            rgb=[lum*.5,lum*.5+.5,lum*.5];luma=.2126*rgb[0]+.7152*rgb[1]+.0722*rgb[2]
            expected=tuple(round(gamma(c/(1+luma))*255) for c in rgb)
            actual=image.getpixel((x,y));assert max(abs(actual[i]-expected[i]) for i in range(3))<=2 and actual[3]==255,(actual,expected)
            # Unsupported ICC fails only that Wayland client; it is never silently accepted.
            rejected=subprocess.run([str(ROOT/'build/tests/hdr-app'),'icc'],env=env,capture_output=True,text=True,timeout=8)
            assert rejected.returncode==0 and 'error' in rejected.stderr.lower(),rejected.stderr
            subprocess.run([str(ROOT/'build/tests/hdr-app'),'lifecycle'],env=env,check=True,timeout=8)
            app.stdin.write('pending\n');app.stdin.flush();time.sleep(.08)
            viewer.send(type='refresh');again=viewer.until(lambda k,v:k==1)
            viewer.send(type='frame_ack',id=struct.unpack('>I',again[:4])[0],sequence=struct.unpack('>Q',again[12:20])[0])
            assert Image.open(io.BytesIO(again[36:])).convert('RGBA').getpixel((width//2,height//2))==image.getpixel((width//2,height//2)),'description state must remain double-buffered until surface commit'
            app.stdin.write('sdr\n');app.stdin.flush()
            def is_blue(k,v):
                if k!=1:return False
                viewer.send(type='frame_ack',id=struct.unpack('>I',v[:4])[0],sequence=struct.unpack('>Q',v[12:20])[0])
                pixel=Image.open(io.BytesIO(v[36:])).convert('RGBA').getpixel((width//2,height*3//4))
                assert pixel[:3]==(0,0,255),pixel
                return True
            blue=viewer.until(is_blue)
            app.stdin.write('scrgb\n');app.stdin.flush();returned=viewer.until(lambda k,v:k==1)
            assert Image.open(io.BytesIO(returned[36:])).convert('RGBA').getpixel((width//2,height//2))==image.getpixel((width//2,height//2)),'same-size ARGB→FP16 must update cached texture format'
            assert host.poll() is None and app.poll() is None
            print('PASS: real tagged FP16/scRGB content, >SDR luminance tonemapped without clipping, exact 8-bit fallback alpha, mixed SDR linear composition, double-buffered color state, ICC rejection and return to ordinary SDR')
        finally:
            if viewer:viewer.media.close();viewer.control.close()
            if app:app.terminate();app.wait(timeout=5)
            host.terminate();host.wait(timeout=5)
if __name__=='__main__':main()
