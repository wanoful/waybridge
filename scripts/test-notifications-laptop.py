#!/usr/bin/env python3
"""Run the isolated native toast probe as the logged-in, limited Windows user.

Requires SSH/scp access to the explicitly named Windows machine. Never replaces
an installation, profile, credential or existing task. All remote files and the
uniquely named task are removed afterwards.
"""
import argparse
import base64
from pathlib import Path
import subprocess
import time
import uuid
import zipfile

ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--host',required=True)
    parser.add_argument('--publish-directory',type=Path,default=ROOT/'build/notifications-probe-x64')
    args=parser.parse_args()
    publish=args.publish_directory.resolve(strict=True)
    assert (publish/'Waybridge.Notifications.Probe.exe').is_file()
    key='Waybridge-notifications-probe-'+uuid.uuid4().hex[:12]
    def remote(code,check=True):
        encoded=base64.b64encode(code.encode('utf-16-le')).decode()
        return subprocess.run(['ssh','-o','BatchMode=yes','-o','ConnectTimeout=10',args.host,
                               'powershell -NoProfile -NonInteractive -EncodedCommand '+encoded],
                              check=check,capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=35)
    temporary=remote('[IO.Path]::GetTempPath()').stdout.strip()
    assert temporary and '\\' in temporary
    destination=temporary.rstrip('\\')+'\\'+key
    archive=ROOT/'build'/f'{key}.zip'
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=1) as zipped:
        for path in publish.rglob('*'):
            if path.is_file(): zipped.write(path,path.relative_to(publish))
    remote_zip=destination+'.zip'
    try:
        subprocess.run(['scp','-q',str(archive),args.host+':'+remote_zip.replace('\\','/')],check=True,timeout=120)
        setup=r'''
$ErrorActionPreference='Stop'
$root='@ROOT@'; $task='@TASK@'
Expand-Archive -LiteralPath ($root+'.zip') -DestinationPath $root
$script=@'
$ErrorActionPreference='Stop'
try {
  # Remove abandoned registrations only from this probe's reserved AppID prefix.
  Get-ChildItem 'HKCU:\Software\Classes\AppUserModelId' -ErrorAction SilentlyContinue | Where-Object { $_.PSChildName.StartsWith('Waybridge.Notifications.Probe.') } | ForEach-Object { & (Join-Path $PSScriptRoot 'Waybridge.Notifications.Probe.exe') --cleanup-id $_.PSChildName | Out-Null }
  $probe=Start-Process -FilePath (Join-Path $PSScriptRoot 'Waybridge.Notifications.Probe.exe') -WorkingDirectory $PSScriptRoot -NoNewWindow -PassThru -Wait -RedirectStandardOutput (Join-Path $PSScriptRoot 'stdout.txt') -RedirectStandardError (Join-Path $PSScriptRoot 'stderr.txt')
  $code=$probe.ExitCode
} catch { $_.Exception.ToString() | Set-Content -Encoding UTF8 (Join-Path $PSScriptRoot 'stderr.txt'); $code=99 }
$code | Set-Content -Encoding ASCII (Join-Path $PSScriptRoot 'exit.txt')
'@
$run=Join-Path $root 'run.ps1'; $script | Set-Content -Encoding UTF8 $run
$action=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -ExecutionPolicy Bypass -File "'+$run+'"') -WorkingDirectory $root
$principal=New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName $task -Action $action -Principal $principal | Out-Null
Start-ScheduledTask -TaskName $task
'''.replace('@ROOT@',destination.replace("'","''")).replace('@TASK@',key)
        result=remote(setup)
        deadline=time.monotonic()+100
        while time.monotonic()<deadline:
            result=remote("$p='"+destination+"'; if(Test-Path ($p+'\\exit.txt')) { Get-Content ($p+'\\exit.txt'); Get-Content ($p+'\\stdout.txt'); Get-Content ($p+'\\stderr.txt') }")
            if result.stdout.strip():
                output=result.stdout.strip()
                log=ROOT/'build/tests/windows-notifications-probe.log';log.parent.mkdir(parents=True,exist_ok=True);log.write_text(output+'\n')
                print(output)
                assert output.splitlines()[0].strip()=='0', 'native Windows notification probe failed; see '+str(log)
                return
            time.sleep(2)
        diagnostic=remote("$p='"+destination+"'; Get-Content ($p+'\\stdout.txt') -ErrorAction SilentlyContinue; Get-Content ($p+'\\stderr.txt') -ErrorAction SilentlyContinue; Get-ScheduledTaskInfo -TaskName '"+key+"' | Format-List LastTaskResult,LastRunTime")
        print(diagnostic.stdout)
        raise AssertionError('native Windows probe timed out; logged-in interactive desktop required')
    finally:
        cleanup="$root='"+destination+"'; $task='"+key+"'; if(Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue) { Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue; Unregister-ScheduledTask -TaskName $task -Confirm:$false }; Get-CimInstance Win32_Process | Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($root+'\\',[StringComparison]::OrdinalIgnoreCase) } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }; Remove-Item -Recurse -Force -LiteralPath $root -ErrorAction SilentlyContinue; Remove-Item -Force -LiteralPath ($root+'.zip') -ErrorAction SilentlyContinue"
        remote(cleanup,check=False);archive.unlink(missing_ok=True)

if __name__=='__main__': main()
