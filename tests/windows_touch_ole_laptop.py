#!/usr/bin/env python3
"""Private limited-interactive runtime probe; no installed program/profile changes."""
import argparse
import base64
import json
from pathlib import Path
import re
import subprocess
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', required=True)
    parser.add_argument('--exercise', action='store_true')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]*', args.host):
        raise ValueError('Use an SSH host alias/name')
    executable = ROOT / 'build/tests/windows-touch-ole-probe.exe'
    executable.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['x86_64-w64-mingw32-g++', '-static', '-O2', '-std=c++17', '-Wall', '-Wextra',
                    '-Werror', '-D_WIN32_WINNT=0x0A00', '-Iclient', 'tests/windows_touch_ole_probe.cpp',
                    '-o', str(executable), '-lversion', '-ladvapi32', '-lole32', '-luuid', '-lgdi32', '-lshell32'], cwd=ROOT, check=True)
    def remote(code, check=True):
        encoded = base64.b64encode(code.encode('utf-16-le')).decode()
        return subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', args.host,
                               'powershell -NoProfile -NonInteractive -EncodedCommand ' + encoded],
                              capture_output=True, text=True, encoding='utf-8', errors='replace',
                              timeout=35, check=check)
    key = 'Waybridge-touch-ole-' + uuid.uuid4().hex[:12]
    temporary = remote('[IO.Path]::GetTempPath()').stdout.strip()
    assert temporary and '\\' in temporary
    destination = temporary.rstrip('\\') + '\\' + key
    quoted = destination.replace("'", "''")
    try:
        remote("[IO.Directory]::CreateDirectory('" + quoted + "') | Out-Null")
        subprocess.run(['scp', '-q', str(executable), args.host + ':' + destination.replace('\\', '/') + '/probe.exe'],
                       check=True, timeout=35)
        remote(r'''
$ErrorActionPreference='Stop';$root='@ROOT@';$name='@TASK@'
$run=@'
$ErrorActionPreference='Stop'
try {
 $p=Start-Process -FilePath (Join-Path $PSScriptRoot 'probe.exe') @ARGUMENT@ -NoNewWindow -Wait -PassThru -RedirectStandardOutput (Join-Path $PSScriptRoot 'stdout.txt') -RedirectStandardError (Join-Path $PSScriptRoot 'stderr.txt')
 $code=$p.ExitCode
} catch { $_ | Out-String | Set-Content -Encoding UTF8 (Join-Path $PSScriptRoot 'stderr.txt');$code=99 }
$code | Set-Content -Encoding ASCII (Join-Path $PSScriptRoot 'exit.txt')
'@
$path=Join-Path $root 'run.ps1';$run|Set-Content -Encoding UTF8 $path
$action=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -ExecutionPolicy Bypass -File "'+$path+'"') -WorkingDirectory $root
$principal=New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName $name -Action $action -Principal $principal | Out-Null
Start-ScheduledTask -TaskName $name
'''.replace('@ROOT@', quoted).replace('@TASK@', key).replace('@ARGUMENT@', "-ArgumentList '--exercise'" if args.exercise else ''))
        deadline = time.monotonic() + 50
        while time.monotonic() < deadline:
            result = remote("$p='" + quoted + "';if(Test-Path ($p+'\\exit.txt')) { Get-Content ($p+'\\exit.txt');Get-Content ($p+'\\stdout.txt');Get-Content ($p+'\\stderr.txt') }")
            output = result.stdout.strip()
            if output:
                inventory = json.loads(output.splitlines()[1])
                assert inventory.get('elevated') is False, inventory
                path = ROOT / 'build/tests/windows-touch-ole-laptop.json'
                path.write_text(json.dumps(inventory, indent=2) + '\n')
                print(json.dumps(inventory, indent=2))
                if output.splitlines()[0].strip() != '0':
                    exercise = inventory.get('exercise', {})
                    if exercise.get('blocked'):
                        raise RuntimeError(exercise.get('reason', 'Private fixture is not exposed') +
                                           '; unlock the laptop desktop and rerun --exercise')
                    raise AssertionError('Touch OLE exercise failed; see ' + str(path))
                return
            time.sleep(1)
        raise AssertionError('Touch OLE capability probe timed out; interactive desktop required')
    finally:
        remote("$root='" + quoted + "';$name='" + key + "';if(Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue){Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue;Unregister-ScheduledTask -TaskName $name -Confirm:$false};Get-CimInstance Win32_Process|Where-Object{$_.ExecutablePath -eq ($root+'\\probe.exe')}|ForEach-Object{Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue};Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue", check=False)


if __name__ == '__main__':
    main()
