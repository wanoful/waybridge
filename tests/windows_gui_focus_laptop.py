#!/usr/bin/env python3
"""Opt-in actual WinUI click -> management SSH -> GIO -> native foreground test.

Runs only an independently named GUI binary, temporary profile/key/known_hosts,
private sshd and SessionFixture bus/HOME. Never attaches to an installed session.
"""
import argparse
import base64
import json
import os
from pathlib import Path
import pwd
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import uuid
import zipfile
from session_fixture import SessionFixture

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--host', required=True)
    parser.add_argument('--publish-directory', type=Path, default=ROOT/'build/gui-focus-probe-x64')
    args = parser.parse_args()
    publish = args.publish_directory.resolve(strict=True)
    assert (publish/'Waybridge.GuiFocus.Probe.exe').is_file()
    key = 'Waybridge-gui-focus-probe-' + uuid.uuid4().hex[:12]
    logfile = ROOT/'build/tests/gui-focus-laptop.log'
    def remote(code, check=True):
        encoded = base64.b64encode(code.encode('utf-16-le')).decode()
        result = subprocess.run(['ssh','-o','BatchMode=yes','-o','ConnectTimeout=10',args.host,
            'powershell -NoProfile -NonInteractive -EncodedCommand ' + encoded],
            capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=35)
        if check: assert result.returncode == 0, result.stdout + result.stderr
        return result.stdout.strip()
    destination = remote('[IO.Path]::GetTempPath()').rstrip('\\') + '\\' + key
    archive = ROOT/'build'/f'{key}.zip'
    subprocess.run(['c++','-std=c++17','-fPIC','tests/qt_focus_app.cpp','-o','build/tests/qt-focus-app',
                    *subprocess.check_output(['pkg-config','--cflags','--libs','Qt6Widgets'],text=True).split()],cwd=ROOT,check=True)
    with zipfile.ZipFile(archive,'w',zipfile.ZIP_DEFLATED,compresslevel=1) as zipped:
        for path in publish.rglob('*'):
            if path.is_file(): zipped.write(path,path.relative_to(publish))
        zipped.write(ROOT/'build/windows/waybridge-client.exe','waybridge-client.exe')
    with tempfile.TemporaryDirectory(prefix='wb-gui-focus-') as directory, SessionFixture(Path(directory)/'fixture') as fixture:
        root = Path(directory)
        for name in ('host','identity'):
            subprocess.run(['ssh-keygen','-q','-t','ed25519','-N','','-f',str(root/name)],check=True)
        with socket.socket() as free:
            free.bind(('127.0.0.1',0)); linux_port = free.getsockname()[1]
        windows_port = int(remote('$l=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$l.Start();$p=$l.LocalEndpoint.Port;$l.Stop();$p'))
        env = {k:v for k,v in fixture.env.items() if k in ('DBUS_SYSTEM_BUS_ADDRESS','DBUS_SESSION_BUS_ADDRESS','HOME',
            'XDG_RUNTIME_DIR','XDG_CONFIG_HOME','XDG_DATA_HOME','XDG_CACHE_HOME','XDG_STATE_HOME','WAYBRIDGE_TEST_SESSION_FIXTURE')}
        env.update(QT_QPA_PLATFORM='wayland',QT_WAYLAND_DISABLE_WINDOWDECORATION='1',XDG_DATA_DIRS=str(root/'empty'))
        def quote(value): return '"'+value.replace('\\','\\\\').replace('"','\\"')+'"'
        (root/'sshd_config').write_text(f'''Port {linux_port}
ListenAddress 127.0.0.1
HostKey {root/'host'}
PidFile {root/'sshd.pid'}
AuthorizedKeysFile {root/'identity.pub'}
StrictModes no
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
SetEnv {' '.join(k+'='+quote(v) for k,v in env.items())}
LogLevel ERROR
''')
        binary = root/'server'
        binary.write_text('#!/bin/sh\nexec '+str(ROOT/'target/debug/waybridge')+' --no-audio --no-xwayland "$@"\n')
        binary.chmod(0o700)
        apps = Path(fixture.env['XDG_DATA_HOME'])/'applications'; apps.mkdir(parents=True)
        (apps/'org.waybridge.FocusProbe.desktop').write_text('[Desktop Entry]\nType=Application\nName=Waybridge isolated focus fixture\nExec='+str(ROOT/'build/tests/qt-focus-app')+'\nStartupNotify=false\n')
        hostkey = (root/'host.pub').read_text().split()
        profile_id = str(uuid.uuid4())
        profile = dict(id=profile_id,name='Disposable GUI focus fixture',mode='ssh',host='127.0.0.1',ssh_port=windows_port,
            user=pwd.getpwuid(os.getuid()).pw_name,identity_file=destination+'\\identity',server_binary=str(binary),
            session='gui-focus-'+uuid.uuid4().hex[:8],session_mode='auto',port=0,auto_reconnect=False,
            settings=dict(codec='png',fps=30,audio=False,notifications=False,tray=False,clipboard='off',files=False))
        children = []
        with logfile.open('w+') as log:
            try:
                sshd = subprocess.Popen([shutil.which('sshd'),'-D','-e','-f',str(root/'sshd_config')],stdout=log,stderr=log);children.append(sshd)
                for _ in range(80):
                    assert sshd.poll() is None, 'temporary sshd failed'
                    try:
                        with socket.create_connection(('127.0.0.1',linux_port),timeout=.1): break
                    except OSError: time.sleep(.05)
                tunnel = subprocess.Popen(['ssh','-o','BatchMode=yes','-o','ExitOnForwardFailure=yes','-N',
                    '-R',f'127.0.0.1:{windows_port}:127.0.0.1:{linux_port}',args.host],stdout=log,stderr=log);children.append(tunnel)
                subprocess.run(['scp','-q',str(archive),args.host+':'+(destination+'.zip').replace('\\','/')],check=True,timeout=150)
                remote("Expand-Archive -LiteralPath '"+destination+".zip' -DestinationPath '"+destination+"'; New-Item -ItemType Directory -Path '"+destination+"\\state' | Out-Null")
                own = {'profiles.json':json.dumps([profile]),'known_hosts':f'[127.0.0.1]:{windows_port} {hostkey[0]} {hostkey[1]}\n',
                       'profile-id.txt':profile_id}
                for name,text in own.items():
                    path = root/name;path.write_text(text)
                    sub = '/state/' if name in ('profiles.json','known_hosts') else '/'
                    subprocess.run(['scp','-q',str(path),args.host+':'+destination.replace('\\','/')+sub+name],check=True,timeout=30)
                subprocess.run(['scp','-q',str(root/'identity'),args.host+':'+destination.replace('\\','/')+'/identity'],check=True,timeout=30)
                setup = r'''
$ErrorActionPreference='Stop'; $root='@ROOT@';$name='@NAME@'
$identity=$root+'\identity'; & icacls.exe $identity /inheritance:r /grant:r ($env:USERNAME+':R') | Out-Null
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
'''.replace('@ROOT@',destination).replace('@NAME@',key)
                remote(setup)
                deadline = time.monotonic()+130
                result = ''
                while time.monotonic() < deadline:
                    result = remote("$p='"+destination+"';if(Test-Path ($p+'\\exit.txt')){Get-Content ($p+'\\exit.txt');Get-Content ($p+'\\result.txt') -ErrorAction SilentlyContinue;Get-Content ($p+'\\stderr.txt') -ErrorAction SilentlyContinue;Get-Content ($p+'\\startup.log') -ErrorAction SilentlyContinue}")
                    if result: break
                    time.sleep(1)
                if not result:
                    result = remote("$p='"+destination+"';Get-Content ($p+'\\result.txt') -ErrorAction SilentlyContinue;Get-Content ($p+'\\stderr.txt') -ErrorAction SilentlyContinue;Get-Content ($p+'\\startup.log') -ErrorAction SilentlyContinue")
                    raise AssertionError('GUI probe timeout: '+result)
                log.write(result+'\n');log.flush()
                for path in Path(fixture.env['XDG_RUNTIME_DIR']).rglob('*.log'):
                    log.write('\nLINUX '+path.name+'\n'+path.read_text(errors='replace'));log.flush()
                print(result)
                assert result.splitlines()[0].strip()=='0', 'GUI focus test failed; see '+str(logfile)
                print('PASS actual WinUI UI/SSH/GIO/native worker foreground, second process and Qt modal restoration')
            finally:
                remote("$root='"+destination+"';Get-ScheduledTask -TaskName '"+key+"' -ErrorAction SilentlyContinue|Unregister-ScheduledTask -Confirm:$false;Get-CimInstance Win32_Process|Where-Object{$_.ExecutablePath -and $_.ExecutablePath.StartsWith($root+'\\',[StringComparison]::OrdinalIgnoreCase)}|ForEach-Object{Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue};Remove-Item -Recurse -Force -LiteralPath $root -ErrorAction SilentlyContinue;Remove-Item -Force -LiteralPath ($root+'.zip') -ErrorAction SilentlyContinue",check=False)
                for descriptor in Path(fixture.env['XDG_RUNTIME_DIR']).glob('gui-focus-*.session.json'):
                    saved=json.loads(descriptor.read_text())
                    try:
                        current=Path(f"/proc/{saved['pid']}/stat").read_text().rsplit(')',1)[1].split()[19]
                        if current==str(saved['started']):os.kill(saved['pid'],signal.SIGTERM)
                    except FileNotFoundError:pass
                for child in reversed(children):
                    if child.poll() is None:child.terminate()
                for child in children:
                    try:child.wait(timeout=5)
                    except subprocess.TimeoutExpired:child.kill();child.wait()
                archive.unlink(missing_ok=True)

if __name__=='__main__':main()
