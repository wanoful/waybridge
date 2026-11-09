#!/usr/bin/env python3
"""Actual PE/WASAPI playback into an explicitly selected private null endpoint.
Never changes or plays to the host default audio endpoint.
"""
import json,os,signal,socket,struct,subprocess,tempfile,time
from pathlib import Path
from audio_smoke import ROOT,command,tone

def wait(check,seconds=15):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        result=check()
        if result:return result
        time.sleep(.05)
    raise AssertionError('Windows audio test timed out; see windows-audio.log')

def main():
    subprocess.run(['x86_64-w64-mingw32-g++','-municode','-static','-std=c++17','-D_WIN32_WINNT=0x0A00','tests/windows_audio_probe.cpp','-o','build/tests/windows-audio-probe.exe','-lole32','-luuid'],cwd=ROOT,check=True)
    before=command('pactl','get-default-sink')
    server=next(line.split(': ',1)[1] for line in command('pactl','info').splitlines() if line.startswith('Server String:'))
    pulse={**os.environ,'PULSE_SERVER':server}
    output_name='waybridge-audio-out-'+str(os.getpid())
    description='Waybridge-Audio-Out-'+str(os.getpid())
    module=int(command('pactl','load-module','module-null-sink','sink_name='+output_name,'rate=48000','channels=2','sink_properties=device.description='+description))
    processes=[];capture=None;player=None;wine_env=None;prefix=ROOT/'build/wine-audio';prefix.mkdir(parents=True,exist_ok=True)
    raw=ROOT/'build/tests/windows-audio.raw'
    try:
        with tempfile.TemporaryDirectory(prefix='waybridge-win-audio-') as runtime,open(ROOT/'build/tests/windows-audio.log','w') as log:
            fd_read,fd_write=os.pipe()
            xvfb=subprocess.Popen(['Xvfb','-displayfd',str(fd_write),'-screen','0','1280x1024x24','-nolisten','tcp'],pass_fds=[fd_write],stdout=log,stderr=log)
            processes.append(xvfb);os.close(fd_write)
            with os.fdopen(fd_read) as pipe:display=':'+pipe.readline().strip()
            wine_env={**pulse,'DISPLAY':display,'WINEPREFIX':str(prefix),'WINEDEBUG':'-all','WINEDLLOVERRIDES':'winemenubuilder.exe=d;mscoree,mshtml=','WAYBRIDGE_GDI':'1'}
            subprocess.run(['wineboot','-u'],env=wine_env,stdout=log,stderr=log,check=True,timeout=60)
            resolved=subprocess.run(['wine',str(ROOT/'build/tests/windows-audio-probe.exe'),description],env=wine_env,capture_output=True,text=True,timeout=15,check=True).stdout.strip()
            assert resolved.startswith('{'),resolved
            wine_env['WAYBRIDGE_AUDIO_DEVICE']=resolved
            sink_file=Path(runtime)/'sink'
            token_file=Path(runtime)/'token';token='win-audio-'+40*'b';token_file.write_text(token);token_file.chmod(0o600)
            with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
            env={**pulse,'XDG_RUNTIME_DIR':runtime}
            host=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--audio','--socket','win-audio-0','--listen',f'127.0.0.1:{port}','--token-file',str(token_file),'--','python','-c','import os,sys;from pathlib import Path;Path(sys.argv[1]).write_text(os.environ["PULSE_SINK"])',str(sink_file)],env=env,stdout=log,stderr=log)
            processes.append(host)
            wait(lambda:sink_file.exists())
            sink=sink_file.read_text()
            client_log=ROOT/'build/tests/windows-audio-client.log'
            with client_log.open('w') as output:
                client=subprocess.Popen(['wine',str(ROOT/'build/windows/waybridge-client.exe'),'127.0.0.1',str(port),token],env=wine_env,stdout=output,stderr=output)
                processes.append(client)
                wait(lambda:'WASAPI 48 kHz stereo audio playback active' in client_log.read_text(),20)
                with raw.open('wb') as recording:
                    capture=subprocess.Popen(['parec','--raw','--format=s16le','--rate=48000','--channels=2','--latency-msec=10','--device='+output_name+'.monitor'],env=pulse,stdout=recording,stderr=log)
                    processes.append(capture)
                    time.sleep(.15)
                    player=tone(sink,pulse)
                    player[1].join(timeout=12)
                    assert not player[1].is_alive() and player[2] and player[2][0][0]==0,player[2]
                    time.sleep(.6)
                    capture.terminate();capture.wait(timeout=5)
                data=raw.read_bytes();data=data[:len(data)//4*4]
                samples=struct.unpack('<'+'h'*(len(data)//2),data)
                left=max(map(abs,samples[0::2]),default=0)
                right=max(map(abs,samples[1::2]),default=0)
                assert left>5000 and right>2000 and .3<right/left<.7,(left,right)
                print(f'PASS: real Windows PE/WASAPI audio through private PCM socket, private endpoint loopback peaks L={left} R={right}; correct stereo, no default sink changes')
    finally:
        if player and player[0].poll() is None:player[0].kill();player[1].join(timeout=3)
        for process in reversed(processes):
            if process.poll() is None:
                process.terminate()
                try:process.wait(timeout=10)
                except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
        if wine_env:subprocess.run(['wineserver','-k'],env=wine_env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=10)
        subprocess.run(['pactl','unload-module',str(module)],check=True)
        assert command('pactl','get-default-sink')==before

if __name__=='__main__':main()
