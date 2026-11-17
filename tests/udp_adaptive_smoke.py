#!/usr/bin/env python3
"""Production PE/CNG UDP through private loss/delayed-feedback relays, with recovery."""
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
import threading
import time

from PIL import Image
from udp_flow_smoke import (BUILD, ROOT, Crypto, DelayProxy, LossProxy, free_port,
                            read_packet, stop, wait)


class FeedbackRelay(DelayProxy):
    def __init__(self, address):
        self.feedback_started = None
        self.delayed_acks = 0
        super().__init__(address)

    def forward(self, source, target, upstream):
        try:
            while not self.closed.is_set():
                body = read_packet(source)
                if body[0] == 0:
                    value = json.loads(body[1:])
                    if not upstream and isinstance(value.get('udp_fec_v1'), dict):
                        self.offer = value['udp_fec_v1']
                        self.offer_ready.set()
                    if upstream and value.get('type') == 'udp_ack':
                        if self.feedback_started is None:
                            self.feedback_started = time.monotonic()
                        elapsed = time.monotonic() - self.feedback_started
                        if 1 <= elapsed < 3:
                            self.delayed_acks += 1
                            self.closed.wait(.2)
                target.sendall(struct.pack('>I', len(body)) + body)
        except (EOFError, OSError):
            pass
        except Exception as error:
            self.errors.append(error)
        finally:
            try:
                target.shutdown(socket.SHUT_WR)
            except OSError:
                pass


def run(wine, crypto, log, log_path):
    with tempfile.TemporaryDirectory(prefix='waybridge-udp-adaptive-') as directory:
        runtime = Path(directory)
        env = {**os.environ, 'XDG_RUNTIME_DIR': directory, 'WAYLAND_DISPLAY': 'adaptive-0'}
        for name in ('DISPLAY', 'WAYBRIDGE_FIXTURE_ALPHA', 'WAYBRIDGE_FIXTURE_CSD'):
            env.pop(name, None)
        token = 'adaptive-' + 'a' * 40
        token_path = runtime / 'token'
        token_path.write_text(token)
        token_path.chmod(0o600)
        tcp_port, udp_port = free_port(), free_port(socket.SOCK_DGRAM)
        host = app = relay = loss = None
        try:
            host = subprocess.Popen([
                str(ROOT / 'target/debug/waybridge'), '--listen', f'127.0.0.1:{tcp_port}',
                '--udp-listen', f'127.0.0.1:{udp_port}', '--udp-mbps', '80', '--stats',
                '--socket', 'adaptive-0', '--token-file', str(token_path), '--codec', 'png',
                '--no-audio', '--no-xwayland'], env=env, stdout=log, stderr=log)
            wait(lambda: host.poll() is not None or (runtime / 'adaptive-0').exists())
            assert host.poll() is None, 'compositor exited'
            relay = FeedbackRelay(('127.0.0.1', tcp_port))
            loss = LossProxy(('127.0.0.1', udp_port), relay, crypto)
            app = subprocess.Popen([str(BUILD / 'test-app')], env=env, stdout=log, stderr=log)
            output = BUILD / 'udp-adaptive-frames.bin'
            result = subprocess.run([
                'wine', str(ROOT / 'build/windows/waybridge-udp-probe.exe'), str(relay.port),
                token, str(output), '180'], env={**wine, 'WAYBRIDGE_UDP_HOST': '127.0.0.1',
                                               'WAYBRIDGE_UDP_PORT': str(loss.port)},
                capture_output=True, text=True, timeout=40)
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert not relay.errors and not loss.errors, (relay.errors, loss.errors)
            assert loss.dropped > 0 and relay.delayed_acks >= 4
            data, previous, count = output.read_bytes(), 0, 0
            while data:
                size, = struct.unpack('>I', data[:4])
                assert data[4] == 1 and len(data) >= size + 4
                frame = data[5:4 + size]
                _, width, height, sequence = struct.unpack('>IIIQ', frame[:20])
                assert sequence > previous
                previous = sequence
                image = Image.open(io.BytesIO(frame[36:])).convert('RGBA')
                assert image.size == (width, height)
                assert image.getpixel((width // 2, height // 4)) == (255, 0, 0, 255)
                assert image.getpixel((width // 2, height * 3 // 4)) == (0, 0, 255, 255)
                count += 1
                data = data[4 + size:]
            assert count >= 180
            time.sleep(2.2)
            trace = log_path.read_text()
            counters = {}
            for name, number in re.findall(r'counter="(udp_[^"]+)" count=(\d+)', trace):
                counters[name] = counters.get(name, 0) + int(number)
            for required in ('udp_pacing_backoff_repair', 'udp_pacing_recovery'):
                assert counters.get(required, 0) > 0, (required, counters, trace[-6000:])
            rates = [int(value) for value in re.findall(r'gauge="udp_pacing_kbps" value=(\d+)', trace)]
            assert len(rates) >= 3 and min(rates) < 80_000 and rates[-1] > min(rates), rates
            assert all(2_000 <= rate <= 80_000 for rate in rates)
            assert 'stage="udp_delivery_ack"' in trace
            report = dict(frames=count, dropped_shards=loss.dropped, delayed_acks=relay.delayed_acks,
                          pacing_kbps=rates, counters=counters)
            (BUILD / 'udp-adaptive-report.json').write_text(json.dumps(report, indent=2) + '\n')
            print(f'PASS: real PE/CNG, {count} exact ordered PNG frames through UDP loss/TCP repair and 200ms feedback delay; pacing backs off then recovers within ceiling: {rates}', flush=True)
        finally:
            if loss:
                loss.close()
            if relay:
                relay.close()
            stop(app)
            stop(host)


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    (ROOT / 'build/windows').mkdir(parents=True, exist_ok=True)
    subprocess.run(['cargo', 'build', '--locked'], cwd=ROOT, check=True)
    subprocess.run(['bash', 'tests/build-fixture.sh'], cwd=ROOT, check=True)
    subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror', 'tests/udp_crypto_fixture.c',
                    '-o', str(BUILD / 'udp-crypto-fixture.so'), '-lcrypto'], cwd=ROOT, check=True)
    subprocess.run(['x86_64-w64-mingw32-g++', '-municode', '-static', '-O2', '-std=c++17',
                    '-D_WIN32_WINNT=0x0A00', '-Iclient', 'tests/windows_udp_probe.cpp', '-o',
                    str(ROOT / 'build/windows/waybridge-udp-probe.exe'), '-lws2_32', '-lcrypt32', '-lbcrypt'],
                   cwd=ROOT, check=True)
    log_path = BUILD / 'udp-adaptive.log'
    prefix = ROOT / 'build/wine-udp-adaptive'
    prefix.mkdir(mode=0o700, parents=True, exist_ok=True)
    with log_path.open('w') as log:
        read_fd, write_fd = os.pipe()
        xvfb = subprocess.Popen(['Xvfb', '-displayfd', str(write_fd), '-screen', '0', '1280x1024x24',
                                 '-nolisten', 'tcp'], pass_fds=[write_fd], stdout=log, stderr=log)
        os.close(write_fd)
        wine = None
        try:
            assert select.select([read_fd], [], [], 10)[0]
            with os.fdopen(read_fd) as pipe:
                display = ':' + pipe.readline().strip()
            read_fd = None
            wine = {**os.environ, 'DISPLAY': display, 'WINEPREFIX': str(prefix), 'WINEDEBUG': '-all',
                    'WINEDLLOVERRIDES': 'winemenubuilder.exe=d;mscoree,mshtml='}
            subprocess.run(['wineboot', '-u'], env=wine, stdout=log, stderr=log, check=True, timeout=60)
            run(wine, Crypto(), log, log_path)
        finally:
            if read_fd is not None:
                os.close(read_fd)
            if wine:
                subprocess.run(['wineserver', '-k'], env=wine, stdout=log, stderr=log, timeout=10)
            stop(xvfb)


if __name__ == '__main__':
    main()
