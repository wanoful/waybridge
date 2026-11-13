#!/usr/bin/env python3
"""Real compositor/NVENC resize latency, fresh IDRs, bounded cache and reference continuity."""
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

from hardware_smoke import ROOT, Viewer, decoded


def main():
    subprocess.run(['bash', 'tests/build-fixture.sh'], cwd=ROOT, check=True)
    output = Path(os.environ.get('WAYBRIDGE_RESIZE_REPORT', str(ROOT/'build/diagnostics/resize-latency.json')))
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='waybridge-resize-') as runtime, output.with_suffix('.log').open('w') as log:
        env = {**os.environ, 'XDG_RUNTIME_DIR': runtime, 'WAYLAND_DISPLAY': 'resize-0', 'WAYBRIDGE_FIXTURE_ALPHA': '1'}
        with socket.socket() as free:
            free.bind(('127.0.0.1', 0))
            port = free.getsockname()[1]
        token = 'resize-test-' + '7'*40
        token_path = Path(runtime)/'token'
        token_path.write_text(token)
        token_path.chmod(0o600)
        binary = os.environ.get('WAYBRIDGE_HOST_BINARY', str(ROOT/'target/debug/waybridge'))
        host = subprocess.Popen([binary, '--listen', f'127.0.0.1:{port}', '--socket', 'resize-0', '--token-file', str(token_path), '--codec', 'h264-nvenc', '--stats']+(['--gpu-input',os.environ['WAYBRIDGE_GPU_INPUT']] if os.getenv('WAYBRIDGE_GPU_INPUT') else []), env=env, stdout=log, stderr=log)
        app = viewer = None
        try:
            end = time.monotonic()+10
            while not (Path(runtime)/'resize-0').exists():
                assert host.poll() is None and time.monotonic()<end
                time.sleep(.02)
            viewer = Viewer(port, token)
            app = subprocess.Popen([str(ROOT/'build/tests/test-app')], env=env, stdout=log, stderr=log)
            first = viewer.video()
            window = first['id']
            previous_epoch = first['epoch']
            timings = []
            for width, height in [(2240,1400),(960,640)]*5:
                started = time.monotonic()
                viewer.send(type='resize', id=window, width=width, height=height, dpi=96)
                while True:
                    frame = viewer.video()
                    if frame['id']==window and (frame['w'],frame['h'])==(width,height):
                        break
                elapsed = (time.monotonic()-started)*1000
                assert frame['key'] and frame['epoch']!=previous_epoch, 'resize/cache activation needs a new epoch and IDR'
                previous_epoch = frame['epoch']
                image = decoded([frame])
                assert image.getpixel((width//2,height//2))[3]==128
                viewer.send(type='refresh')
                p = viewer.video()
                assert p['epoch']==frame['epoch'] and not p['key']
                assert decoded([frame,p]).getpixel((width//2,height//2))[3]==128
                timings.append(dict(width=width, height=height, first_frame_ms=elapsed, epoch=frame['epoch']))
            # Exceed the global idle-cache entry budget, then revisit an evicted size.
            for width,height in [(1280,720),(1024,768),(1400,900),(960,640)]:
                viewer.send(type='resize',id=window,width=width,height=height,dpi=96)
                while True:
                    frame=viewer.video()
                    if frame['id']==window and (frame['w'],frame['h'])==(width,height): break
                assert frame['key'] and frame['epoch']!=previous_epoch
                previous_epoch=frame['epoch']
                assert decoded([frame]).getpixel((width//2,height//2))[3]==128
            started=time.monotonic()
            for width,height in [(1800,1100),(1820,1120),(1840,1140),(976,656)]:
                viewer.send(type='resize',id=window,width=width,height=height,dpi=96)
            while True:
                frame=viewer.video()
                if frame['id']==window and (frame['w'],frame['h'])==(976,656): break
            burst_ms=(time.monotonic()-started)*1000
            assert frame['key'] and frame['epoch']!=previous_epoch
            assert decoded([frame]).getpixel((488,328))[3]==128
            time.sleep(2.1)
            if os.environ.get('WAYBRIDGE_RESIZE_BASELINE')!='1':
                trace=output.with_suffix('.log').read_text()
                assert 'encoder_reuse' in trace and 'resize_first_sent' in trace
                if os.getenv('WAYBRIDGE_REQUIRE_GPU')=='1': assert 'input="cuda"' in trace and 'CUDA GL capture failed' not in trace
            output.write_text(json.dumps({'binary':binary,'resizes':timings,'burst_first_frame_ms':burst_ms},indent=2)+'\n')
            times=[t['first_frame_ms'] for t in timings]
            # Keep the viewer, window and cached NVENC sessions alive: disconnect
            # cleanup must not be responsible for releasing them before SIGTERM.
            host.terminate()
            assert host.wait(timeout=5)==0, 'live NVENC session shutdown failed'
        finally:
            if viewer:
                viewer.media.close()
                viewer.control.close()
            if app:
                app.terminate()
                app.wait(timeout=5)
            host.terminate()
            try:
                assert host.wait(timeout=5)==0, 'graceful NVENC session shutdown failed'
            except subprocess.TimeoutExpired:
                host.kill(); host.wait(timeout=5)
                raise
        print(f'PASS: 10 real NVENC size switches, fresh IDRs, intact P references/alpha and graceful cached-encoder shutdown; first={times[0]:.2f}ms, warm average={sum(times[1:])/len(times[1:]):.2f}ms; report={output}')


if __name__=='__main__':
    main()
