#!/usr/bin/env python3
"""Opt-in native Explorer acceptance; only a random self-cleaning probe/task is created."""
import base64
from pathlib import Path, PureWindowsPath
import subprocess
import time
import uuid
from smoke import ROOT

def main():
    import argparse
    p=argparse.ArgumentParser();p.add_argument('--host',default='laptop');p.add_argument('--probe',type=Path,default=ROOT/'build/tests/windows-tray-probe.exe');args=p.parse_args()
    executable=args.probe
    if not executable.is_file():raise RuntimeError('Run tests/windows_tray_smoke.py to build the native probe first')
    name='WaybridgeTrayProbe-'+uuid.uuid4().hex
    def ps(code):
        code="$ErrorActionPreference='Stop'; [Console]::OutputEncoding=[System.Text.UTF8Encoding]::new($false); "+code
        encoded=base64.b64encode(code.encode('utf-16-le')).decode()
        r=subprocess.run(['ssh','-o','ConnectTimeout=10',args.host,'powershell -NoProfile -NonInteractive -EncodedCommand '+encoded],capture_output=True,text=True,encoding="utf-8",errors="replace",timeout=30)
        if r.returncode:raise RuntimeError(r.stderr)
        return r.stdout.strip()
    def quoted(value):return "'"+value.replace("'","''")+"'"
    print("task="+name,flush=True)
    root=ps('$p=Join-Path $env:TEMP '+quoted(name)+'; New-Item -ItemType Directory -Path $p | Out-Null; $p')
    target=str(PureWindowsPath(root)/'probe.exe');report=str(PureWindowsPath(root)/'result.txt')
    try:
        subprocess.run(['scp',str(executable),args.host+':'+target.replace(chr(92),'/')],check=True,timeout=30)
        ps('$a=New-ScheduledTaskAction -Execute '+quoted(target)+' -Argument '+quoted('"'+report+'"')+' -WorkingDirectory '+quoted(root)+'; '
           '$p=New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited; '
           '$s=New-ScheduledTaskSettingsSet -ExecutionTimeLimit (New-TimeSpan -Minutes 1); '
           'Register-ScheduledTask -TaskName '+quoted(name)+' -Action $a -Principal $p -Settings $s | Out-Null; Start-ScheduledTask -TaskName '+quoted(name))
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            result=ps('if(Test-Path '+quoted(report)+'){Get-Content -Raw '+quoted(report)+'}')
            if 'shell_rect=' in result:break
            time.sleep(.5)
        print(result)
        print(ps('Get-ScheduledTaskInfo -TaskName '+quoted(name)+' | Select-Object LastTaskResult,LastRunTime | ConvertTo-Json -Compress'))
        assert 'activate=1 secondary=1 menu_checked=1 menu_clicked=1 shell_rect=1 readonly=1 disabled=1 cleared=1 missing=1' in result,result
        print('PASS: native Windows Explorer Shell icon accepted, real popup menu/callback, settings and cleanup')
    finally:
        ps('$task=Get-ScheduledTask -TaskName '+quoted(name)+' -ErrorAction SilentlyContinue; if($task){Stop-ScheduledTask -TaskName '+quoted(name)+' -ErrorAction SilentlyContinue; Unregister-ScheduledTask -TaskName '+quoted(name)+' -Confirm:$false}; Remove-Item -Recurse -Force '+quoted(root)+' -ErrorAction SilentlyContinue')
if __name__=='__main__':main()
