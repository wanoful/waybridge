#!/usr/bin/env python3
"""Opt-in Explorer event capture; moves the mouse, leaves user applications untouched."""
import argparse,base64,subprocess,uuid,time
from smoke import ROOT
parser=argparse.ArgumentParser(description="Opt-in real mouse clicks on an isolated notification icon only");parser.add_argument("--host",default="laptop");args=parser.parse_args()
subprocess.run(["x86_64-w64-mingw32-g++","-std=c++17","-O1","-static","-DUNICODE","-D_UNICODE","-D_WIN32_WINNT=0x0A00","tests/windows_tray_shell_events_probe.cpp","-o","build/tests/windows-tray-shell-events.exe","-lshell32"],cwd=ROOT,check=True)
from pathlib import Path
name='WaybridgeTrayEvents-'+uuid.uuid4().hex;q=lambda x:"'"+x.replace("'","''")+"'"
def ps(code):
 e=base64.b64encode(("$ErrorActionPreference='Stop';[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false);"+code).encode('utf-16-le')).decode();r=subprocess.run(['ssh',args.host,'powershell -NoProfile -NonInteractive -EncodedCommand '+e],capture_output=True,text=True,encoding='utf8',errors='replace',timeout=30);assert not r.returncode,r.stderr;return r.stdout.strip()
root=ps('$p=Join-Path $env:TEMP '+q(name)+';New-Item -ItemType Directory -Path $p|Out-Null;$p');exe=root+'\\probe.exe';log=root+'\\result.txt'
try:
 subprocess.run(['scp',str(ROOT/'build/tests/windows-tray-shell-events.exe'),args.host+':'+exe.replace('\\','/')],check=True)
 ps('$a=New-ScheduledTaskAction -Execute '+q(exe)+' -Argument '+q('"'+log+'"')+';$p=New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited;Register-ScheduledTask -TaskName '+q(name)+' -Action $a -Principal $p|Out-Null;Start-ScheduledTask -TaskName '+q(name))
 for _ in range(35):
  output=ps('if(Test-Path '+q(log)+'){Get-Content -Raw '+q(log)+'}')
  if 'done ' in output:break
  time.sleep(.4)
 print(output)
 assert 'legacy_left=1 v4_left=1 legacy_right=1 v4_right=1' in output,output
 print('PASS: actual Explorer emits both compatibility mouse-up and version-4 semantic events for each click')
finally:ps('Stop-ScheduledTask -TaskName '+q(name)+' -ErrorAction SilentlyContinue;Unregister-ScheduledTask -TaskName '+q(name)+' -Confirm:$false;Remove-Item -Recurse -Force '+q(root))
