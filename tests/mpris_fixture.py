#!/usr/bin/env python3
"""Real, disposable Gio MPRIS2 player source for media_controls_v1 tests."""
import argparse
import json
import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

ROOT_XML = """<node><interface name="org.mpris.MediaPlayer2">
<property name="CanQuit" type="b" access="read"/>
<property name="CanRaise" type="b" access="read"/>
<property name="HasTrackList" type="b" access="read"/>
<property name="Identity" type="s" access="read"/>
<property name="DesktopEntry" type="s" access="read"/>
</interface></node>"""

PLAYER_XML = """<node><interface name="org.mpris.MediaPlayer2.Player">
<property name="PlaybackStatus" type="s" access="read"/>
<property name="Rate" type="d" access="read"/>
<property name="Metadata" type="a{sv}" access="read"/>
<property name="Position" type="x" access="read"/>
<property name="CanGoNext" type="b" access="read"/>
<property name="CanGoPrevious" type="b" access="read"/>
<property name="CanPlay" type="b" access="read"/>
<property name="CanPause" type="b" access="read"/>
<property name="CanSeek" type="b" access="read"/>
<property name="CanControl" type="b" access="read"/>
<method name="Play"/>
<method name="Pause"/>
<method name="PlayPause"/>
<method name="Stop"/>
<method name="Next"/>
<method name="Previous"/>
<method name="SetPosition">
  <arg name="TrackId" type="o" direction="in"/>
  <arg name="Position" type="x" direction="in"/>
</method>
</interface></node>"""

CONTROL_XML = """<node><interface name="org.waybridge.MprisFixture">
<method name="Update"><arg type="s" direction="in"/></method>
<method name="Stop"/>
</interface></node>"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", required=True)
    parser.add_argument("--control-name", required=True)
    parser.add_argument("--log", required=True)
    parser.add_argument("--initial", default="{}")
    parser.add_argument("--replace", action="store_true")
    args = parser.parse_args()

    state = {
        "identity": "Fixture Player",
        "desktop_entry": "",
        "status": "Playing",
        "title": "Opening Track",
        "artists": ["Fixture Artist"],
        "album": "Fixture Album",
        "album_artists": [],
        "track_id": "/org/mpris/MediaPlayer2/Track/1",
        "length_us": 180_000_000,
        "position_us": 10_000_000,
        "rate": 1.0,
        "art_url": "",
        "can_control": True,
        "can_play": True,
        "can_pause": True,
        "can_go_next": True,
        "can_go_previous": True,
        "can_seek": True,
    }
    state.update(json.loads(args.initial))

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    loop = GLib.MainLoop()

    def record(method_name, values):
        with open(args.log, "a", encoding="utf-8") as handle:
            handle.write(
                json.dumps(
                    {"player": args.name, "method": method_name, "args": values}
                )
                + "\n"
            )

    def metadata_variant():
        entries = {
            "mpris:trackid": GLib.Variant("o", state["track_id"]),
            "mpris:length": GLib.Variant("x", int(state["length_us"])),
            "xesam:title": GLib.Variant("s", state["title"]),
            "xesam:artist": GLib.Variant("as", list(state["artists"])),
            "xesam:album": GLib.Variant("s", state["album"]),
            "xesam:albumArtist": GLib.Variant("as", list(state["album_artists"])),
        }
        if state.get("art_url"):
            entries["mpris:artUrl"] = GLib.Variant("s", state["art_url"])
        return GLib.Variant("a{sv}", entries)

    def on_method(conn, sender, path, interface, name, params, invocation):
        values = list(params.unpack())
        if interface == "org.waybridge.MprisFixture":
            if name == "Update":
                state.update(json.loads(values[0]))
                invocation.return_value(None)
                return
            if name == "Stop":
                invocation.return_value(None)
                loop.quit()
                return
        record(name, values)
        if name == "Play":
            state["status"] = "Playing"
        elif name == "Pause":
            state["status"] = "Paused"
        elif name == "PlayPause":
            state["status"] = "Paused" if state["status"] == "Playing" else "Playing"
        elif name == "Stop":
            state["status"] = "Stopped"
        elif name == "SetPosition":
            state["position_us"] = int(values[1])
        invocation.return_value(None)

    def on_prop(conn, sender, path, interface, name):
        if interface == "org.mpris.MediaPlayer2":
            values = {
                "CanQuit": GLib.Variant("b", False),
                "CanRaise": GLib.Variant("b", False),
                "HasTrackList": GLib.Variant("b", False),
                "Identity": GLib.Variant("s", state["identity"]),
                "DesktopEntry": GLib.Variant("s", state["desktop_entry"]),
            }
            return values[name]
        values = {
            "PlaybackStatus": GLib.Variant("s", state["status"]),
            "Rate": GLib.Variant("d", float(state["rate"])),
            "Metadata": metadata_variant(),
            "Position": GLib.Variant("x", int(state["position_us"])),
            "CanGoNext": GLib.Variant("b", bool(state["can_go_next"])),
            "CanGoPrevious": GLib.Variant("b", bool(state["can_go_previous"])),
            "CanPlay": GLib.Variant("b", bool(state["can_play"])),
            "CanPause": GLib.Variant("b", bool(state["can_pause"])),
            "CanSeek": GLib.Variant("b", bool(state["can_seek"])),
            "CanControl": GLib.Variant("b", bool(state["can_control"])),
        }
        return values[name]

    for xml in (ROOT_XML, PLAYER_XML):
        bus.register_object(
            "/org/mpris/MediaPlayer2",
            Gio.DBusNodeInfo.new_for_xml(xml).interfaces[0],
            on_method,
            on_prop,
            None,
        )
    bus.register_object(
        "/Control",
        Gio.DBusNodeInfo.new_for_xml(CONTROL_XML).interfaces[0],
        on_method,
        on_prop,
        None,
    )
    flags = 7 if args.replace else 5
    bus.call_sync(
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "RequestName",
        GLib.Variant("(su)", (args.name, flags)),
        None,
        0,
        1000,
        None,
    )
    bus.call_sync(
        "org.freedesktop.DBus",
        "/org/freedesktop/DBus",
        "org.freedesktop.DBus",
        "RequestName",
        GLib.Variant("(su)", (args.control_name, flags)),
        None,
        0,
        1000,
        None,
    )
    print("ready", flush=True)
    loop.run()


if __name__ == "__main__":
    main()
