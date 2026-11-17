#!/usr/bin/env python3
"""Real compositor admission, final repaint, frame callbacks and per-window credits."""
import argparse
import io
import json
import os
from pathlib import Path
import re
import select
import socket
import struct
import subprocess
import tempfile
import time

from PIL import Image
from hardware_smoke import ROOT, Viewer, decoded


def wait_for(check, host, timeout=10):
    deadline = time.monotonic() + timeout
    while not check():
        assert host.poll() is None, 'compositor exited; inspect media-backpressure log'
        assert time.monotonic() < deadline, 'fixture timed out'
        time.sleep(.02)


def run(args):
    subprocess.run(['bash', 'tests/build-fixture.sh'], cwd=ROOT, check=True)
    output = ROOT / 'build/diagnostics'
    output.mkdir(parents=True, exist_ok=True)
    log_path = output / f'media-backpressure-{args.codec}.log'
    fixture_path = output / f'media-backpressure-{args.codec}-app.log'
    with tempfile.TemporaryDirectory(prefix='waybridge-backpressure-') as runtime, \
            log_path.open('w') as log, fixture_path.open('w') as fixture_log:
        env = {**os.environ, 'XDG_RUNTIME_DIR': runtime, 'WAYLAND_DISPLAY': 'pressure-0',
               'WAYBRIDGE_TEST_FRAME_CALLBACKS': '1'}
        with socket.socket() as free:
            free.bind(('127.0.0.1', 0))
            port = free.getsockname()[1]
        token = 'pressure-' + 'a' * 40
        token_file = Path(runtime) / 'token'
        token_file.write_text(token)
        token_file.chmod(0o600)
        cli, capability, kind = {
            'png': ('png', [], 1),
            'h264': ('h264-nvenc', 'h264_alpha', 2),
        }[args.codec]
        host = subprocess.Popen([str(args.binary), '--listen', f'127.0.0.1:{port}',
                                 '--socket', 'pressure-0', '--token-file', str(token_file),
                                 '--codec', cli, '--stats', '--no-audio', '--no-xwayland'],
                                env=env, stdout=log, stderr=log)
        app = viewer = None
        try:
            wait_for(lambda: (Path(runtime) / 'pressure-0').exists(), host)
            viewer = Viewer(port, token, capability, kind)
            app = subprocess.Popen([str(ROOT / 'build/tests/test-app'), 'two'], env=env,
                                   stdout=fixture_log, stderr=fixture_log)

            def frame():
                body = viewer.until(lambda k, v: k == kind)
                if kind != 1:
                    return viewer.parse_video(body, ack=False)
                window, width, height, sequence = struct.unpack('>IIIQ', body[:20])
                return dict(id=window, w=width, h=height, seq=sequence,
                            image=Image.open(io.BytesIO(body[36:])).convert('RGBA'))

            first = {}
            while len(first) < 2:
                current = frame()
                first[current['id']] = current
            viewer.send(type='refresh')
            last = dict(first)
            filled = set()
            while len(filled) < 2:
                current = frame()
                last[current['id']] = current
                filled.add(current['id'])
            stalled, active = sorted(last)
            callback_count = fixture_path.read_text().count('FRAME_DONE ')
            for width, height in [(620, 410), (660, 430), (700, 450)]:
                viewer.send(type='resize', id=stalled, width=width, height=height, dpi=96)
            wait_for(lambda: '700 450' in fixture_path.read_text(), host)
            wait_for(lambda: fixture_path.read_text().count('FRAME_DONE ') > callback_count, host)
            # Flush timings from the two initial captures. No further ACKs follow.
            time.sleep(2.2)
            marker = len(log_path.read_text())
            for _ in range(25):
                viewer.send(type='refresh')
                time.sleep(.01)
            viewer.send(type='ping', nonce=987)
            viewer.until(lambda k, v: k == 0 and v.get('type') == 'pong' and v['nonce'] == 987)
            time.sleep(2.2)
            assert not select.select([viewer.media], [], [], .1)[0], 'media exceeded two presentation credits'
            trace = log_path.read_text()[marker:]
            assert 'stage="composite"' not in trace, 'blocked viewer still caused GPU composition/readback'
            waits = sum(map(int, re.findall(r'counter="capture_wait_credit" count=(\d+)', trace)))
            assert waits > 0, 'no capture admission diagnostics'

            # An unblocked second window remains usable while the first is stalled.
            viewer.send(type='frame_ack', id=active, sequence=last[active]['seq'])
            current = frame()
            assert current['id'] == active, 'stalled window consumed another window\'s credit'
            viewer.send(type='frame_ack', id=active, sequence=current['seq'])
            # No refresh/new application commit: the last resized image must survive.
            viewer.send(type='frame_ack', id=stalled, sequence=last[stalled]['seq'])
            current = frame()
            assert (current['id'], current['w'], current['h']) == (stalled, 700, 450), current
            image = current['image'] if kind == 1 else decoded([current])
            assert image.getpixel((350, 100))[0] > 220
            assert image.getpixel((350, 350))[2] > 220
            viewer.send(type='frame_ack', id=stalled, sequence=current['seq'])
            time.sleep(2.2)
            trace = log_path.read_text()
            assert all(field in trace for field in ('p50_ms=', 'p95_ms=', 'p99_ms='))
            assert 'stage="capture_to_encode"' in trace and 'stage="media_send_queue"' in trace
            assert 'gauge="raw_queue_bytes"' in trace and 'gauge="raw_queue_frames"' in trace
            report = dict(codec=args.codec, capture_wait_polls=waits, blocked_composites=0,
                          final_size=[700, 450], frame_callbacks=True, independent_window=True)
            (output / f'media-backpressure-{args.codec}.json').write_text(json.dumps(report, indent=2) + '\n')
            print(f'PASS: {args.codec} stops capture before GL while blocked, keeps callbacks/control/other windows live, resumes the final static resize, and reports bounded queue/percentile diagnostics')
        finally:
            if viewer:
                viewer.control.close()
                viewer.media.close()
            if app:
                app.terminate()
                app.wait(timeout=5)
            host.terminate()
            host.wait(timeout=5)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--codec', choices=('png', 'h264'), default='png')
    parser.add_argument('--binary', type=Path, default=ROOT / 'target/debug/waybridge')
    run(parser.parse_args())
