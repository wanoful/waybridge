#!/usr/bin/python3
"""Real compositor, disposable user bus, explicit provider and viewer protocol."""
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import tempfile
import time
import gi
gi.require_version('Gio','2.0')
from gi.repository import Gio,GLib
from session_fixture import SessionFixture

ROOT=Path(__file__).resolve().parents[1]
NAME='org.freedesktop.Notifications'; OBJECT='/org/freedesktop/Notifications'

class Viewer:
    def __init__(self,port,token,enabled=True):
        self.socket=socket.create_connection(('127.0.0.1',port),timeout=10)
        self.send(type='hello',version=3,token=token,notifications_v1=True,notifications_enabled=enabled)
        assert self.receive()['notifications_v1'] is True
    def send(self,**value):
        data=b'\0'+json.dumps(value).encode();self.socket.sendall(struct.pack('>I',len(data))+data)
    def exact(self,size):
        data=b''
        while len(data)<size:
            piece=self.socket.recv(size-len(data));assert piece;data+=piece
        return data
    def receive(self):
        size=struct.unpack('>I',self.exact(4))[0];data=self.exact(size);assert data[0]==0;return json.loads(data[1:])
    def until(self,kind,predicate=lambda _:True):
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            value=self.receive()
            if value['type']==kind and predicate(value): return value
        raise AssertionError('missing '+kind)
    def close(self):self.socket.close()

def main():
    with tempfile.TemporaryDirectory(prefix='wb-notify-bridge-') as directory, SessionFixture(Path(directory)/'fixture') as fixture:
        runtime=Path(fixture.env['XDG_RUNTIME_DIR'])
        with socket.socket() as free:free.bind(('127.0.0.1',0));port=free.getsockname()[1]
        token='notify-bridge-'+('a'*40);(runtime/'token').write_text(token);(runtime/'token').chmod(0o600)
        log=(Path(directory)/'compositor.log').open('w+')
        host=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--no-audio','--no-xwayland','--codec','png','--socket','notify-fixture','--token-file',str(runtime/'token'),'--listen',f'127.0.0.1:{port}'],env=fixture.env,stdout=log,stderr=log)
        provider=None;viewer=None;other=None
        bus=Gio.DBusConnection.new_for_address_sync(fixture.env['DBUS_SESSION_BUS_ADDRESS'],Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT|Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,None,None)
        def notify():
            return bus.call_sync(NAME,OBJECT,NAME,'Notify',GLib.Variant('(susssasa{sv}i)',('fixture',0,'','通知','','default 打开'.split(),{},10000)),None,Gio.DBusCallFlags.NONE,3000,None).unpack()[0]
        try:
            deadline=time.monotonic()+15
            descriptor=runtime/'notify-fixture.session.json'
            while not descriptor.exists():
                assert host.poll() is None,'compositor failed: '+(Path(directory)/'compositor.log').read_text()
                assert time.monotonic()<deadline;time.sleep(.03)
            viewer=Viewer(port,token)
            assert viewer.until('notification_status')['available'] is False
            environment=json.loads(descriptor.read_text())['environment']
            provider=subprocess.Popen([str(ROOT/'target/debug/waybridge'),'--notification-provider'],env={**fixture.env,'WAYBRIDGE_SESSION_ID':environment['WAYBRIDGE_SESSION_RUNTIME']},stdout=log,stderr=log)
            viewer.until('notification_status',lambda v:v['available'])
            time.sleep(.08)
            id=notify();value=viewer.until('notification');assert value['id']==id and value['summary']=='通知'
            viewer.send(**{**value,'type':'notification_feedback','action':'default'})
            viewer.until('notification_close')
            # Disconnect drops all pending notifications, including resident ones.
            notify();viewer.until('notification');viewer.close();viewer=None;time.sleep(.15)
            viewer=Viewer(port,token,enabled=False)
            viewer.until('notification_status',lambda v:v['available']);time.sleep(.1)
            try:notify();raise AssertionError('viewer opt-out ignored')
            except GLib.Error:pass
            viewer.send(type='notifications_enabled',enabled=True);time.sleep(.1)
            id=notify();assert viewer.until('notification')['id']==id
            provider.terminate();provider.wait(timeout=5);provider=None
            viewer.until('notification_reset')
            viewer.until('notification_status',lambda v:not v['available'])
            # A different selected provider is diagnosed, never observed as if it
            # were a source we could forward or replaced by the compositor.
            other=Gio.DBusConnection.new_for_address_sync(fixture.env['DBUS_SESSION_BUS_ADDRESS'],Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT|Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,None,None)
            other.call_sync('org.freedesktop.DBus','/org/freedesktop/DBus','org.freedesktop.DBus','RequestName',GLib.Variant('(su)',(NAME,4)),None,Gio.DBusCallFlags.NONE,3000,None)
            status=viewer.until('notification_status',lambda v:v.get('reason')=='provider_conflict')
            assert not status['available']
            print('PASS: real compositor notifications_v1 transport/action, explicit opt-in provider diagnostics, live sharing switch, disconnect reset, provider lease-loss cleanup, shared-bus conflict without takeover')
        finally:
            if viewer:viewer.close()
            if provider:provider.terminate();provider.wait(timeout=5)
            if other:other.close_sync(None)
            bus.close_sync(None);host.terminate();host.wait(timeout=15);log.close()

if __name__=='__main__':main()
