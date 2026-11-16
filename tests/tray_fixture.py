#!/usr/bin/env python3
"""Real, disposable Gio SNI + DBusMenu source (no session mocks in the adapter)."""
import argparse
import json
import os
from pathlib import Path
import sys
import gi
gi.require_version('Gio', '2.0')
from gi.repository import Gio, GLib

ITEM_XML = '''<node><interface name="org.kde.StatusNotifierItem">
<property name="Id" type="s" access="read"/><property name="DesktopEntry" type="s" access="read"/>
<property name="Title" type="s" access="read"/><property name="Status" type="s" access="read"/>
<property name="ItemIsMenu" type="b" access="read"/><property name="IconName" type="s" access="read"/>
<property name="IconPixmap" type="a(iiay)" access="read"/><property name="Menu" type="o" access="read"/>
<method name="Activate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
<method name="SecondaryActivate"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
<method name="ContextMenu"><arg type="i" direction="in"/><arg type="i" direction="in"/></method>
<method name="Scroll"><arg type="i" direction="in"/><arg type="s" direction="in"/></method>
</interface></node>'''
MENU_XML = '''<node><interface name="com.canonical.dbusmenu">
<method name="GetLayout"><arg type="i" direction="in"/><arg type="i" direction="in"/><arg type="as" direction="in"/>
<arg type="u" direction="out"/><arg type="(ia{sv}av)" direction="out"/></method>
<method name="AboutToShow"><arg type="i" direction="in"/><arg type="b" direction="out"/></method>
<method name="Event"><arg type="i" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="in"/><arg type="u" direction="in"/></method>
</interface></node>'''
CONTROL_XML = '''<node><interface name="org.waybridge.TrayFixture">
<method name="SetStatus"><arg type="s" direction="in"/></method><method name="BumpRevision"/>
<method name="SetLayoutDelay"><arg type="i" direction="in"/></method><method name="Stop"/>
</interface></node>'''
WATCHER_XML = '''<node><interface name="org.kde.StatusNotifierWatcher">
<property name="RegisteredStatusNotifierItems" type="as" access="read"/>
<property name="IsStatusNotifierHostRegistered" type="b" access="read"/><property name="ProtocolVersion" type="i" access="read"/>
<method name="RegisterStatusNotifierHost"><arg type="s" direction="in"/></method>
<method name="RegisterStatusNotifierItem"><arg type="s" direction="in"/></method>
<signal name="StatusNotifierItemRegistered"><arg type="s"/></signal>
</interface></node>'''

def main():
    p=argparse.ArgumentParser();p.add_argument('--log',required=True);p.add_argument('--replace',action='store_true');p.add_argument('--watcher',action='store_true');p.add_argument('--context-only',action='store_true');p.add_argument('--freedesktop',action='store_true');args=p.parse_args()
    bus=Gio.bus_get_sync(Gio.BusType.SESSION,None);loop=GLib.MainLoop()
    state={'status':'Active','revision':1,'layout_delay':0};items=[]
    def record(name,data):
        with open(args.log,'a') as f:f.write(json.dumps({'method':name,'args':data})+'\n')
    def leaf(i,label,**props):
        values={'label':GLib.Variant('s',label)}
        values.update({k.replace('_','-'):GLib.Variant('b' if isinstance(v,bool) else 'i' if isinstance(v,int) else 's',v) for k,v in props.items()})
        return GLib.Variant('(ia{sv}av)',(i,values,[]))
    def tree():
        children=[leaf(1,'_Restore'),leaf(2,'Disabled',enabled=False),leaf(3,'Checked',toggle_type='checkmark',toggle_state=1),
          GLib.Variant('(ia{sv}av)',(4,{'label':GLib.Variant('s','Submenu')},[leaf(5,'Radio',toggle_type='radio',toggle_state=1)])),leaf(6,'Quit'),leaf(7,'Invisible',visible=False),
          GLib.Variant('(ia{sv}av)',(8,{'label':GLib.Variant('s','Disabled parent'),'enabled':GLib.Variant('b',False)},[leaf(9,'Forbidden child')])),
          GLib.Variant('(ia{sv}av)',(10,{'label':GLib.Variant('s','Hidden parent'),'visible':GLib.Variant('b',False)},[leaf(11,'Hidden child')]))]
        return (0,{},children)
    def method(conn,sender,path,interface,name,params,invocation):
        values=params.unpack();record(name,values)
        if name=='GetLayout':
            def reply():invocation.return_value(GLib.Variant('(u(ia{sv}av))',(state['revision'],tree())));return False
            if values[1]==0 and state['layout_delay']:GLib.timeout_add(state['layout_delay'],reply)
            else:reply()
        elif name=='AboutToShow':invocation.return_value(GLib.Variant('(b)',(False,)))
        elif name=='SetStatus':state['status']=values[0];invocation.return_value(None)
        elif name=='BumpRevision':state['revision']+=1;invocation.return_value(None)
        elif name=='SetLayoutDelay':state['layout_delay']=values[0];invocation.return_value(None)
        elif name=='Stop':invocation.return_value(None);loop.quit()
        elif name=='RegisterStatusNotifierItem':
            key=sender+values[0] if values[0].startswith('/') else values[0]+'/StatusNotifierItem';items.append(key)
            invocation.return_value(None)
        else:invocation.return_value(None)
    def prop(conn,sender,path,interface,name):
        values={'Id':GLib.Variant('s','org.waybridge.TrayFixture'),'DesktopEntry':GLib.Variant('s','org.waybridge.TrayFixture'),
          'Title':GLib.Variant('s','Tray fixture'),'Status':GLib.Variant('s',state['status']),
          'ItemIsMenu':GLib.Variant('b',False),'IconName':GLib.Variant('s',''),
          'IconPixmap':GLib.Variant('a(iiay)',[(16,16,bytes([255,255,0,0])*256)]),
          'Menu':GLib.Variant('o','/' if args.context_only else '/Menu'),
          'RegisteredStatusNotifierItems':GLib.Variant('as',items),'IsStatusNotifierHostRegistered':GLib.Variant('b',True),'ProtocolVersion':GLib.Variant('i',0)}
        return values[name]
    if args.watcher:
        bus.register_object('/StatusNotifierWatcher',Gio.DBusNodeInfo.new_for_xml(WATCHER_XML).interfaces[0],method,prop,None)
        bus.call_sync('org.freedesktop.DBus','/org/freedesktop/DBus','org.freedesktop.DBus','RequestName',GLib.Variant('(su)',('org.kde.StatusNotifierWatcher',4)),None,0,1000,None)
    else:
        xml=ITEM_XML.replace('org.kde.StatusNotifierItem','org.freedesktop.StatusNotifierItem') if args.freedesktop else ITEM_XML
        for path,xml in [('/StatusNotifierItem',xml),('/Menu',MENU_XML),('/Control',CONTROL_XML)]:
            bus.register_object(path,Gio.DBusNodeInfo.new_for_xml(xml).interfaces[0],method,prop,None)
        flags=7 if args.replace else 5
        bus.call_sync('org.freedesktop.DBus','/org/freedesktop/DBus','org.freedesktop.DBus','RequestName',GLib.Variant('(su)',('org.waybridge.TrayFixture',flags)),None,0,1000,None)
        def register():
            try:bus.call_sync('org.kde.StatusNotifierWatcher','/StatusNotifierWatcher','org.kde.StatusNotifierWatcher','RegisterStatusNotifierItem',GLib.Variant('(s)',('org.waybridge.TrayFixture',)),None,0,1000,None)
            except GLib.Error:return True
            print('ready',flush=True);return False
        GLib.timeout_add(100,register)
    if args.watcher:print('ready',flush=True)
    loop.run()
if __name__=='__main__':main()
