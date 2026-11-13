"""Isolated UDP fault injector; forwards only between one local test server/client."""
import socket,threading
class Proxy:
    def __init__(self,server,faults=True,blackhole_after=None):
        self.socket=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        self.socket.bind(('127.0.0.1',0));self.socket.settimeout(.05)
        self.port=self.socket.getsockname()[1];self.server=server;self.client=None
        self.live=True;self.faults=faults;self.received=self.dropped=self.corrupted=self.replayed=0
        self.blackhole_after=blackhole_after
        self.thread=threading.Thread(target=self.run);self.thread.start()
    def run(self):
        pending=None
        while self.live:
            try:data,source=self.socket.recvfrom(4096)
            except socket.timeout:
                if pending and self.client:self.socket.sendto(pending,self.client);pending=None
                continue
            except OSError:break
            if source!=self.server:
                self.client=source;self.socket.sendto(data,self.server);continue
            self.received+=1
            if not self.client:continue
            if self.blackhole_after and self.received>self.blackhole_after:self.dropped+=1;continue
            n=int.from_bytes(data[12:20],'big')
            if self.faults and n%37 in (0,1):self.dropped+=1;continue
            if self.faults and n%41==0:
                data=data[:-1]+bytes([data[-1]^128]);self.corrupted+=1
            if self.faults and n%7==0 and pending is None:pending=data;continue
            self.socket.sendto(data,self.client)
            if self.faults and n%13==0:self.socket.sendto(data,self.client);self.replayed+=1
            if pending:self.socket.sendto(pending,self.client);pending=None
    def close(self):
        self.live=False;self.thread.join(timeout=2);self.socket.close()
