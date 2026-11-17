#!/usr/bin/env python3
"""Opt-in actual WinUI touch/pen/gamepad settings and private save/reload.

Uses the actual UI sources in GuiFocus.Probe's settings-only mode. Creates a
unique limited interactive task and private state; never connects a server,
reads real profiles/credentials, injects touch, or replaces an installed app.
"""
import argparse
import base64
import json
from pathlib import Path
import subprocess
import time
import uuid
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', required=True)
    parser.add_argument('--publish-directory', type=Path, default=ROOT / 'build/gui-settings-probe-x64')
    args = parser.parse_args()
    publish = args.publish_directory.resolve(strict=True)
    assert (publish / 'Waybridge.GuiFocus.Probe.exe').is_file()
    worker = ROOT / 'build/windows/waybridge-client.exe'
    assert worker.is_file(), 'worker is needed only for the actual GUI audio-device enumeration'
    key = 'Waybridge-gui-focus-probe-settings-' + uuid.uuid4().hex[:12]
    logfile = ROOT / 'build/tests/touch-settings-laptop.log'
    logfile.parent.mkdir(parents=True, exist_ok=True)
    archive = ROOT / 'build' / f'{key}.zip'

    def remote(code, check=True):
        encoded = base64.b64encode(code.encode('utf-16-le')).decode()
        result = subprocess.run(['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', args.host,
                                 'powershell -NoProfile -NonInteractive -EncodedCommand ' + encoded],
                                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=35)
        if check:
            assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout.strip()

    destination = remote('[IO.Path]::GetTempPath()').rstrip('\\') + '\\' + key
    profile_id = str(uuid.uuid4())
    profile = dict(id=profile_id, name='Disposable touch settings fixture', mode='tunnel', host='127.0.0.1',
                   port=49090, auto_reconnect=False,
                   settings=dict(codec='png', fps=30, audio=False, notifications=False, tray=False,
                                 clipboard='off', files=False, drag_text=False, ime=False, read_only=True))
    # Omit touch/pen/gamepad fields to verify previous-version defaults.
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=1) as bundle:
        for path in publish.rglob('*'):
            if path.is_file() and path.name != 'waybridge-client.exe':
                bundle.write(path, path.relative_to(publish))
        bundle.write(worker, 'waybridge-client.exe')
        bundle.writestr('state/profiles.json', json.dumps([profile]))
        bundle.writestr('profile-id.txt', profile_id)
        bundle.writestr('settings-only', 'offline native touch settings probe\n')
    try:
        subprocess.run(['scp', '-q', str(archive), args.host + ':' + (destination + '.zip').replace('\\', '/')],
                       check=True, timeout=150)
        remote("Expand-Archive -LiteralPath '" + destination + ".zip' -DestinationPath '" + destination + "'")
        setup = r'''
$ErrorActionPreference='Stop'; $root='@ROOT@';$name='@NAME@'
$run=@'
$ErrorActionPreference='Stop';$root=$PSScriptRoot
try {
 $p=Start-Process -FilePath ($root+'\Waybridge.GuiFocus.Probe.exe') -ArgumentList ('"'+$root+'"') -WorkingDirectory $root -NoNewWindow -Wait -PassThru -RedirectStandardOutput ($root+'\stdout.txt') -RedirectStandardError ($root+'\stderr.txt')
 $p.ExitCode | Set-Content -Encoding ASCII ($root+'\exit.txt')
} catch { $_.Exception.ToString()|Set-Content ($root+'\stderr.txt');'99'|Set-Content ($root+'\exit.txt') }
'@
$path=$root+'\run.ps1';$run|Set-Content -Encoding UTF8 $path
$act=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -ExecutionPolicy Bypass -File "'+$path+'"') -WorkingDirectory $root
$principal=New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName $name -Action $act -Principal $principal | Out-Null
Start-ScheduledTask -TaskName $name
'''.replace('@ROOT@', destination).replace('@NAME@', key)
        remote(setup)
        deadline = time.monotonic() + 70
        result = ''
        while time.monotonic() < deadline:
            result = remote("$p='" + destination + "';if(Test-Path ($p+'\\exit.txt')){Get-Content ($p+'\\exit.txt');Get-Content ($p+'\\result.txt') -ErrorAction SilentlyContinue;Get-Content ($p+'\\stderr.txt') -ErrorAction SilentlyContinue;Get-Content ($p+'\\startup.log') -ErrorAction SilentlyContinue}")
            if result:
                break
            time.sleep(1)
        if not result:
            result = remote("$p='" + destination + "';Get-Content ($p+'\\result.txt') -ErrorAction SilentlyContinue;Get-Content ($p+'\\stderr.txt') -ErrorAction SilentlyContinue;Get-Content ($p+'\\startup.log') -ErrorAction SilentlyContinue")
            raise AssertionError('GUI settings probe timed out: ' + result)
        logfile.write_text(result + '\n')
        lines = result.splitlines()
        print('\n'.join(line for line in lines if line == '0' or line.startswith('PASS '))
              if lines and lines[0].strip() == '0' else result)
        assert result.splitlines()[0].strip() == '0', 'GUI touch settings test failed; inspect ' + str(logfile)
        assert 'PASS actual WinUI native-touch setting' in result
        assert 'PASS actual WinUI pen/gamepad controls' in result
        print('PASS: actual WinUI native-touch control renders, saves on/off, reloads and preserves unrelated settings without a connection')
        print('PASS: actual WinUI pen/gamepad controls persist calibration and independent policies; effective, missing-device, readonly and error states remain visible')
    finally:
        remote("$root='" + destination + "';Get-ScheduledTask -TaskName '" + key + "' -ErrorAction SilentlyContinue|Unregister-ScheduledTask -Confirm:$false;Get-CimInstance Win32_Process|Where-Object{$_.ExecutablePath -and $_.ExecutablePath.StartsWith($root+'\\',[StringComparison]::OrdinalIgnoreCase)}|ForEach-Object{Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue};Remove-Item -Recurse -Force -LiteralPath $root -ErrorAction SilentlyContinue;Remove-Item -Force -LiteralPath ($root+'.zip') -ErrorAction SilentlyContinue", check=False)
        archive.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
