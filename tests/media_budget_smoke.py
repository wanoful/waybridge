#!/usr/bin/env python3
"""Real compositor resource contention, impossible requests and CUDA codec recovery.

Run under tests/session_fixture.py. --png-only omits the explicit NVIDIA/CUDA
case; the default requires working CUDA/NVENC and fails rather than silently
turning a hardware check into PNG-only coverage.
"""
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

MIB = 1024 * 1024
OUTPUT = ROOT / 'build/diagnostics'
GAUGES = ('media_cpu_reserved_bytes', 'media_gpu_estimated_bytes', 'media_encoder_slots')


class BudgetViewer(Viewer):
    """The real v3 split transport, with ACKs and deliberate foreground policy."""
    def __init__(self, port, token, video=False):
        self.control = self.media = None
        self.kind = 2
        self.frames = []
        self.messages = []
        self.latest_sequence = {}
        deadline = time.monotonic() + 8
        while True:
            try:
                self.control = socket.create_connection(('127.0.0.1', port), timeout=3)
                self.send(type='hello', version=3, token=token, split_media=True,
                          frame_ack=True, activation_v1=True,
                          codecs=['png_rgba'] + (['h264_alpha'] if video else []))
                kind, hello = self.packet(self.control)
                assert kind == 0 and hello['type'] == 'hello', hello
                self.media = socket.create_connection(('127.0.0.1', port), timeout=3)
                self.write(self.media, dict(type='hello', version=3, token=token,
                                           channel='media', media_key=hello['media_key']))
                assert self.packet(self.media)[1]['type'] == 'media_ready'
                break
            except (ConnectionError, EOFError, OSError):
                self.close()
                assert time.monotonic() < deadline, 'viewer could not reconnect'
                time.sleep(.05)
        # Prove the compositor has consumed HELLO before fixtures are created.
        self.ping()

    def close(self):
        for name in ('control', 'media'):
            sock = getattr(self, name, None)
            if sock:
                sock.close()
                setattr(self, name, None)

    def poll(self, duration=.1):
        ready, _, _ = select.select([self.control, self.media], [], [], duration)
        for sock in ready:
            kind, payload = self.packet(sock)
            if kind == 0:
                self.messages.append(payload)
            elif kind in (1, 2):
                window, width, height, sequence = struct.unpack('>IIIQ', payload[:20])
                self.send(type='frame_ack', id=window, sequence=sequence)
                frame = dict(id=window, w=width, h=height, seq=sequence,
                             kind=kind, body=payload)
                if kind == 2:
                    frame['epoch'], _, _, frame['key'] = struct.unpack('>IIII', payload[36:52])
                self.frames.append(frame)
                self.latest_sequence[window] = sequence
            else:
                raise AssertionError(f'unexpected media kind {kind}')

    def pump(self, duration):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            self.poll(min(.1, max(0, deadline - time.monotonic())))

    def ping(self):
        nonce = time.monotonic_ns()
        self.send(type='ping', nonce=nonce)
        deadline = time.monotonic() + 3
        while True:
            for index, message in enumerate(self.messages):
                if message.get('type') == 'pong' and message.get('nonce') == nonce:
                    self.messages.pop(index)
                    return
            assert time.monotonic() < deadline, 'control stalled under media pressure'
            self.poll()

    def expect_frame(self, window=None, size=None, kind=None, after=-1, timeout=8, exclude=()):
        deadline = time.monotonic() + timeout
        while True:
            for index, frame in enumerate(self.frames):
                if (frame['id'] not in exclude
                        and (window is None or frame['id'] == window)
                        and (size is None or (frame['w'], frame['h']) == size)
                        and (kind is None or frame['kind'] == kind)
                        and frame['seq'] > after):
                    return self.frames.pop(index)
            assert time.monotonic() < deadline, (
                f'missing frame window={window} size={size} kind={kind}; '
                f'pending={[(f["id"], f["w"], f["h"], f["kind"]) for f in self.frames[-8:]]}')
            self.poll()

    def image(self, frame, preceding=()):
        if frame['kind'] == 1:
            return Image.open(io.BytesIO(frame['body'][36:])).convert('RGBA')
        chain = [self.parse_video(item['body'], ack=False) for item in (*preceding, frame)]
        return decoded(chain)


def colors(viewer, frame, preceding=()):
    image = viewer.image(frame, preceding)
    assert image.size == (frame['w'], frame['h'])
    x, y = image.width // 2, image.height // 4
    assert image.getpixel((x, y))[0] > 220, 'top fixture pixels must remain red'
    assert image.getpixel((x, y * 3))[2] > 220, 'bottom fixture pixels must remain blue'


def wait_for(check, session, timeout=8):
    deadline = time.monotonic() + timeout
    while not check():
        assert session.host.poll() is None, f'compositor exited; inspect {session.log_path}'
        assert time.monotonic() < deadline, 'fixture timeout'
        session.viewer.poll(.05)


class Session:
    def __init__(self, name, binary, cpu, gpu, encoders=1, video=False):
        self.name, self.binary = name, binary
        self.cpu, self.gpu, self.encoders, self.video = cpu, gpu, encoders, video
        self.log_path = OUTPUT / f'media-budget-{name}.log'
        self.apps = []
        self.app_logs = []
        self.viewer = self.host = None

    def __enter__(self):
        try:
            return self.start()
        except BaseException:
            self.__exit__()
            raise

    def start(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='waybridge-media-budget-')
        self.runtime = Path(self.temporary.name)
        self.env = {**os.environ, 'XDG_RUNTIME_DIR': str(self.runtime),
                    'WAYLAND_DISPLAY': 'budget-0', 'WAYBRIDGE_TEST_FRAME_CALLBACKS': '1'}
        config = self.runtime / 'waybridge.toml'
        config.write_text(f'''[media_budget]
cpu_mib = {self.cpu}
gpu_mib = {self.gpu}
window_cpu_mib = {self.cpu}
window_gpu_mib = {self.gpu}
encoders = {self.encoders}
''')
        self.token = 'budget-' + 'a' * 40
        token_path = self.runtime / 'token'
        token_path.write_text(self.token)
        token_path.chmod(0o600)
        with socket.socket() as free:
            free.bind(('127.0.0.1', 0))
            self.port = free.getsockname()[1]
        self.log = self.log_path.open('w')
        arguments = [str(self.binary), '--config', str(config), '--listen',
                     f'127.0.0.1:{self.port}', '--socket', 'budget-0', '--token-file',
                     str(token_path), '--codec', 'h264-nvenc' if self.video else 'png',
                     '--stats', '--no-audio', '--no-xwayland']
        if self.video:
            arguments += ['--gpu-input', 'cuda']
        self.host = subprocess.Popen(arguments, env=self.env, stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 10
        while not (self.runtime / 'budget-0').exists():
            assert self.host.poll() is None, self.log_path.read_text()[-4000:]
            assert time.monotonic() < deadline, 'compositor socket unavailable'
            time.sleep(.02)
        self.viewer = BudgetViewer(self.port, self.token, self.video)
        return self

    def start_app(self):
        path = OUTPUT / f'media-budget-{self.name}-app-{len(self.apps)}.log'
        stream = path.open('w')
        app = subprocess.Popen([str(ROOT / 'build/tests/test-app')], env=self.env,
                               stdout=stream, stderr=stream)
        self.apps.append(app)
        self.app_logs.append((stream, path))
        return app, path

    def small_app(self, exclude=()):
        app, path = self.start_app()
        frame = self.viewer.expect_frame(kind=1, exclude=exclude)
        if (frame['w'], frame['h']) != (400, 300):
            self.viewer.send(type='resize', id=frame['id'], width=400, height=300, dpi=96)
            frame = self.viewer.expect_frame(frame['id'], (400, 300), kind=1)
        return app, path, frame

    def stop_app(self, app):
        if app.poll() is None:
            app.terminate()
            app.wait(timeout=5)

    def reconnect(self):
        self.viewer.close()
        self.viewer = BudgetViewer(self.port, self.token, self.video)
        assert self.host.poll() is None

    def snapshot(self):
        trace = self.log_path.read_text()
        rows = re.findall(r'gauge="([^"]+)" value=(\d+) max=(\d+)', trace)
        peaks, latest = {}, {}
        for name, value, maximum in rows:
            latest[name] = int(value)
            peaks[name] = max(peaks.get(name, 0), int(maximum))
        return peaks, latest

    def check_limits(self):
        self.viewer.pump(2.2)
        peaks, latest = self.snapshot()
        limits = dict(zip(GAUGES, (self.cpu * MIB, self.gpu * MIB, self.encoders)))
        for name, limit in limits.items():
            assert name in peaks, f'missing {name} diagnostics'
            assert peaks[name] <= limit, (name, peaks[name], limit)
        self.viewer.ping()
        return dict(peak={name: peaks[name] for name in GAUGES},
                    steady={name: latest[name] for name in GAUGES})

    def check_released(self):
        wait_for(lambda: all(self.snapshot()[1].get(name) == 0 for name in GAUGES),
                 self, timeout=6)
        self.viewer.ping()
        return {name: self.snapshot()[1][name] for name in GAUGES}

    def __exit__(self, *unused):
        for app in self.apps:
            self.stop_app(app)
        if self.viewer:
            self.viewer.close()
        if self.host and self.host.poll() is None:
            self.host.terminate()
            try:
                self.host.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.host.kill()
                self.host.wait(timeout=5)
        for stream, _ in self.app_logs:
            stream.close()
        if hasattr(self, 'log'):
            self.log.close()
        if hasattr(self, 'temporary'):
            self.temporary.cleanup()


def png_contention(binary):
    with Session('png', binary, cpu=16, gpu=4) as session:
        first, _, a = session.small_app()
        session.viewer.send(type='focus', id=a['id'], focused=True)
        second, path, b = session.small_app(exclude=(a['id'],))
        assert a['id'] != b['id']
        session.viewer.send(type='resize', id=a['id'], width=1000, height=700, dpi=96)
        a = session.viewer.expect_frame(a['id'], (1000, 700), kind=1)
        colors(session.viewer, a)
        session.viewer.send(type='resize', id=b['id'], width=1000, height=700, dpi=96)
        wait_for(lambda: 'CONFIGURE 1 1000 700' in path.read_text(), session)
        session.viewer.pump(.6)
        assert not any(f['id'] == b['id'] and (f['w'], f['h']) == (1000, 700)
                       for f in session.viewer.frames), 'two targets exceeded the 4MiB GPU cap'
        session.viewer.ping()
        # The pending source is now static; freeing A must be enough to deliver it.
        commits = path.read_text().count('CONFIGURE ')
        session.stop_app(first)
        recovered = session.viewer.expect_frame(b['id'], (1000, 700), kind=1)
        colors(session.viewer, recovered)
        assert path.read_text().count('CONFIGURE ') == commits, 'recovery relied on a new application repaint'
        session.reconnect()
        restored = session.viewer.expect_frame(b['id'], (1000, 700), kind=1)
        colors(session.viewer, restored)
        assert second.poll() is None, 'viewer reconnect ended the application'
        report = session.check_limits()
        session.stop_app(second)
        report['released'] = session.check_released()
        print('PASS: low-budget PNG target contention preserves foreground, final static update recovers after release, reconnect retains app and all leases return')
        return report


def impossible_request(binary):
    with Session('oversized', binary, cpu=16, gpu=4) as session:
        healthy, _, a = session.small_app()
        bad, path, b = session.small_app(exclude=(a['id'],))
        session.viewer.send(type='focus', id=b['id'], focused=True)
        session.viewer.send(type='resize', id=b['id'], width=3840, height=2160, dpi=96)
        wait_for(lambda: 'CONFIGURE 1 3840 2160' in path.read_text(), session)
        # Drain the interval containing setup, then observe persistent refusal.
        session.viewer.pump(2.2)
        marker = len(session.log_path.read_text())
        session.viewer.pump(2.2)
        trace = session.log_path.read_text()[marker:]
        assert 'counter="media_budget_request_exceeds_limit"' in trace, 'impossible request was not distinguished'
        assert 'counter="media_target_reclaim"' not in trace, 'impossible request churned a healthy target'
        assert 'counter="media_encoder_reclaim"' not in trace, 'impossible request churned healthy encoders'
        session.viewer.send(type='refresh')
        current = session.viewer.expect_frame(a['id'], (400, 300), kind=1, after=a['seq'])
        colors(session.viewer, current)
        session.viewer.ping()
        # A later feasible geometry must recover without resetting the connection.
        before = session.viewer.latest_sequence[b['id']]
        session.viewer.send(type='resize', id=b['id'], width=400, height=300, dpi=96)
        restored = session.viewer.expect_frame(b['id'], (400, 300), kind=1, after=before)
        colors(session.viewer, restored)
        report = session.check_limits()
        session.stop_app(healthy)
        session.stop_app(bad)
        report['released'] = session.check_released()
        print('PASS: permanently oversized capture does not evict unrelated windows, control stays live and feasible resize recovers')
        return report


def cuda_contention(binary):
    with Session('cuda', binary, cpu=64, gpu=128, encoders=1, video=True) as session:
        first, _ = session.start_app()
        a = session.viewer.expect_frame(kind=2)
        assert a['key']
        colors(session.viewer, a)
        session.viewer.send(type='focus', id=a['id'], focused=True)
        second, path = session.start_app()
        b = session.viewer.expect_frame(exclude=(a['id'],))
        assert b['kind'] == 1, 'encoder pressure must provide a usable temporary PNG without evicting focused video'
        colors(session.viewer, b)
        session.viewer.pump(.5)
        assert not any(f['id'] == b['id'] and f['kind'] == 2 for f in session.viewer.frames), 'background video stole the protected encoder slot'
        session.viewer.ping()
        # No refresh or resize: release is the only event needed for recovery.
        commits = path.read_text().count('CONFIGURE ')
        session.stop_app(first)
        size = (b['w'], b['h'])
        video = session.viewer.expect_frame(b['id'], size, kind=2, after=b['seq'])
        assert video['key'], 'temporary PNG recovery must start an independent video epoch'
        colors(session.viewer, video)
        assert path.read_text().count('CONFIGURE ') == commits, 'video recovery relied on a fresh source commit'
        session.viewer.send(type='refresh')
        following = session.viewer.expect_frame(b['id'], size, kind=2, after=video['seq'])
        assert following['epoch'] == video['epoch'] and not following['key']
        colors(session.viewer, following, preceding=(video,))
        session.viewer.pump(.7)
        assert all(f.get('epoch') == video['epoch'] for f in session.viewer.frames
                   if f['id'] == b['id'] and f['kind'] == 2), 'recovered encoder churned after stale budget pressure'
        trace = session.log_path.read_text()
        assert 'CUDA GL color input available' in trace and 'input="cuda"' in trace, 'case requires real CUDA input'
        session.reconnect()
        restored = session.viewer.expect_frame(b['id'], size, kind=2)
        assert restored['key'] and second.poll() is None
        colors(session.viewer, restored)
        report = session.check_limits()
        assert 'counter="media_budget_wait"' in session.log_path.read_text(), 'encoder pressure was not observed'
        session.stop_app(second)
        report['released'] = session.check_released()
        print('PASS: actual CUDA/NVENC slot contention yields usable PNG, preserves focused video, resumes a fresh H264 IDR/P chain after release and reconnects within budget')
        return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, default=ROOT / 'target/debug/waybridge')
    parser.add_argument('--png-only', action='store_true')
    parser.add_argument('--scenario', choices=('all', 'png', 'oversized', 'cuda'), default='all')
    args = parser.parse_args()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    subprocess.run(['bash', 'tests/build-fixture.sh'], cwd=ROOT, check=True)
    report = {}
    if args.scenario in ('all', 'png'):
        report['png'] = png_contention(args.binary)
    if args.scenario in ('all', 'oversized'):
        report['oversized'] = impossible_request(args.binary)
    if args.scenario == 'cuda' or (args.scenario == 'all' and not args.png_only):
        report['cuda'] = cuda_contention(args.binary)
    for name, evidence in report.items():
        (OUTPUT / f'media-budget-{name}.json').write_text(json.dumps(evidence, indent=2) + '\n')
    (OUTPUT / 'media-budget-report.json').write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
