#!/usr/bin/env python3
"""Actual Chrome selection -> real OS OLE -> streamed Chrome input, both directions."""
import argparse,base64,json,os,socket,subprocess,tempfile,time,uuid
from pathlib import Path
from chrome_dnd_fixture import Cdp,PageServer,DragDelayProxy,TEXT,ROOT
from drag_out_smoke import wait_for
from session_fixture import SessionFixture


def freeport():
    with socket.socket() as s:s.bind(('127.0.0.1',0));return s.getsockname()[1]


def source_finished(events,success):
    return (sum(e['type']=='dragstart' for e in events)==1 and
            [e.get('effect') for e in events if e['type']=='dragend']==['copy' if success else 'none'])


def main():
    p=argparse.ArgumentParser();p.add_argument('--host',required=True);p.add_argument('--baseline',action='store_true');p.add_argument('--extended',action='store_true');p.add_argument('--only',choices=['within']);a=p.parse_args()
    key='Waybridge-browser-dnd-'+uuid.uuid4().hex[:10]
    logdir=ROOT/'build/tests/browser-dnd';logdir.mkdir(parents=True,exist_ok=True)
    driver=ROOT/'build/tests/windows-browser-drag-driver.exe'
    subprocess.run(['x86_64-w64-mingw32-g++','-std=c++17','-municode','-static','-O1','tests/windows_browser_drag_driver.cpp','-o',str(driver)],cwd=ROOT,check=True)
    def remote(code):
        code="$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue';$OutputEncoding=[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false);"+code
        r=subprocess.run(['ssh','-o','BatchMode=yes',a.host,'powershell -NoProfile -NonInteractive -EncodedCommand '+base64.b64encode(code.encode('utf-16-le')).decode()],capture_output=True,text=True,errors='replace',timeout=35)
        assert r.returncode==0,r.stdout+r.stderr
        return r.stdout.strip()
    remote_root=remote('[IO.Path]::GetTempPath()').rstrip('\\')+'\\'+key
    remote("New-Item -ItemType Directory -Path '"+remote_root+"'|Out-Null")
    children=[];native=linux=second=proxy=None;user_state=None
    with tempfile.TemporaryDirectory(prefix='wb-browser-dnd-') as directory,SessionFixture(Path(directory)/'fixture') as fixture,PageServer() as page:
        root=Path(directory);log=(logdir/'host.log').open('w');chrome_log=(logdir/'linux-chrome.log').open('w')
        env={**fixture.env,'WAYLAND_DISPLAY':'browser-dnd-0','WAYLAND_DEBUG':'1'}
        port=freeport();http_win=int(remote('$s=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$s.Start();$p=$s.LocalEndpoint.Port;$s.Stop();$p'))
        viewer_win=int(remote('$s=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$s.Start();$p=$s.LocalEndpoint.Port;$s.Stop();$p'))
        token=root/'token';token.write_text('browser-dnd-'+uuid.uuid4().hex);token.chmod(0o600)
        def spawn(cmd,environment=env,output=log):
            child=subprocess.Popen(cmd,cwd=ROOT,env=environment,stdout=output,stderr=output);children.append(child);return child
        def run_driver(arguments):
            task=key+'-driver'
            code=r'''$root='@ROOT@';$args='@ARGS@';$name='@NAME@';$run=@'
$p=Start-Process -FilePath ($PSScriptRoot+'\driver.exe') -ArgumentList '@ARGS@' -Wait -PassThru -RedirectStandardOutput ($PSScriptRoot+'\driver.txt') -RedirectStandardError ($PSScriptRoot+'\driver-err.txt')
$p.ExitCode|Set-Content ($PSScriptRoot+'\driver-exit.txt')
'@;$path=$root+'\driver-run.ps1';$run|Set-Content -Encoding UTF8 $path;Remove-Item ($root+'\driver-exit.txt') -ErrorAction SilentlyContinue;$action=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -ExecutionPolicy Bypass -File "'+$path+'"');$principal=New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited;Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue|Unregister-ScheduledTask -Confirm:$false;Register-ScheduledTask -TaskName $name -Action $action -Principal $principal|Out-Null;Start-ScheduledTask -TaskName $name'''.replace('@ROOT@',remote_root).replace('@ARGS@',arguments).replace('@NAME@',task)
            remote(code)
            until=time.monotonic()+20
            while time.monotonic()<until:
                result=remote("$p='"+remote_root+"';if(Test-Path ($p+'\\driver-exit.txt')){Get-Content ($p+'\\driver-exit.txt');Get-Content ($p+'\\driver.txt');Get-Content ($p+'\\driver-err.txt')}")
                if result:
                    if result.splitlines()[0]=='3':return False
                    assert result.splitlines()[0]=='0',result
                    return result
                time.sleep(.1)
            raise TimeoutError('native browser driver')
        try:
            host=spawn([str(ROOT/'target/debug/waybridge'),'--socket','browser-dnd-0','--listen',f'127.0.0.1:{port}','--token-file',str(token),'--codec','png','--no-audio'])
            wait_for(lambda:(Path(env['XDG_RUNTIME_DIR'])/'browser-dnd-0.session.json').exists())
            profile=root/'linux-profile'
            chrome=spawn([str(ROOT/'target/debug/waybridge'),'--socket','browser-dnd-0','--launch','--','google-chrome-stable',f'--user-data-dir={profile}','--password-store=basic','--no-first-run','--no-default-browser-check','--disable-background-networking','--remote-debugging-port=0',f'http://127.0.0.1:{page.port}/?side=remote-{key}'],output=chrome_log)
            wait_for(lambda:(profile/'DevToolsActivePort').exists());linux=Cdp(int((profile/'DevToolsActivePort').read_text().splitlines()[0]))
            for source,name in [(ROOT/'build/windows/waybridge-client.exe','viewer.exe'),(driver,'driver.exe')]:subprocess.run(['scp','-q',str(source),a.host+':'+remote_root.replace('\\','/')+'/'+name],check=True,timeout=40)
            user_state=run_driver('save').splitlines()[1]
            proxy=DragDelayProxy(port)
            tunnel=spawn(['ssh','-o','BatchMode=yes','-o','ExitOnForwardFailure=yes','-N','-R',f'127.0.0.1:{http_win}:127.0.0.1:{page.port}','-R',f'127.0.0.1:{viewer_win}:127.0.0.1:{proxy.port}',a.host],environment=os.environ)
            time.sleep(.4);assert tunnel.poll() is None
            code=r'''$root='@ROOT@';$run=@'
$env:WAYBRIDGE_DND_TRACE='1'
$viewer=Start-Process -FilePath ($PSScriptRoot+'\viewer.exe') -ArgumentList @('127.0.0.1','@VIEWER@','@TOKEN@') -PassThru -RedirectStandardOutput ($PSScriptRoot+'\viewer.txt') -RedirectStandardError ($PSScriptRoot+'\viewer-err.txt')
$viewer.Id|Set-Content ($PSScriptRoot+'\viewer-pid.txt')
$arguments=@('--user-data-dir='+$PSScriptRoot+'\chrome-profile','--remote-debugging-port=0','--no-first-run','--no-default-browser-check','--disable-background-networking','--disable-session-crashed-bubble','http://127.0.0.1:@HTTP@/?side=native-@NAME@')
$p=Start-Process -FilePath 'C:\Program Files\Google\Chrome\Application\chrome.exe' -ArgumentList $arguments -PassThru -RedirectStandardOutput ($PSScriptRoot+'\chrome.txt') -RedirectStandardError ($PSScriptRoot+'\chrome-err.txt')
$p.Id|Set-Content ($PSScriptRoot+'\chrome-pid.txt')
'@;$path=$root+'\start.ps1';$run|Set-Content -Encoding UTF8 $path;$action=New-ScheduledTaskAction -Execute 'powershell.exe' -Argument ('-NoProfile -ExecutionPolicy Bypass -File "'+$path+'"');$principal=New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited;Register-ScheduledTask -TaskName '@NAME@' -Action $action -Principal $principal|Out-Null;Start-ScheduledTask -TaskName '@NAME@' '''.replace('@ROOT@',remote_root).replace('@VIEWER@',str(viewer_win)).replace('@HTTP@',str(http_win)).replace('@TOKEN@',token.read_text()).replace('@NAME@',key)
            remote(code)
            activefile=remote_root+'\\chrome-profile\\DevToolsActivePort'
            native_port=wait_for(lambda:remote("if(Test-Path '"+activefile+"'){Get-Content '"+activefile+"'|Select-Object -First 1}"),25)
            local_cdp=freeport();forward=spawn(['ssh','-o','BatchMode=yes','-o','ExitOnForwardFailure=yes','-N','-L',f'127.0.0.1:{local_cdp}:127.0.0.1:{native_port}',a.host],environment=os.environ);time.sleep(.3);native=Cdp(local_cdp)
            wait_for(lambda:run_driver('prepare'),25);time.sleep(.7)
            results=[]
            cases=[('in',700,False,0,'target'),('out',700,False,0,'target'),('in',0,False,0,'target'),('out',0,False,0,'target'),('in',50,True,0,'target')]
            if a.extended:cases += [('in',0,False,.12,'target'),('out',0,False,.12,'target'),('in',0,False,0,'reject'),('in',0,False,.5,'target')]
            if a.only:cases=[]
            for direction,hover,cancel,delay,element in cases:
                proxy.delay=delay
                native.evaluate('prepare()');linux.evaluate('prepare()')
                source,target=(native,linux) if direction=='in' else (linux,native)
                source.call('Page.bringToFront');source.evaluate('prepare()')
                l1=source.evaluate('({...layout(),innerHeight})');l2=target.evaluate('({...layout(),innerHeight})');p1=l1['source'];p2=l2[element]
                args=' '.join(map(str,[direction,p1['x'],p1['y'],l1['innerHeight'],p2['x'],p2['y'],l2['innerHeight'],hover,'cancel' if cancel else 'drop']))
                answer=run_driver(args);time.sleep(.5)
                actual=target.evaluate('target.value');events=source.evaluate('events');received=target.evaluate('events');expected='' if cancel or element=='reject' or delay>.4 else TEXT
                passed=(actual==expected and source_finished(events,bool(expected)) and source.evaluate('source.textContent')==TEXT)
                record=dict(direction=direction,hover=hover,cancel=cancel,delay=delay,element=element,passed=passed,source_events=events,target_events=received,driver=answer);results.append(record)
                (logdir/'results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2));print(f'{direction} hover={hover} cancel={cancel} delay={delay} target={element}: {"PASS" if passed else "FAIL"}',flush=True)
                if not passed and not a.baseline:raise AssertionError('real Chrome drag failed')
            if a.extended or a.only:
                proxy.delay=0
                linux.call('Target.createTarget',url=f'http://127.0.0.1:{page.port}/?side=remote2-{key}',newWindow=True)
                second=Cdp(int((profile/'DevToolsActivePort').read_text().splitlines()[0]),title='DND remote2-')
                wait_for(lambda:run_driver('prepare-within'),20);time.sleep(.5)
                linux.evaluate('prepare()');second.evaluate('prepare()')
                left=linux.evaluate('({...layout(),innerHeight})');right=second.evaluate('({...layout(),innerHeight})')
                args=' '.join(map(str,['within',left['source']['x'],left['source']['y'],left['innerHeight'],right['target']['x'],right['target']['y'],right['innerHeight'],0,'drop']))
                result=run_driver(args);time.sleep(.5)
                state=dict(source=linux.evaluate('({events,selection:getSelection().toString()})'),target=second.evaluate('({events,value:target.value})'),driver=result,source_layout=left,target_layout=right)
                (logdir/'within.json').write_text(json.dumps(state,ensure_ascii=False,indent=2))
                assert state['target']['value']==TEXT,'streamed Chrome -> streamed Chrome failed'
                assert source_finished(state['source']['events'],True) and linux.evaluate('source.textContent')==TEXT
                print('within two streamed Chrome windows: PASS',flush=True)
                run_driver('prepare-handoff');time.sleep(.5)
                native.evaluate('prepare()');linux.evaluate('prepare()');second.evaluate('prepare()')
                native.call('Page.bringToFront')
                left=native.evaluate('({...layout(),innerHeight})');right=linux.evaluate('({...layout(),innerHeight})')
                args=' '.join(map(str,['handoff',left['source']['x'],left['source']['y'],left['innerHeight'],right['target']['x'],right['target']['y'],right['innerHeight'],0,'drop']))
                run_driver(args);time.sleep(.5)
                handoff=dict(source=native.evaluate('events'),first=linux.evaluate('({events,value:target.value})'),second=second.evaluate('({events,value:target.value})'))
                (logdir/'handoff.json').write_text(json.dumps(handoff,ensure_ascii=False,indent=2))
                assert handoff['second']['value']==TEXT and not handoff['first']['value'],'native drag did not switch targets safely'
                assert source_finished(handoff['source'],True) and native.evaluate('source.textContent')==TEXT
                trace=remote("Get-Content '"+remote_root+"\\\\viewer-err.txt'")
                enters=[json.loads(row.split('DND ',1)[1]) for row in trace.splitlines() if 'DND ' in row and '"event":"text.enter"' in row]
                assert len(enters)>=2 and enters[-1]['drag']==enters[-2]['drag'] and enters[-1]['source_reused'],'handoff replaced the source gesture/provider'
                print('native source across two streamed targets: PASS',flush=True)
            assert a.baseline or all(r['passed'] for r in results)
        finally:
            try:
                diagnostics=remote("$p='"+remote_root+"';Get-Content ($p+'\\viewer-err.txt') -ErrorAction SilentlyContinue")
                (logdir/'windows-viewer.log').write_text(diagnostics)
                if user_state:run_driver('restore-user '+user_state)
                for c in (second,native,linux):
                    if c:c.close()
                remote("$p='"+remote_root+"';Get-ScheduledTask|Where-Object{$_.TaskName -eq '"+key+"' -or $_.TaskName -eq '"+key+"-driver'}|Unregister-ScheduledTask -Confirm:$false;foreach($name in @('chrome-pid.txt','viewer-pid.txt')){if(Test-Path ($p+'\\'+$name)){$n=[int](Get-Content ($p+'\\'+$name));taskkill /PID $n /T /F 2>$null|Out-Null}};Remove-Item -Recurse -Force -LiteralPath $p -ErrorAction SilentlyContinue")
            finally:
                if proxy:proxy.close()
                for child in reversed(children):
                    if child.poll() is None:child.terminate()
                for child in children:
                    try:child.wait(timeout=12)
                    except subprocess.TimeoutExpired:child.kill();child.wait()
                log.close();chrome_log.close()

if __name__=='__main__':main()
