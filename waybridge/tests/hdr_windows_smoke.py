#!/usr/bin/env python3
"""Execute actual PE HDR shader math offscreen in isolated Wine/Xvfb.
This can validate FP16 arithmetic on software D3D11, never physical HDR/MF capability.
"""
import json,os,subprocess,tempfile
import numpy as np
from PIL import Image
from hardware_smoke import ROOT

def main():
    prefix=ROOT/'build/wine-hdr';prefix.mkdir(mode=0o700,parents=True,exist_ok=True)
    with open(ROOT/'build/tests/hdr-windows.log','w') as log:
        r,w=os.pipe();xvfb=subprocess.Popen(['Xvfb','-displayfd',str(w),'-screen','0','1280x1024x24','-nolisten','tcp'],pass_fds=[w],stdout=log,stderr=log);os.close(w)
        with os.fdopen(r) as pipe:display=':'+pipe.readline().strip()
        env={**os.environ,'DISPLAY':display,'WINEPREFIX':str(prefix),'WINEDEBUG':'-all','LIBGL_ALWAYS_SOFTWARE':'1','GALLIUM_DRIVER':'llvmpipe','MESA_LOADER_DRIVER_OVERRIDE':'llvmpipe','WINEDLLOVERRIDES':'winemenubuilder.exe=d;mscoree,mshtml='}
        env.pop('WAYBRIDGE_GDI',None)
        try:
            subprocess.run(['wineboot','-u'],env=env,stdout=log,stderr=log,timeout=40,check=True)
            result=subprocess.run(['wine',str(ROOT/'build/windows/waybridge-client.exe'),'--check-hdr-shaders'],env=env,capture_output=True,text=True,timeout=40)
            print(result.stdout,result.stderr)
            report=json.loads(result.stdout.strip().splitlines()[-1]);assert result.returncode==0 and report['verified'],report
            assert report['distinct_red_values']>256 and report['max_scrgb']>100 and report['exact_fp16_alpha'],report
            (ROOT/'build/diagnostics/hdr-windows-shader.json').write_text(json.dumps(report,indent=2)+'\n')
            alpha=ROOT/'build/tests/hdr-alpha16.png';eight=ROOT/'build/tests/hdr-alpha8.png'
            Image.fromarray(np.tile(np.arange(0x3000,0x3400,dtype=np.uint16),(64,1))).save(alpha)
            Image.new('L',(1024,64),128).save(eight)
            probe=subprocess.run(['wine',str(ROOT/'build/windows/waybridge-hdr-alpha-probe.exe'),str(alpha),str(eight)],env=env,text=True,capture_output=True,timeout=20,check=True)
            assert 'PASS' in probe.stdout,probe.stdout;print(probe.stdout.strip())
            print('PASS: actual Windows PE shader inverse PQ/P010 scaling/FP16 scRGB >100 SDR white/>256 levels/raw-half alpha; private software D3D11 math only, real HDR display/MFT remain untested')
        finally:
            subprocess.run(['wineserver','-k'],env=env,stdout=log,stderr=log,timeout=5)
            xvfb.terminate();xvfb.wait(timeout=5)
if __name__=='__main__':main()
