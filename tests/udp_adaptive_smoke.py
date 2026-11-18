#!/usr/bin/env python3
"""Production PE/CNG UDP through private loss/delayed-feedback relays, with recovery."""
import argparse
import io
from itertools import groupby
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
from types import SimpleNamespace

from PIL import Image
from hardware_smoke import Viewer, decoded
from udp_flow_smoke import (BUILD, ROOT, Crypto, DelayProxy, LossProxy, free_port,
                            read_packet, stop, wait)


class FeedbackRelay(DelayProxy):
    def __init__(self, address, delay_acks):
        self.delay_acks = delay_acks
        self.feedback_started = None
        self.delayed_acks = 0
        super().__init__(address)

    def forward(self, source, target, upstream):
        try:
            # An established control socket can sit idle while media flows.
            # The inherited connect timeout should not cut off its reader.
            source.settimeout(None)
            if not self.delay_acks:
                target.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            while not self.closed.is_set():
                body = read_packet(source)
                if body[0] == 0:
                    value = json.loads(body[1:])
                    if not upstream and isinstance(value.get('udp_fec_v1'), dict):
                        self.offer = value['udp_fec_v1']
                        self.offer_ready.set()
                    if upstream and value.get('type') == 'udp_retry' and value.get('message') == 1 and self.retry_started is None:
                        # Ensure the intentionally lost first message is old
                        # enough to train pacing when its repair reaches Linux.
                        self.retry_started = time.monotonic()
                        self.closed.wait(.15)
                    if self.delay_acks and upstream and value.get('type') == 'udp_ack':
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


def run(wine, crypto, log, log_path, nvenc, two_windows):
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
        udp_mbps = 8 if nvenc else 80
        bitrate = 6000
        codec = 'h264-nvenc' if nvenc else 'png'
        frame_count = 360 if two_windows else 270 if nvenc else 180
        host = app = relay = loss = None
        try:
            host = subprocess.Popen([
                str(ROOT / 'target/debug/waybridge'), '--listen', f'127.0.0.1:{tcp_port}',
                '--udp-listen', f'127.0.0.1:{udp_port}', '--udp-mbps', str(udp_mbps), '--stats',
                '--socket', 'adaptive-0', '--token-file', str(token_path), '--codec', codec,
                '--bitrate', str(bitrate), '--fps', '30' if nvenc else '60',
                '--no-audio', '--no-xwayland'], env=env, stdout=log, stderr=log)
            wait(lambda: host.poll() is not None or (runtime / 'adaptive-0').exists())
            assert host.poll() is None, 'compositor exited'
            relay = FeedbackRelay(('127.0.0.1', tcp_port), delay_acks=not nvenc)
            loss = LossProxy(('127.0.0.1', udp_port), relay, crypto)
            app_command = [str(BUILD / 'test-app')]
            if two_windows:
                app_command.append('two')
            app = subprocess.Popen(app_command, env=env, stdout=log, stderr=log)
            output = log_path.with_name(log_path.stem + '-frames.bin')
            result = subprocess.run([
                'wine', str(ROOT / 'build/windows/waybridge-udp-probe.exe'), str(relay.port),
                token, str(output), str(frame_count)], env={**wine, 'WAYBRIDGE_UDP_HOST': '127.0.0.1',
                                               'WAYBRIDGE_UDP_PORT': str(loss.port)},
                capture_output=True, text=True, timeout=40)
            assert result.returncode == 0, (result.stdout, result.stderr)
            assert not relay.errors and not loss.errors, (relay.errors, loss.errors)
            delayed = relay.delayed_acks
            assert loss.dropped > 0 and (nvenc or delayed >= 4)
            data, sequences, count = output.read_bytes(), {}, 0
            video = {}
            reader = SimpleNamespace(kind=2)
            while data:
                size, = struct.unpack('>I', data[:4])
                assert data[4] == (2 if nvenc else 1) and len(data) >= size + 4
                frame = data[5:4 + size]
                window, width, height, sequence = struct.unpack('>IIIQ', frame[:20])
                assert sequence > sequences.get(window, 0), (window, sequence, sequences)
                sequences[window] = sequence
                if nvenc:
                    video.setdefault(window, []).append(Viewer.parse_video(reader, frame, ack=False))
                else:
                    image = Image.open(io.BytesIO(frame[36:])).convert('RGBA')
                    assert image.size == (width, height)
                    assert image.getpixel((width // 2, height // 4)) == (255, 0, 0, 255)
                    assert image.getpixel((width // 2, height * 3 // 4)) == (0, 0, 255, 255)
                count += 1
                data = data[4 + size:]
            assert len(sequences) == (2 if two_windows else 1), sequences
            assert count >= frame_count
            time.sleep(2.2)
            trace = log_path.read_text()
            counters = {}
            for name, number in re.findall(r'counter="(udp_[^"]+)" count=(\d+)', trace):
                counters[name] = counters.get(name, 0) + int(number)
            for required in ('udp_pacing_backoff_repair', 'udp_pacing_recovery'):
                assert counters.get(required, 0) > 0, (required, counters, trace[-6000:])
            rates = [int(value) for value in re.findall(r'gauge="udp_pacing_kbps" value=(\d+)', trace)]
            ceiling = udp_mbps * 1000
            assert len(rates) >= 3 and min(rates) < ceiling and rates[-1] > min(rates), rates
            assert all(2_000 <= rate <= ceiling for rate in rates)
            assert 'stage="udp_delivery_ack"' in trace
            report = dict(frames=count, dropped_shards=loss.dropped, delayed_acks=delayed,
                          pacing_kbps=rates, counters=counters)
            if nvenc:
                encoder_rates = {
                    int(epoch): int(rate) for rate, epoch in
                    re.findall(r'bitrate_kbps=(\d+).*?epoch=(\d+)', trace)
                }
                epoch_rates = {
                    window: check_epochs(frames, encoder_rates, bitrate)
                    for window, frames in video.items()
                }
                targets = {window: [rate for _, rate in epochs] for window, epochs in epoch_rates.items()}
                if two_windows:
                    shared_ceiling = ceiling * 3 // 8
                    shares = {
                        int(epoch): max(1000, int(pacing) * 3 // 8)
                        for pacing, epoch in re.findall(r'udp_pacing_kbps=(\d+) active_windows=2 epoch=(\d+)', trace)
                    }
                    for window, epochs in epoch_rates.items():
                        assert any(epoch in shares and rate == shares[epoch] for epoch, rate in epochs), (epochs, shares)
                        assert min(targets[window]) < shared_ceiling == targets[window][-1], targets
                else:
                    only_window = next(iter(targets.values()))
                    assert only_window[0] == bitrate and min(only_window) < bitrate, only_window
                    assert only_window[-1] == bitrate, only_window
                report['encoder_epoch_kbps'] = epoch_rates
                print(f'PASS: {count} ordered PE/CNG NVENC frames; targets by window {targets} kbps, independent decodable epochs through loss and recovery', flush=True)
            else:
                print(f'PASS: real PE/CNG, {count} exact ordered PNG frames through UDP loss/TCP repair and 200ms feedback delay; pacing backs off then recovers within ceiling: {rates}', flush=True)
            log_path.with_name(log_path.stem + '-report.json').write_text(json.dumps(report, indent=2) + '\n')
        finally:
            if loss:
                loss.close()
            if relay:
                relay.close()
            stop(app)
            stop(host)


def check_epochs(frames, encoder_rates, bitrate):
    epoch_rates = {}
    for epoch, group in groupby(frames, key=lambda packet: packet['epoch']):
        batch = list(group)
        assert batch[0]['key'] == 1 and epoch not in epoch_rates, epoch
        rate = encoder_rates[epoch]
        assert 1_000 <= rate <= bitrate, (epoch, rate)
        epoch_rates[epoch] = rate
        image = decoded(batch[:6])
        w, h = image.size
        assert image.getpixel((w // 2, h // 4))[0] > 225
        assert image.getpixel((w // 2, 3 * h // 4))[2] > 225
    assert any(not packet['key'] for packet in frames)
    return list(epoch_rates.items())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--nvenc', action='store_true', help='exercise decoder epochs across bitrate changes on an NVIDIA host')
    parser.add_argument('--two-windows', action='store_true', help='exercise the shared pacing budget across two NVENC windows')
    options = parser.parse_args()
    nvenc = options.nvenc or options.two_windows
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
    case = 'nvenc-two' if options.two_windows else 'nvenc' if nvenc else ''
    log_path = BUILD / (f'udp-adaptive-{case}.log' if case else 'udp-adaptive.log')
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
            run(wine, Crypto(), log, log_path, nvenc, options.two_windows)
        finally:
            if read_fd is not None:
                os.close(read_fd)
            if wine:
                subprocess.run(['wineserver', '-k'], env=wine, stdout=log, stderr=log, timeout=10)
            stop(xvfb)


if __name__ == '__main__':
    main()
