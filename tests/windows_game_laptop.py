#!/usr/bin/env python3
"""Opt-in physical Windows checks of X11 CSD movement and GLFW fullscreen."""
import argparse
import base64
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import uuid
from session_fixture import SessionFixture
from smoke import ROOT
from drag_out_smoke import wait_for


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--host',required=True)
    parser.add_argument('--hmcl',action='store_true',help='also drag the installed launcher with a disposable HOME')
    parser.add_argument('--ewmh',action='store_true',help='exercise _NET_WM_MOVERESIZE instead of direct X11 CSD moves')
    parser.add_argument('--dpi',type=int,help='override presentation density in the disposable viewer only')
    parser.add_argument('--drag-dx',type=int,default=120)
    parser.add_argument('--drag-dy',type=int,default=80)
    args=parser.parse_args()
    key='Waybridge-game-'+uuid.uuid4().hex[:10]
    def remote(code,check=True):
        code="$ProgressPreference='SilentlyContinue';"+code
        encoded=base64.b64encode(code.encode('utf-16-le')).decode()
        r=subprocess.run(['ssh','-o','BatchMode=yes',args.host,
            'powershell -NoProfile -NonInteractive -EncodedCommand '+encoded],
            capture_output=True,text=True,errors='replace',timeout=30)
        if check:assert r.returncode==0,r.stdout+r.stderr
        return r.stdout.strip()
    subprocess.run(['x86_64-w64-mingw32-g++','-std=c++17','-D_WIN32_WINNT=0x0A00',
        '-municode','-static','-O1','-Iclient','tests/windows_game_probe.cpp',
        '-o','build/tests/windows-game-probe.exe','-lgdi32'],cwd=ROOT,check=True)
    subprocess.run(['cc','tests/x11_pointer_app.c','-o','build/tests/x11-pointer-app',
        *subprocess.check_output(['pkg-config','--cflags','--libs','glfw3','gl','x11'],text=True).split()],cwd=ROOT,check=True)
    dest=remote('[IO.Path]::GetTempPath()').rstrip('\\')+'\\'+key
    remote("New-Item -ItemType Directory -Path '"+dest+"' | Out-Null")
    with tempfile.TemporaryDirectory(prefix='wb-game-') as directory, tempfile.TemporaryDirectory(prefix='game-home-',dir=ROOT/'build/tests') as home, SessionFixture(Path(directory)/'fixture') as fixture:
        runtime=Path(fixture.env['XDG_RUNTIME_DIR'])
        token=runtime/'token';token.write_text('game-'+uuid.uuid4().hex);token.chmod(0o600)
        env={**fixture.env,'HOME':home,'WAYBRIDGE_TEST_CSD':'1','RUST_LOG':'waybridge=debug'}
        if args.ewmh:env['WAYBRIDGE_TEST_EWMH']='1'
        with socket.socket() as s:s.bind(('127.0.0.1',0));port=s.getsockname()[1]
        children=[]
        with (ROOT/'build/tests/game-laptop.log').open('w+') as log:
            try:
                server=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--socket','game-0','--listen',
                    f'127.0.0.1:{port}','--token-file',str(token),'--codec','png','--no-audio'],env=env,stdout=log,stderr=log);children.append(server)
                wait_for(lambda:(runtime/'game-0.session.json').exists())
                app=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--socket','game-0','--launch','--',
                    str(ROOT/'build/tests/x11-pointer-app')],env=env,stdin=subprocess.PIPE,stdout=log,stderr=log);children.append(app)
                for source,name in [(ROOT/'build/windows/waybridge-client.exe','viewer.exe'),(ROOT/'build/tests/windows-game-probe.exe','driver.exe')]:
                    subprocess.run(['scp','-q',str(source),args.host+':'+dest.replace('\\','/')+'/'+name],check=True,timeout=30)
                remote_port=int(remote('$l=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$l.Start();$p=$l.LocalEndpoint.Port;$l.Stop();$p'))
                tunnel=subprocess.Popen(['ssh','-o','BatchMode=yes','-o','ExitOnForwardFailure=yes','-N','-R',
                    f'127.0.0.1:{remote_port}:127.0.0.1:{port}',args.host],stdout=log,stderr=log);children.append(tunnel)
                time.sleep(.5);assert tunnel.poll() is None
                code=r'''
$root='@ROOT@';$name='@NAME@'
$run=@'
$ErrorActionPreference='Stop';$root=$PSScriptRoot;$worker=$null
@DPI@
try {
 $worker=Start-Process ($root+'\viewer.exe') -NoNewWindow -ArgumentList @('127.0.0.1','@PORT@','@TOKEN@') -PassThru -RedirectStandardOutput ($root+'\worker.txt') -RedirectStandardError ($root+'\worker-err.txt')
 foreach($mode in @MODES@) {
  $arguments=if($mode -eq 'drag'){@('drag','"Waybridge GLFW launcher"','250','20','@DX@','@DY@')}elseif($mode -eq 'hmcl'){@('drag','"Hello Minecraft! Launcher"','250','20','@DX@','@DY@')}else{@('fullscreen','"Waybridge GLFW game"')}
  for($i=0;$i -lt 80;$i++) {
   $p=Start-Process ($root+'\driver.exe') -NoNewWindow -ArgumentList $arguments -PassThru -Wait -RedirectStandardOutput ($root+'\'+$mode+'.txt') -RedirectStandardError ($root+'\'+$mode+'-err.txt')
   if($p.ExitCode -ne 3 -and $p.ExitCode -ne 4){break};Start-Sleep -Milliseconds 250
  }
  if($p.ExitCode -ne 0){throw ($mode+' failed: '+$p.ExitCode)}
  'ok'|Set-Content ($root+'\'+$mode+'-done.txt')
 }
 '0'|Set-Content ($root+'\exit.txt')
} catch { $_|Out-String|Set-Content ($root+'\failure.txt');'1'|Set-Content ($root+'\exit.txt') }
finally { if($worker){Stop-Process -Id $worker.Id -Force -ErrorAction SilentlyContinue} }
'@
$path=$root+'\run.ps1';$run|Set-Content -Encoding UTF8 $path
$act=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "'+$path+'"') -WorkingDirectory $root
$principal=New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName $name -Action $act -Principal $principal | Out-Null
Start-ScheduledTask -TaskName $name
'''.replace('@ROOT@',dest).replace('@NAME@',key).replace('@PORT@',str(remote_port)).replace('@TOKEN@',token.read_text()).replace('@MODES@',"@('drag','hmcl','fullscreen')" if args.hmcl else "@('drag','fullscreen')").replace('@DPI@',"$env:WAYBRIDGE_DPI='"+str(args.dpi)+"'" if args.dpi else '').replace('@DX@',str(args.drag_dx)).replace('@DY@',str(args.drag_dy))
                remote(code)
                launched=False;sent=False;result=''
                deadline=time.monotonic()+100
                while time.monotonic()<deadline:
                    status=remote("$r='"+dest+"';[string](Test-Path ($r+'\\drag-done.txt'));[string](Test-Path ($r+'\\hmcl-done.txt'));if(Test-Path ($r+'\\exit.txt')){Get-Content ($r+'\\exit.txt')}")
                    lines=status.splitlines()
                    if lines and lines[0]=='True' and args.hmcl and not launched:
                        children.append(subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--socket','game-0','--launch','--','hmcl-bin'],env=env,stdout=log,stderr=log));launched=True
                    if lines and (lines[1]=='True' if args.hmcl else lines[0]=='True') and not sent:
                        app.stdin.write(b'gf');app.stdin.flush();sent=True
                    if len(lines)>2:
                        result=remote("Get-ChildItem '"+dest+"\\*.txt'|ForEach-Object{$_.Name;Get-Content $_.FullName}")
                        log.write('\nWINDOWS RESULTS\n'+result);log.flush()
                        assert lines[2]=='0',result
                        break
                    time.sleep(.3)
                if not result:
                    result=remote("Get-ChildItem '"+dest+"\\*.txt'|ForEach-Object{$_.Name;Get-Content $_.FullName}")
                    log.write('\nTIMEOUT WINDOWS RESULTS\n'+result);log.flush()
                    raise AssertionError('physical game test timed out: '+result)
                print(result)
                print('PASS: physical X11 CSD ConfigureRequest drag, fullscreen bounds/AltSnap-style resize, minimize/taskbar restore, rightmost pixels')
            finally:
                remote("$root='"+dest+"';Get-ScheduledTask -TaskName '"+key+"' -ErrorAction SilentlyContinue|Unregister-ScheduledTask -Confirm:$false;Get-CimInstance Win32_Process|Where-Object{$_.ExecutablePath -and $_.ExecutablePath.StartsWith($root+'\\',[StringComparison]::OrdinalIgnoreCase)}|ForEach-Object{Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue};Remove-Item -Recurse -Force -LiteralPath $root -ErrorAction SilentlyContinue",check=False)
                for child in reversed(children):
                    if child.poll() is None:child.terminate()
                for child in children:
                    try:child.wait(timeout=8)
                    except subprocess.TimeoutExpired:child.kill();child.wait()


if __name__=='__main__':main()
