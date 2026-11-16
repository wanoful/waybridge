#!/usr/bin/env python3
"""Execute only this self-contained, self-cleaning focus probe on Windows."""
import argparse,base64,subprocess,time,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser();p.add_argument('--host',required=True);p.add_argument('--visual',action='store_true');a=p.parse_args()
 key='Waybridge-focus-probe-'+uuid.uuid4().hex[:12]
 def remote(code):
  r=subprocess.run(['ssh','-o','BatchMode=yes',a.host,'powershell -NoProfile -NonInteractive -EncodedCommand '+base64.b64encode(code.encode('utf-16-le')).decode()],capture_output=True,text=True,errors='replace',timeout=30)
  assert r.returncode==0,r.stdout+r.stderr
  return r.stdout.strip()
 root=remote('[IO.Path]::GetTempPath()').rstrip('\\')+'\\'+key
 remote("New-Item -ItemType Directory -Path '"+root+"' | Out-Null")
 try:
  binary='windows-drag-visual-probe.exe' if a.visual else 'windows-activation-probe.exe'
  subprocess.run(['scp','-q',str(ROOT/'build/tests'/binary),a.host+':'+root.replace('\\','/')+'/probe.exe'],check=True,timeout=30)
  code=r'''
$root='@ROOT@';$name='@NAME@'
$run=@'
$p=Start-Process -FilePath (Join-Path $PSScriptRoot 'probe.exe') @ARGS@ -NoNewWindow -Wait -PassThru -RedirectStandardOutput (Join-Path $PSScriptRoot 'out.txt') -RedirectStandardError (Join-Path $PSScriptRoot 'err.txt')
$p.ExitCode | Set-Content -Encoding ASCII (Join-Path $PSScriptRoot 'exit.txt')
'@
$path=Join-Path $root 'run.ps1';$run|Set-Content -Encoding UTF8 $path
$act=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -ExecutionPolicy Bypass -File "'+$path+'"') -WorkingDirectory $root
$principal=New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName $name -Action $act -Principal $principal | Out-Null
Start-ScheduledTask -TaskName $name
'''.replace('@ROOT@',root).replace('@NAME@',key).replace('@ARGS@',"-ArgumentList '--pixels'" if a.visual else '')
  remote(code)
  end=time.monotonic()+40
  while time.monotonic()<end:
   result=remote("$p='"+root+"';if(Test-Path ($p+'\\exit.txt')){Get-Content ($p+'\\exit.txt');Get-Content ($p+'\\out.txt');Get-Content ($p+'\\err.txt')}")
   if result:
    print(result);assert result.splitlines()[0]=='0';return
   time.sleep(1)
  raise AssertionError('focus probe timed out')
 finally:
  remote("$root='"+root+"';$name='"+key+"';Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue|Unregister-ScheduledTask -Confirm:$false;Get-CimInstance Win32_Process|Where-Object{$_.ExecutablePath -eq ($root+'\\probe.exe')}|ForEach-Object{Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue};Remove-Item -Recurse -Force -LiteralPath $root -ErrorAction SilentlyContinue")
if __name__=='__main__':main()
