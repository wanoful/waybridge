#!/usr/bin/env python3
"""Private Precision Touchpad probe in the Windows user's interactive session."""
import argparse
import base64
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
    executable = ROOT / 'build/tests/windows-touchpad-probe.exe'
    executable.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['x86_64-w64-mingw32-g++', '-static', '-O2', '-std=c++17', '-Wall', '-Wextra',
                    '-Werror', '-D_WIN32_WINNT=0x0A00', '-DUNICODE', '-D_UNICODE', '-DNOMINMAX',
                    '-Iclient', 'tests/windows_touchpad_probe.cpp', '-o', executable, '-lole32',
                    '-luuid'], cwd=ROOT, check=True)

    def remote(code, check=True):
        encoded = base64.b64encode(code.encode('utf-16-le')).decode()
        command = 'powershell.exe -NoProfile -NonInteractive -EncodedCommand ' + encoded
        result = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=8', args.host,
                                 command], capture_output=True, text=True, encoding='utf-8',
                                errors='replace', timeout=35, check=check)
        return result.stdout.strip()

    name = 'Waybridge-touchpad-' + uuid.uuid4().hex[:12]
    destination = remote('[IO.Path]::GetTempPath()').rstrip('\\') + '\\' + name
    quoted = destination.replace("'", "''")
    common = "$root='" + quoted + "';$name='" + name + "';"
    try:
        remote(common + 'New-Item -ItemType Directory -Path $root | Out-Null')
        subprocess.run(['scp', '-q', executable, args.host + ':' + destination.replace('\\', '/') + '/probe.exe'],
                       check=True, timeout=35)
        remote(common + r'''
$ErrorActionPreference='Stop'
$script=@'
$ErrorActionPreference='Stop'
$p=Start-Process -FilePath (Join-Path $PSScriptRoot 'probe.exe') @ARGUMENT@ -NoNewWindow -Wait -PassThru -RedirectStandardOutput (Join-Path $PSScriptRoot 'out.txt') -RedirectStandardError (Join-Path $PSScriptRoot 'err.txt')
$p.ExitCode | Set-Content -Encoding ASCII (Join-Path $PSScriptRoot 'exit.txt')
'@
$runner=Join-Path $root 'probe.ps1'
$script | Set-Content -Encoding UTF8 $runner
$action=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "'+$runner+'"') -WorkingDirectory $root
$principal=New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName $name -Action $action -Principal $principal | Out-Null
Start-ScheduledTask -TaskName $name
'''.replace('@ARGUMENT@', "-ArgumentList '--inject-into-private-window'" if args.exercise else ''))
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            output = remote(common + r'''
if(Test-Path ($root+'\exit.txt')) {
  Get-Content ($root+'\exit.txt')
  Get-Content ($root+'\out.txt')
  Get-Content ($root+'\err.txt')
}
''')
            if output:
                lines = output.lstrip('\ufeff').splitlines()
                print('\n'.join(lines[1:]))
                if lines[0] != '0':
                    raise RuntimeError('Touchpad probe exited ' + lines[0])
                return
            time.sleep(.4)
        raise TimeoutError('Interactive Windows touchpad probe did not finish')
    finally:
        remote(common + r'''
if(Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue){
  Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
  Unregister-ScheduledTask -TaskName $name -Confirm:$false
}
Get-CimInstance Win32_Process | Where-Object {$_.ExecutablePath -eq ($root+'\probe.exe')} |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
''', check=False)


if __name__ == '__main__':
    main()
