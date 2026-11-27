#!/usr/bin/env python3
"""Private sink PCM or Opus, authenticated audio transport, credits and cleanup.
All generated sound goes to null sinks; the host's default output is never changed.
"""
import argparse,ctypes,json,math,os,select,socket,struct,subprocess,tempfile,threading,time
from pathlib import Path
from smoke import ROOT

def command(*args):return subprocess.check_output(args,text=True).strip()
def exact(sock,n):
    data=bytearray()
    while len(data)<n:
        more=sock.recv(n-len(data))
        if not more:raise EOFError()
        data.extend(more)
    return bytes(data)
def packet(sock):
    size,=struct.unpack('>I',exact(sock,4));assert 0<size<=19229
    body=exact(sock,size)
    return body[0],json.loads(body[1:]) if body[0]==0 else body[1:]
def send(sock,**value):
    body=b'\0'+json.dumps(value).encode();sock.sendall(struct.pack('>I',len(body))+body)
def control_message(sock,message_type):
    deadline=time.monotonic()+2
    while time.monotonic()<deadline:
        kind,value=packet(sock)
        assert kind==0,'control socket must carry JSON'
        if value.get('type')==message_type:return value
    raise AssertionError('missing control message: '+message_type)
def tone(sink,env):
    pcm=b''.join(struct.pack('<hh',int(12000*math.sin(2*math.pi*1000*i/48000)),int(6000*math.sin(2*math.pi*500*i/48000))) for i in range(24000))
    process=subprocess.Popen(['pacat','--playback','--raw','--format=s16le','--rate=48000','--channels=2','--device='+sink],env=env,stdin=subprocess.PIPE,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
    result=[]
    def play():
        _,error=process.communicate(pcm,timeout=10)
        result.append((process.returncode,error))
    worker=threading.Thread(target=play);worker.start();return process,worker,result

class OpusDecoder:
    def __init__(self):
        self.library = ctypes.CDLL("libopus.so.0")
        self.library.opus_decoder_create.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
        self.library.opus_decoder_create.restype = ctypes.c_void_p
        self.library.opus_decode.argtypes = [
            ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_int16),
            ctypes.c_int, ctypes.c_int,
        ]
        self.library.opus_decoder_destroy.argtypes = [ctypes.c_void_p]
        self.library.opus_decoder_destroy.restype = None
        error = ctypes.c_int()
        self.decoder = self.library.opus_decoder_create(48000, 2, ctypes.byref(error))
        assert self.decoder, error.value

    def decode(self, packet):
        samples = (ctypes.c_int16 * 960)()
        assert self.library.opus_decode(self.decoder, packet, len(packet), samples, 480, 0) == 480
        return samples

    def close(self):
        self.library.opus_decoder_destroy(self.decoder)

def main(use_opus=False):
    before=command('pactl','get-default-sink')
    server=next(line.split(': ',1)[1] for line in command('pactl','info').splitlines() if line.startswith('Server String:'))
    pulse={**os.environ,'PULSE_SERVER':server}
    log_path = ROOT / 'build/tests/audio-host.log'
    with tempfile.TemporaryDirectory(prefix='waybridge-audio-') as runtime,log_path.open('w') as log:
        token='audio-test-'+40*'a';path=Path(runtime)/'token';path.write_text(token);path.chmod(0o600)
        env={**pulse,'XDG_RUNTIME_DIR':runtime}
        sink_path=Path(runtime)/'sink'
        with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
        host=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--audio','--stats','--socket','audio-0','--listen',f'127.0.0.1:{port}','--token-file',str(path),'--','python','-c','import os,sys;from pathlib import Path;Path(sys.argv[1]).write_text(os.environ["PULSE_SINK"])',str(sink_path)],env=env,stdout=log,stderr=log)
        control=audio=None;decoy=None;players=[];sink=None;decoder=None
        try:
            end=time.monotonic()+10
            while not sink_path.exists():
                assert host.poll() is None and time.monotonic()<end;time.sleep(.02)
            sink=sink_path.read_text();assert sink.startswith('waybridge-')
            control=socket.create_connection(('127.0.0.1',port),timeout=5)
            capabilities = {"audio_pcm": True}
            if use_opus:
                capabilities["audio_opus_v1"] = True
            send(control,type='hello',version=3,token=token,**capabilities)
            kind,hello=packet(control)
            assert kind==0 and hello['audio_pcm'] is (not use_opus)
            assert hello['audio_opus_v1'] is use_opus
            for supplied_token,key in [(token,'wrong'),('wrong',hello['media_key'])]:
                bad=socket.create_connection(('127.0.0.1',port),timeout=3)
                send(bad,type='hello',version=3,token=supplied_token,channel='audio',media_key=key)
                assert bad.recv(1)==b'';bad.close()
            audio=socket.create_connection(('127.0.0.1',port),timeout=5)
            send(audio,type='hello',version=3,token=token,channel='audio',media_key=hello['media_key'])
            ready = {"type": "audio_ready"}
            if use_opus:
                ready["codec"] = "opus"
                decoder = OpusDecoder()
            assert packet(audio) == (0, ready)
            duplicate=socket.create_connection(('127.0.0.1',port),timeout=3)
            send(duplicate,type='hello',version=3,token=token,channel='audio',media_key=hello['media_key'])
            assert duplicate.recv(1)==b'';duplicate.close()
            last=0;last_time=0
            def frame(ack=True):
                nonlocal last,last_time
                kind,body=packet(audio);assert kind==3
                rate,channels,bits,n,seq,stamp=struct.unpack('>IHHIQQ',body[:28])
                assert (rate,channels,bits,n)==(48000,2,16,480)
                payload = body[28:]
                assert len(payload) == (160 if use_opus else 1920)
                assert seq>last and stamp>=last_time
                last=seq;last_time=stamp
                samples = decoder.decode(payload) if decoder else struct.unpack('<960h',payload)
                if ack:send(audio,type='audio_ack',sequence=seq)
                return samples
            for _ in range(4):frame(False)
            assert not select.select([audio],[],[],.15)[0],'four unacknowledged packets must stop kernel buffering too'
            send(audio,type='audio_ack',sequence=last+9999)
            assert not select.select([audio],[],[],.05)[0],'future ACK must not grant credits'
            started=time.monotonic();send(control,type='ping',nonce=123)
            assert control_message(control,'pong')==dict(type='pong',nonce=123)
            assert time.monotonic()-started<.5
            send(audio,type='audio_ack',sequence=last);frame()
            decoy_name='waybridge-audio-decoy-'+str(host.pid)
            decoy=int(command('pactl','load-module','module-null-sink','sink_name='+decoy_name,'rate=48000','channels=2'))
            players.append(tone(decoy_name,pulse))
            for _ in range(70):assert max(map(abs,frame()))==0,'unrelated sink leaked into private capture'
            players.append(tone(sink,pulse))
            peak=0
            for _ in range(100):peak=max(peak,max(map(abs,frame())))
            assert peak>2000,peak
            for process,worker,result in players:
                worker.join(timeout=12);assert not worker.is_alive() and result and result[0][0]==0,result
            deadline = time.monotonic() + 3
            while True:
                diagnostics = log_path.read_text()
                if 'audio_credit_wait' in diagnostics and 'audio_unsent_drops' in diagnostics:
                    break
                assert time.monotonic() < deadline, 'missing audio credit wait and drop diagnostics'
                time.sleep(.02)
            audio.close();audio=None
            send(control,type='ping',nonce=456);assert control_message(control,'pong')['nonce']==456
            control.close();control=None
            time.sleep(.1)
            # Legacy clients do not negotiate an audio socket.
            control=socket.create_connection(('127.0.0.1',port),timeout=5)
            send(control,type='hello',version=3,token=token)
            _,legacy=packet(control);assert legacy['audio_pcm'] is False and legacy['audio_opus_v1'] is False
            bad=socket.create_connection(('127.0.0.1',port),timeout=3)
            send(bad,type='hello',version=3,token=token,channel='audio',media_key=legacy['media_key'])
            assert bad.recv(1)==b'';bad.close()
            codec = "Opus at 128 kb/s" if use_opus else "PCM"
            print(f'PASS: private {codec} playback stream (peak {peak}), sink isolation, auth/duplicate/legacy gating, four-frame credits and future-ACK rejection, credit and drop diagnostics, independent control, optional audio disconnect')
        finally:
            if decoder:decoder.close()
            if audio:audio.close()
            if control:control.close()
            for process,worker,_ in players:
                if process.poll() is None:process.kill()
                worker.join(timeout=2)
            host.terminate();host.wait(timeout=10)
            if decoy is not None:subprocess.run(['pactl','unload-module',str(decoy)],stdout=subprocess.DEVNULL,check=True)
            if sink:
                sinks=json.loads(command('pactl','--format=json','list','sinks'))
                assert not any(s['name']==sink for s in sinks),'private sink leaked after SIGTERM'
            assert command('pactl','get-default-sink')==before,'default sink changed'

if __name__=='__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--opus", action="store_true")
    main(parser.parse_args().opus)
