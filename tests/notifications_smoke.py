#!/usr/bin/python3
"""Standard provider against a disposable bus and private mock session bridge.

Never acquires a name on the user's bus or reads an application/wallet profile.
"""
import json
import base64
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

ROOT = Path(__file__).resolve().parents[1]
NAME = 'org.freedesktop.Notifications'
OBJECT = '/org/freedesktop/Notifications'

def main():
    with tempfile.TemporaryDirectory(prefix='wb-notify-') as directory:
        root = Path(directory)
        runtime = root/'runtime'; runtime.mkdir(mode=0o700)
        address, pid = subprocess.check_output(['dbus-daemon', '--session', '--fork', '--print-address=1', '--print-pid=1'], text=True).splitlines()
        bridge = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        bridge.bind(str(runtime/'notifications.sock')); bridge.settimeout(4)
        environment = {**os.environ, 'DBUS_SESSION_BUS_ADDRESS':address, 'WAYBRIDGE_SESSION_ID':str(runtime), 'HOME':str(root), 'XDG_CONFIG_HOME':str(root/'config'), 'XDG_DATA_HOME':str(root/'data'), 'XDG_DATA_DIRS':str(root/'data')}
        provider = subprocess.Popen([str(ROOT/'target/debug/waybridge'), '--notification-provider'], env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        flags = Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION
        bus = Gio.DBusConnection.new_for_address_sync(address, flags, None, None)
        other = Gio.DBusConnection.new_for_address_sync(address, flags, None, None)
        signals = []
        bus.signal_subscribe(None, NAME, None, OBJECT, None, Gio.DBusSignalFlags.NONE,
            lambda c,s,p,i,m,v,u: signals.append((m, v.unpack())), None)
        binding = '1'*32
        destination = None
        def receive(kind):
            nonlocal destination
            until = time.monotonic()+4
            while time.monotonic()<until:
                packet, source = bridge.recvfrom(65536); packet = json.loads(packet)
                if packet['type'] == 'notification_register':
                    destination = source
                    bridge.sendto(json.dumps(dict(type='notification_state',binding=binding,enabled=True)).encode(), source)
                if packet['type'] == kind: return packet
            raise AssertionError('missing bounded bridge message: '+kind)
        def call(method, signature=None, values=None, connection=bus):
            return connection.call_sync(NAME, OBJECT, NAME, method, GLib.Variant(signature,values) if signature else None,
                None, Gio.DBusCallFlags.NONE, 3000, None).unpack()
        def notify(replace=0, timeout=10000, connection=bus, actions=None, body='中文正文'):
            return call('Notify','(susssasa{sv}i)',('测试应用',replace,'','通知标题',body,actions or ['default','打开','answer','回答'],{},timeout), connection)[0]
        def wait_signal(member, values):
            until=time.monotonic()+3
            while time.monotonic()<until:
                while GLib.MainContext.default().iteration(False): pass
                if (member, values) in signals: return
                time.sleep(.01)
            raise AssertionError((member,values,signals))
        def feedback(message, **fields):
            bridge.sendto(json.dumps({**message, 'type':'notification_feedback',**fields}).encode(),destination)
        try:
            receive('notification_register'); time.sleep(.08)
            assert set(call('GetCapabilities')[0]) == {'body','actions','icon-static'}
            assert call('GetServerInformation')[-1]=='1.2'
            try:notify(body='x'*8193);raise AssertionError('oversize body accepted')
            except GLib.Error as error:assert 'InvalidArgs' in str(error)
            try:notify(actions=['a','one','a','duplicate']);raise AssertionError('duplicate action accepted')
            except GLib.Error as error:assert 'InvalidArgs' in str(error)
            raw=GLib.Variant('(iiibiiay)',(2,1,8,True,8,4,[255,0,0,255,0,255,0,255]))
            image_id=call('Notify','(susssasa{sv}i)',('image fixture',0,'','icon','',[],{'image-data':raw},10000))[0]
            icon=receive('notification');assert base64.b64decode(icon['png']).startswith(b'\x89PNG\r\n\x1a\n')
            call('CloseNotification','(u)',(image_id,));wait_signal('NotificationClosed',(image_id,3));receive('notification_close')
            signals.clear()
            first=notify(); old=receive('notification')
            assert old['summary']=='通知标题' and old['body']=='中文正文'
            assert notify(first)==first
            new=receive('notification'); assert new['revision']>old['revision']
            # Replacement invalidates an action from the previous revision.
            feedback(old,action='answer'); time.sleep(.08)
            assert not signals
            feedback(new,action='answer')
            wait_signal('ActionInvoked',(first,'answer')); wait_signal('NotificationClosed',(first,2))
            receive('notification_close')
            expired=notify(timeout=100); receive('notification')
            wait_signal('NotificationClosed',(expired,1)); receive('notification_close')
            closed=notify(timeout=0); receive('notification')
            try: call('CloseNotification','(u)',(closed,),other); raise AssertionError('cross-owner close accepted')
            except GLib.Error as error: assert 'AccessDenied' in str(error)
            try: notify(closed,connection=other); raise AssertionError('cross-owner replacement accepted')
            except GLib.Error as error: assert 'AccessDenied' in str(error)
            call('CloseNotification','(u)',(closed,)); wait_signal('NotificationClosed',(closed,3)); receive('notification_close')
            try: call('CloseNotification','(u)',(closed,)); raise AssertionError('nonexistent close accepted')
            except GLib.Error as error: assert 'InvalidArgs' in str(error)
            # Same source but old connection binding cannot execute actions.
            reconnect=notify(timeout=0); stale=receive('notification')
            binding='2'*32
            bridge.sendto(json.dumps(dict(type='notification_state',binding=binding,enabled=False)).encode(),destination)
            wait_signal('NotificationClosed',(reconnect,4)); receive('notification_close')
            before=len(signals); feedback(stale,action='answer'); time.sleep(.1)
            while GLib.MainContext.default().iteration(False): pass
            assert len(signals)==before
            try: notify(); raise AssertionError('offline delivery accepted')
            except GLib.Error as error: assert 'no enabled, capable viewer' in str(error)
            # Existing notification provider remains the owner, with no replacement.
            second=subprocess.run([str(ROOT/'target/debug/waybridge'),'--notification-provider'],env=environment,capture_output=True,text=True,timeout=5)
            assert second.returncode!=0 and ('already has a provider' in second.stderr or 'taken' in second.stderr.lower())
            assert call('GetServerInformation')[0]=='Waybridge'
            print('PASS: standard D-Bus methods/signals, Chinese, replacement and stale-revision rejection, expiry/close/owner isolation, disconnect/reset, explicit-provider conflict')
        finally:
            provider.terminate()
            try: stdout,stderr=provider.communicate(timeout=5)
            except subprocess.TimeoutExpired: provider.kill(); stdout,stderr=provider.communicate()
            assert b'\xe4\xb8\xad\xe6\x96\x87' not in stdout+stderr, 'notification text leaked into logs'
            other.close_sync(None);bus.close_sync(None);bridge.close();os.kill(int(pid),15)

if __name__=='__main__': main()
