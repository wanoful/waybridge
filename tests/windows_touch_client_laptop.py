#!/usr/bin/env python3
"""Opt-in real Windows PT_TOUCH through production client and disposable compositor."""
import argparse
import base64
import json
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time
import uuid
from session_fixture import SessionFixture
from smoke import ROOT


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', required=True)
    parser.add_argument('--client', type=Path, default=ROOT/'build/windows/waybridge-client.exe')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', args.host):
        raise ValueError('Use an SSH host alias/name')
    key = 'Waybridge-touch-client-' + uuid.uuid4().hex[:12]
    build = ROOT/'build/tests/touch-client-fixture'
    build.mkdir(parents=True, exist_ok=True)
    # Existing generated protocols are read only; do not regenerate the shared fixtures.
    protocols = ['xdg-shell', 'xdg-decoration', 'cursor-shape', 'tablet', 'xdg-dialog', 'xdg-activation']
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Ibuild/tests',
                    'tests/windows_touch_client_app.c',
                    *[str(ROOT/'build/tests'/f'{name}-protocol.c') for name in protocols],
                    '-o', str(build/'app'),
                    *subprocess.check_output(['pkg-config', '--cflags', '--libs', 'wayland-client'], text=True).split()],
                   cwd=ROOT, check=True)
    subprocess.run(['x86_64-w64-mingw32-g++', '-std=c++17', '-municode', '-Wall', '-Wextra', '-Werror',
                    '-static', '-O2', '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00', '-Iclient',
                    'tests/windows_touch_client_probe.cpp', '-o', str(build/'driver.exe')], cwd=ROOT, check=True)

    def remote(code, check=True):
        encoded = base64.b64encode(code.encode('utf-16-le')).decode()
        result = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', args.host,
                                 'powershell -NoProfile -NonInteractive -EncodedCommand ' + encoded],
                                capture_output=True, text=True, errors='replace', timeout=35)
        if check:
            assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout.strip()

    destination = remote('[IO.Path]::GetTempPath()').rstrip('\\') + '\\' + key
    quoted = destination.replace("'", "''")
    children = []
    evidence = ROOT/'build/tests/touch-client-laptop'
    evidence.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='wb-touch-client-') as directory, \
            SessionFixture(Path(directory)/'fixture') as fixture, \
            (evidence/'server.log').open('w+') as server_log, (evidence/'app.log').open('w+') as app_log:
        runtime = Path(fixture.env['XDG_RUNTIME_DIR'])
        token = 'touch-client-' + uuid.uuid4().hex
        token_path = runtime/'touch-token'
        token_path.write_text(token)
        token_path.chmod(0o600)
        with socket.socket() as free:
            free.bind(('127.0.0.1', 0))
            port = free.getsockname()[1]
        env = {**fixture.env, 'WAYLAND_DISPLAY': 'touch-client-0',
               'WAYBRIDGE_TOUCH_FIXTURE_TITLE': key, 'WAYBRIDGE_TEST_SEMANTICS': '1',
               'RUST_LOG': 'waybridge=debug'}
        try:
            server = subprocess.Popen([str(ROOT/'target/debug/waybridge'), '--socket', 'touch-client-0', '--listen',
                                       f'127.0.0.1:{port}', '--token-file', str(token_path), '--codec', 'png',
                                       '--no-audio', '--no-xwayland'], env=env, stdout=server_log, stderr=server_log)
            children.append(server)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline and not (runtime/'touch-client-0').exists() and server.poll() is None:
                time.sleep(.03)
            assert server.poll() is None and (runtime/'touch-client-0').exists(), (evidence/'server.log').read_text()
            app = subprocess.Popen([str(build/'app')], env=env, stdout=app_log, stderr=app_log)
            children.append(app)
            remote("[IO.Directory]::CreateDirectory('" + quoted + "') | Out-Null")
            for source, name in [(args.client, 'viewer.exe'), (build/'driver.exe', 'driver.exe')]:
                subprocess.run(['scp', '-q', str(source), args.host + ':' + destination.replace('\\', '/') + '/' + name],
                               check=True, timeout=35)
            remote_port = int(remote('$l=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$l.Start();$p=$l.LocalEndpoint.Port;$l.Stop();$p'))
            tunnel = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes', '-N', '-R',
                                       f'127.0.0.1:{remote_port}:127.0.0.1:{port}', args.host], stdout=server_log, stderr=server_log)
            children.append(tunnel)
            time.sleep(.3)
            assert tunnel.poll() is None
            remote(r'''
$ErrorActionPreference='Stop';$root='@ROOT@';$name='@KEY@'
$run=@'
$ErrorActionPreference='Stop';$root=$PSScriptRoot;$worker=$null
$env:WAYBRIDGE_DPI='96';$env:WAYBRIDGE_NO_AUDIO='1'
Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public static class Foreground { [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd); }'
$previous=[Foreground]::GetForegroundWindow()
try {
 $worker=Start-Process ($root+'\viewer.exe') -NoNewWindow -ArgumentList @('127.0.0.1','@PORT@','@TOKEN@','--touch') -PassThru -RedirectStandardOutput ($root+'\viewer.txt') -RedirectStandardError ($root+'\viewer-err.txt')
 Start-Sleep -Seconds 2
 $driver=Start-Process ($root+'\driver.exe') -NoNewWindow -ArgumentList @([string]$worker.Id,'@KEY@') -PassThru -Wait -RedirectStandardOutput ($root+'\driver.txt') -RedirectStandardError ($root+'\driver-err.txt')
 $driver.ExitCode|Set-Content -Encoding ASCII ($root+'\exit.txt')
} catch { $_|Out-String|Set-Content ($root+'\failure.txt');'99'|Set-Content ($root+'\exit.txt') }
finally { if($worker){Stop-Process -Id $worker.Id -Force -ErrorAction SilentlyContinue};[Foreground]::SetForegroundWindow($previous)|Out-Null }
'@
$path=Join-Path $root 'run.ps1';$run|Set-Content -Encoding UTF8 $path
$action=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "'+$path+'"') -WorkingDirectory $root
$principal=New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName $name -Action $action -Principal $principal | Out-Null
Start-ScheduledTask -TaskName $name
'''.replace('@ROOT@', quoted).replace('@KEY@', key).replace('@PORT@', str(remote_port)).replace('@TOKEN@', token))
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                output = remote("$p='" + quoted + "';if(Test-Path ($p+'\\exit.txt')){Get-Content ($p+'\\exit.txt')}")
                if output:
                    inventory = remote("$p='" + quoted + "';if(Test-Path ($p+'\\driver.txt')){Get-Content ($p+'\\driver.txt')}")
                    evidence.joinpath('result.json').write_text(inventory + '\n')
                    details = remote("Get-ChildItem '" + quoted + "\\*.txt'|ForEach-Object{$_.Name;Get-Content $_.FullName}")
                    evidence.joinpath('windows.log').write_text(details + '\n')
                    assert output == '0', details
                    result = json.loads(inventory)
                    assert result['ok'] is True, result
                    text = (evidence/'app.log').read_text()
                    assert text.count('CLIENT_TOUCH_DOWN ') == 4, text
                    assert text.count('CLIENT_REQUEST_MOVE\n') == 1 and text.count('CLIENT_REQUEST_RESIZE\n') == 1, text
                    assert text.count('CLIENT_TOUCH_UP ') >= 2 and 'CLIENT_TOUCH_MOTION ' in text, text
                    assert app.poll() is None and server.poll() is None
                    print(json.dumps(result, indent=2))
                    print('PASS: real PT_TOUCH through production client -> live wl_touch, owner-preserving xdg move/resize, second-finger isolation and fresh touch recovery')
                    return
                time.sleep(.5)
            raise AssertionError('Production touch injection timed out; see ' + str(evidence))
        finally:
            remote("$root='" + quoted + "';$name='" + key + "';Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue|Unregister-ScheduledTask -Confirm:$false;Get-CimInstance Win32_Process|Where-Object{$_.ExecutablePath -and $_.ExecutablePath.StartsWith($root+'\\',[StringComparison]::OrdinalIgnoreCase)}|ForEach-Object{Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue};Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue", check=False)
            for child in reversed(children):
                if child.poll() is None:
                    child.terminate()
            for child in children:
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()


if __name__ == '__main__':
    main()
