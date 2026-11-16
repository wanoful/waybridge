#!/usr/bin/env python3
"""Opt-in physical Windows DND using owned Qt/Win32 fixtures and a temporary tunnel."""
import argparse
import base64
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import uuid
from smoke import ROOT
from drag_out_smoke import wait_for


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', required=True)
    args = parser.parse_args()
    key = 'Waybridge-data-drag-' + uuid.uuid4().hex[:12]
    def remote(code):
        encoded = base64.b64encode(code.encode('utf-16-le')).decode()
        result = subprocess.run(['ssh', '-o', 'BatchMode=yes', args.host,
            'powershell -NoProfile -NonInteractive -EncodedCommand ' + encoded],
            capture_output=True, text=True, errors='replace', timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout.strip()
    output = ROOT / 'build/tests/windows-data-drag-probe.exe'
    subprocess.run(['x86_64-w64-mingw32-g++', '-std=c++17', '-D_WIN32_WINNT=0x0A00', '-municode', '-static', '-O1',
        'tests/windows_data_drag_probe.cpp', '-o', str(output), '-lole32', '-lshell32', '-luuid'], cwd=ROOT, check=True)
    remote_root = remote('[IO.Path]::GetTempPath()').rstrip('\\') + '\\' + key
    remote("New-Item -ItemType Directory -Path '" + remote_root + "' | Out-Null")
    with tempfile.TemporaryDirectory(prefix='waybridge-physical-data-') as directory:
        root = Path(directory)
        token = root / 'token'
        token.write_text('physical-data-' + uuid.uuid4().hex)
        token.chmod(0o600)
        env = {**os.environ, 'XDG_RUNTIME_DIR': directory, 'WAYLAND_DISPLAY': 'physical-0',
            'QT_QPA_PLATFORM': 'wayland', 'QT_WAYLAND_DISABLE_WINDOWDECORATION': '1', 'GVFS_DISABLE_FUSE': '1'}
        with socket.socket() as free:
            free.bind(('127.0.0.1', 0)); port = free.getsockname()[1]
        children = []
        logpath = ROOT / 'build/tests/data-drag-laptop.log'
        with logpath.open('w+') as log, (root / 'app.log').open('w+') as app_log:
            try:
                server = subprocess.Popen([str(ROOT / 'target/debug/waybridge'), '--socket', 'physical-0', '--listen',
                    f'127.0.0.1:{port}', '--token-file', str(token), '--codec', 'png', '--no-audio', '--no-xwayland'],
                    env=env, stdout=log, stderr=log); children.append(server)
                wait_for(lambda: (root / 'physical-0').exists())
                app = subprocess.Popen([str(ROOT / 'build/tests/drag-widgets')], env=env, stdout=app_log, stderr=app_log); children.append(app)
                for source, name in [(ROOT / 'build/windows/waybridge-client.exe', 'viewer.exe'), (output, 'driver.exe')]:
                    subprocess.run(['scp', '-q', str(source), args.host + ':' + remote_root.replace('\\', '/') + '/' + name], check=True, timeout=30)
                remote_port = int(remote('$l=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$l.Start();$p=$l.LocalEndpoint.Port;$l.Stop();$p'))
                tunnel = subprocess.Popen(['ssh', '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes', '-N',
                    '-R', f'127.0.0.1:{remote_port}:127.0.0.1:{port}', args.host], stdout=log, stderr=log); children.append(tunnel)
                time.sleep(.5); assert tunnel.poll() is None
                code = r'''
$root='@ROOT@';$name='@NAME@'
$run=@'
$ErrorActionPreference='Stop';$root=$PSScriptRoot
$env:WAYBRIDGE_DND_TRACE='1'
# Isolate only the disposable probe windows from existing topmost user windows.
$env:WAYBRIDGE_PROBE_ISOLATE='1'
$worker=$null
try {
 $worker=Start-Process -FilePath ($root+'\viewer.exe') -ArgumentList @('127.0.0.1','@PORT@','@TOKEN@') -PassThru -RedirectStandardOutput ($root+'\worker.txt') -RedirectStandardError ($root+'\worker-err.txt')
 Start-Sleep -Seconds 2
 foreach($mode in @('internal','return','out','in')) {
  $done=$false
  for($i=0;$i -lt 30;$i++) {
   $p=Start-Process -FilePath ($root+'\driver.exe') -ArgumentList $mode -PassThru -Wait -RedirectStandardOutput ($root+'\'+$mode+'.txt') -RedirectStandardError ($root+'\'+$mode+'-err.txt')
   if($p.ExitCode -ne 3){$done=$true;break};Start-Sleep -Milliseconds 200
  }
  if(!$done -or $p.ExitCode -ne 0){throw ('Driver '+$mode+' exit '+$p.ExitCode)}
 }
 '0'|Set-Content ($root+'\exit.txt')
} catch { $_ | Out-String | Set-Content ($root+'\failure.txt');'1'|Set-Content ($root+'\exit.txt') }
finally { if($worker){Stop-Process -Id $worker.Id -Force -ErrorAction SilentlyContinue} }
'@
$path=$root+'\run.ps1';$run|Set-Content -Encoding UTF8 $path
$act=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -ExecutionPolicy Bypass -File "'+$path+'"') -WorkingDirectory $root
$principal=New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName $name -Action $act -Principal $principal | Out-Null
Start-ScheduledTask -TaskName $name
'''.replace('@ROOT@', remote_root).replace('@NAME@', key).replace('@PORT@', str(remote_port)).replace('@TOKEN@', token.read_text())
                remote(code)
                result = ''
                deadline = time.monotonic() + 100
                while time.monotonic() < deadline:
                    result = remote("$p='" + remote_root + "';if(Test-Path ($p+'\\exit.txt')){Get-Content ($p+'\\exit.txt');Get-ChildItem ($p+'\\*.txt')|Where-Object{$_.Name -ne 'exit.txt'}|ForEach-Object{$_.Name;Get-Content $_.FullName}}")
                    if result: break
                    time.sleep(1)
                log.write(result); log.flush()
                assert result and result.splitlines()[0] == '0', result + '\nQT events:\n' + (root / 'app.log').read_text()
                text = (root / 'app.log').read_text()
                assert text.count('OTHER ') >= 3 and 'Windows 你好' in text, text
                print('PASS: physical Windows production viewer, Qt cross-window capture, OLE-return, native Unicode out/in; owned fixture only')
                print('Transcript:', logpath)
            finally:
                remote("$root='" + remote_root + "';Get-ScheduledTask -TaskName '" + key + "' -ErrorAction SilentlyContinue|Unregister-ScheduledTask -Confirm:$false;Get-CimInstance Win32_Process|Where-Object{$_.ExecutablePath -eq ($root+'\\viewer.exe') -or $_.ExecutablePath -eq ($root+'\\driver.exe')}|ForEach-Object{Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue};Remove-Item -Recurse -Force -LiteralPath $root -ErrorAction SilentlyContinue")
                for child in reversed(children):
                    if child.poll() is None: child.terminate()
                for child in children:
                    try: child.wait(timeout=10)
                    except subprocess.TimeoutExpired: child.kill();child.wait()


if __name__ == '__main__':
    main()
