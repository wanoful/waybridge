#!/usr/bin/env python3
"""Actual apps inherit a private display/bus/IME/audio environment from a terminal.

--apps exercises Chrome/HMCL/VLC. --audio plays mpv only after confirming its
paused native PipeWire stream targets our null sink, never the default output.
"""
import json
import math
import os
from pathlib import Path
import signal
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import wave

from smoke import ROOT, Viewer
from audio_smoke import packet, send


def wait(check, seconds=20):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        value = check()
        if value:
            return value
        time.sleep(.03)
    raise AssertionError('application launch timed out')


def child(directory):
    directory = Path(directory)
    variables = ('DISPLAY', 'WAYLAND_DISPLAY', 'QT_QPA_PLATFORM', 'GDK_BACKEND',
                 'QT_IM_MODULE', 'GTK_IM_MODULE', 'SDL_IM_MODULE', 'GLFW_IM_MODULE',
                 'DBUS_SESSION_BUS_ADDRESS', 'PULSE_SINK', 'PIPEWIRE_NODE', 'PIPEWIRE_PROPS')
    (directory / 'environment.json').write_text(json.dumps({k: os.environ.get(k) for k in variables}))
    children = []
    try:
        for line in sys.stdin:
            item = json.loads(line)
            if item['op'] == 'quit':
                break
            if item['op'] == 'spawn':
                with (directory / (item['name'] + '.log')).open('w') as output:
                    process = subprocess.Popen(item['args'], env={**os.environ, **item.get('env', {})},
                                               cwd=directory, stdout=output, stderr=output,
                                               start_new_session=True)
                children.append(process)
                (directory / (item['name'] + '.pid')).write_text(str(process.pid))
    finally:
        for process in reversed(children):
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=4)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=4)


def main():
    if '--child' in sys.argv:
        child(sys.argv[-1])
        return
    mode = 'apps' if '--apps' in sys.argv else 'audio' if '--audio' in sys.argv else 'ime'
    (ROOT / 'build/tests').mkdir(parents=True, exist_ok=True)
    before = subprocess.check_output(['pactl', 'get-default-sink'], text=True).strip()
    pulse = next(line.split(': ', 1)[1] for line in subprocess.check_output(['pactl', 'info'], text=True).splitlines() if line.startswith('Server String:'))
    with tempfile.TemporaryDirectory(prefix='waybridge-launch-') as directory, (ROOT / f'build/tests/launch-{mode}.log').open('w') as log:
        runtime = Path(directory)
        token = 'launch-test-' + '8' * 40
        token_path = runtime / 'token'; token_path.write_text(token); token_path.chmod(0o600)
        env = {**os.environ, 'XDG_RUNTIME_DIR': directory, 'PULSE_SERVER': pulse,
               'PIPEWIRE_RUNTIME_DIR': os.environ.get('PIPEWIRE_RUNTIME_DIR', f'/run/user/{os.getuid()}'),
               'QT_IM_MODULE': 'fcitx', 'GTK_IM_MODULE': 'fcitx', 'SDL_IM_MODULE': 'fcitx',
               'GLFW_IM_MODULE': 'fcitx', 'XMODIFIERS': '@im=fcitx',
               'XDG_CONFIG_HOME': str(runtime / 'config'), 'XDG_CACHE_HOME': str(runtime / 'cache'),
               'XDG_DATA_HOME': str(runtime / 'data')}
        with socket.socket() as free:
            free.bind(('127.0.0.1', 0)); port = free.getsockname()[1]
        host = subprocess.Popen([str(ROOT / 'target/debug/waybridge'), '--listen', f'127.0.0.1:{port}',
                                 '--socket', 'launch-0', '--token-file', str(token_path), '--codec', 'png',
                                 '--', sys.executable, str(Path(__file__).resolve()), '--child', directory],
                                cwd=ROOT, env=env, stdin=subprocess.PIPE, text=True, stdout=log, stderr=log)
        viewer = audio = None
        try:
            wait(lambda: (runtime / 'environment.json').exists() or host.poll() is not None)
            assert host.poll() is None, 'host failed; see launch log'
            inherited = json.loads((runtime / 'environment.json').read_text())
            assert inherited['DISPLAY'].startswith(':')
            assert inherited['PULSE_SINK'] == inherited['PIPEWIRE_NODE'] == f'waybridge-{host.pid}'
            assert inherited['DBUS_SESSION_BUS_ADDRESS'] == env.get('DBUS_SESSION_BUS_ADDRESS')
            assert all(inherited[name] is None for name in ('QT_IM_MODULE', 'GTK_IM_MODULE', 'SDL_IM_MODULE', 'GLFW_IM_MODULE'))
            snapshot = 'import json,os,sys;print(json.dumps({k:os.environ.get(k) for k in sys.argv[1:]}))'
            joined = json.loads(subprocess.check_output([str(ROOT / 'target/debug/waybridge'), '--socket', 'launch-0', '--launch', '--',
                                                        sys.executable, '-c', snapshot, *inherited.keys()], env=env, text=True))
            assert joined == inherited, 'external SSH-style launch must inherit the same display, bus, IME and audio settings'
            descriptor = runtime / 'launch-0.session.json'
            saved = descriptor.read_bytes()
            stale = json.loads(saved); stale['started'] = 'stale'
            descriptor.write_text(json.dumps(stale))
            marker = runtime / 'should-not-launch'
            rejected = subprocess.run([str(ROOT / 'target/debug/waybridge'), '--socket', 'launch-0', '--launch', '--', sys.executable, '-c', 'from pathlib import Path;Path(__import__("sys").argv[1]).touch()', str(marker)], env=env, capture_output=True)
            assert rejected.returncode != 0 and not marker.exists(), 'stale environment must not launch an application'
            descriptor.write_bytes(saved)
            viewer = Viewer(port, token, dpi=168)

            def spawn(name, args, extra=None):
                host.stdin.write(json.dumps(dict(op='spawn', name=name, args=args, env=extra or {})) + '\n'); host.stdin.flush()
                return int(wait(lambda: (runtime / (name + '.pid')).read_text() if (runtime / (name + '.pid')).exists() else None))

            def key(window, code):
                viewer.send(type='key', id=window, key=code, down=True)
                viewer.send(type='key', id=window, key=code, down=False)

            if mode == 'apps':
                apps = [
                    ('chrome', ['google-chrome-stable', '--no-first-run', '--no-default-browser-check', '--disable-background-networking', '--user-data-dir=' + str(runtime / 'chrome-profile'), '--app=data:text/html,<title>Waybridge Chrome startup</title><input autofocus>'], {}),
                    ('hmcl', ['hmcl-bin'], {'JAVA_TOOL_OPTIONS': '-Duser.home=' + str(runtime)}),
                    ('vlc', ['vlc', '--no-media-library'], {}),
                ]
                for name, args, extra in apps:
                    spawn(name, args, extra)
                    info = viewer.until(lambda k, v: k == 'control' and v['type'] == 'window' and
                                        (('Chrome startup' in v.get('title', '')) if name == 'chrome' else
                                         ('hmcl' in (v.get('app_id', '') + v.get('title', '')).lower()) if name == 'hmcl' else
                                         ('vlc' in (v.get('app_id', '') + v.get('title', '')).lower())))
                    assert info['width'] > 0 and info['height'] > 0
                    viewer.until(lambda k, v: k == 'frame' and v[0] == info['id'])
                    print(f'PASS: actual {name} created and painted a remote window with inherited session defaults', flush=True)
                    viewer.send(type='close', id=info['id'])
            elif mode == 'audio':
                control = socket.create_connection(('127.0.0.1', port), timeout=8)
                # Release the ordinary viewer so the audio-capable viewer can authenticate.
                viewer.sock.close(); viewer = None; time.sleep(.2)
                send(control, type='hello', version=3, token=token, audio_pcm=True)
                kind, hello = packet(control); assert kind == 0 and hello['audio_pcm']
                audio = socket.create_connection(('127.0.0.1', port), timeout=8)
                send(audio, type='hello', version=3, token=token, channel='audio', media_key=hello['media_key'])
                assert packet(audio)[1]['type'] == 'audio_ready'
                wav = runtime / 'stereo.wav'
                pcm = b''.join(struct.pack('<hh', int(12000*math.sin(2*math.pi*997*i/48000)), int(6000*math.sin(2*math.pi*2003*i/48000))) for i in range(48000*4))
                with wave.open(str(wav), 'wb') as output:
                    output.setparams((2, 2, 48000, 0, 'NONE', 'not compressed')); output.writeframes(pcm)
                ipc_path = runtime / 'mpv.sock'
                marker = f'WaybridgeMPV-{host.pid}'
                spawn('mpv', ['mpv', '--no-config', '--no-video', '--pause', '--audio-client-name=' + marker, '--input-ipc-server=' + str(ipc_path), str(wav)])
                wait(ipc_path.exists)
                ipc = socket.socket(socket.AF_UNIX); ipc.connect(str(ipc_path)); ipc.settimeout(8)
                reader = ipc.makefile('rb')
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    ipc.sendall(b'{"command":["get_property","current-ao"],"request_id":1}\n')
                    while True:
                        reply = json.loads(reader.readline())
                        if reply.get('request_id') == 1: break
                    if reply.get('data'):
                        assert reply['data'] == 'pipewire', reply
                        break
                    time.sleep(.05)
                else: raise AssertionError('mpv audio did not initialize')
                def routed():
                    nodes = json.loads(subprocess.check_output(['pw-dump']))
                    owned = [s['info']['props'] for s in nodes if s.get('type') == 'PipeWire:Interface:Node' and s.get('info', {}).get('props', {}).get('node.name') == marker]
                    return owned and all(s.get('target.object') == inherited['PULSE_SINK'] and str(s.get('node.dont-fallback')).lower() == 'true' for s in owned)
                assert wait(routed), 'paused native stream must target our null sink and forbid fallback before playback'
                ipc.sendall(b'{"command":["set_property","pause",false]}\n')
                peak = 0
                for _ in range(150):
                    kind, body = packet(audio); assert kind == 3
                    sequence = struct.unpack('>Q', body[12:20])[0]
                    peak = max(peak, max(map(abs, struct.unpack('<960h', body[28:]))))
                    send(audio, type='audio_ack', sequence=sequence)
                assert peak > 2000, peak
                reader.close(); ipc.close(); control.close()
                print(f'PASS: actual mpv default native PipeWire audio reaches private streamed PCM (peak {peak}); no default output change', flush=True)
            else:
                start = runtime / 'dolphin-start'; start.mkdir()
                target = runtime / '输入法测试'; target.mkdir()
                spawn('dolphin', ['dolphin', '--new-window', str(start)], {'WAYLAND_DEBUG': '1'})
                info = viewer.until(lambda k, v: k == 'control' and v['type'] == 'window' and v.get('app_id') == 'org.kde.dolphin')
                window = info['id']; assert not info.get('x11'), 'Dolphin should prefer native Wayland'
                viewer.until(lambda k, v: k == 'frame' and v[0] == window)
                time.sleep(.2)
                viewer.send(type='focus', id=window, focused=True)
                viewer.send(type='key', id=window, key=29, down=True); key(window, 38)
                viewer.send(type='key', id=window, key=29, down=False)
                state = viewer.until(lambda k, v: k == 'control' and v['type'] == 'ime_state' and v.get('enabled') and v['id'] == window)
                viewer.send(type='ime', id=window, generation=state['generation'], preedit='ceshi', cursor_begin=5, cursor_end=5)
                viewer.send(type='ime', id=window, generation=state['generation'], preedit='', commit=str(target) + 'x')
                key(window, 14); key(window, 28)
                viewer.until(lambda k, v: k == 'control' and v['type'] == 'window' and v['id'] == window and '输入法测试' in v['title'])
                print('PASS: real Dolphin enables text-input-v3 despite inherited fcitx overrides; Chinese commit then raw Backspace/Enter navigates correctly', flush=True)
            assert subprocess.check_output(['pactl', 'get-default-sink'], text=True).strip() == before
        finally:
            if audio: audio.close()
            if viewer: viewer.sock.close()
            if host.poll() is None:
                try:
                    host.stdin.write('{"op":"quit"}\n'); host.stdin.flush(); time.sleep(.2)
                except (BrokenPipeError, OSError): pass
                host.terminate()
                try: host.wait(timeout=8)
                except subprocess.TimeoutExpired: host.kill(); host.wait(timeout=5)
            assert not (runtime / 'launch-0.session.json').exists(), 'owned session descriptor must be removed on exit'
            for path in runtime.glob('*.log'):
                shutil.copyfile(path, ROOT / ('build/tests/launch-' + mode + '-' + path.name))


if __name__ == '__main__':
    main()
