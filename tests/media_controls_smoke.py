#!/usr/bin/env python3
"""Private D-Bus MPRIS2 vertical integration against the real compositor."""
import base64
import io
import json
import socket
import subprocess
import tempfile
import time
from pathlib import Path
from PIL import Image
from smoke import Viewer, ROOT
from session_fixture import SessionFixture


class MediaViewer(Viewer):
    def __init__(self, port, token, enabled=True, initial_enabled=True):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.geometry = {}
        self.send(
            type="hello",
            version=3,
            token=token,
            media_controls_v1=enabled,
            media_controls_enabled=initial_enabled,
        )
        hello = self.receive()[1]
        assert hello["media_controls_v1"] == enabled, hello
        assert "media_controls_v1" in hello["features"], hello

    def control(self, name, predicate=lambda value: True):
        return self.until(
            lambda kind, value: kind == "control"
            and value.get("type") == name
            and predicate(value)
        )


def main():
    with tempfile.TemporaryDirectory(prefix="waybridge-media-") as temporary, SessionFixture(
        temporary
    ) as fixture:
        env = {**fixture.env, "GVFS_DISABLE_FUSE": "1", "WAYLAND_DISPLAY": "media-0"}
        root = Path(temporary)
        log = (root / "host.log").open("w", encoding="utf-8")
        calls_path = root / "calls.jsonl"
        calls_path.write_text("", encoding="utf-8")
        children = []

        # Create real PNG and extensionless JPEG cover files (like Chromium's temp art files).
        png_path = root / "cover.png"
        Image.new("RGBA", (320, 160), (20, 120, 240, 255)).save(png_path, format="PNG")
        jpg_path = root / "chromium_art_no_ext"
        Image.new("RGB", (200, 200), (230, 80, 40)).save(jpg_path, format="JPEG")

        with socket.socket() as free:
            free.bind(("127.0.0.1", 0))
            port = free.getsockname()[1]
        token = "media-" + 40 * "4"
        token_path = root / "token"
        token_path.write_text(token, encoding="utf-8")
        token_path.chmod(0o600)

        def start_player(name, control_name, initial, replace=False):
            cmd = [
                "/usr/bin/python3",
                "tests/mpris_fixture.py",
                "--name",
                name,
                "--control-name",
                control_name,
                "--log",
                str(calls_path),
                "--initial",
                json.dumps(initial),
            ]
            if replace:
                cmd.append("--replace")
            child = subprocess.Popen(
                cmd,
                cwd=ROOT,
                env=env,
                stdout=subprocess.PIPE,
                stderr=log,
                text=True,
            )
            children.append(child)
            assert child.stdout.readline().strip() == "ready", (root / "host.log").read_text()
            return child

        def update_player(control_name, patch):
            subprocess.run(
                [
                    "gdbus",
                    "call",
                    "--session",
                    "--dest",
                    control_name,
                    "--object-path",
                    "/Control",
                    "--method",
                    "org.waybridge.MprisFixture.Update",
                    json.dumps(patch),
                ],
                env=env,
                stdout=subprocess.DEVNULL,
                check=True,
            )

        def calls():
            return [
                json.loads(line)
                for line in calls_path.read_text(encoding="utf-8").splitlines()
                if line.strip()
            ]

        def wait_call(predicate, timeout=3.0):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                found = [c for c in calls() if predicate(c)]
                if found:
                    return found
                time.sleep(0.03)
            raise AssertionError(f"Expected D-Bus call never arrived: {calls()}")

        try:
            host = subprocess.Popen(
                [
                    str(ROOT / "target/debug/waybridge"),
                    "--listen",
                    f"127.0.0.1:{port}",
                    "--socket",
                    "media-0",
                    "--token-file",
                    str(token_path),
                    "--no-audio",
                    "--codec",
                    "png",
                ],
                env=env,
                stdout=log,
                stderr=log,
            )
            children.append(host)
            deadline = time.monotonic() + 20
            while not (Path(env["XDG_RUNTIME_DIR"]) / "media-0").exists():
                assert host.poll() is None, (root / "host.log").read_text()
                assert time.monotonic() < deadline
                time.sleep(0.05)

            viewer = MediaViewer(port, token)
            p1_name = "org.mpris.MediaPlayer2.waybridge_one"
            p1_ctrl = "org.waybridge.MprisFixture.One"
            p1 = start_player(
                p1_name,
                p1_ctrl,
                {
                    "identity": "Player One",
                    "status": "Playing",
                    "title": "First Track",
                    "artists": ["Artist A", "Artist B"],
                    "album": "Album One",
                    "art_url": f"file://{png_path}",
                    "length_us": 200_000_000,
                    "position_us": 15_000_000,
                },
            )

            state = viewer.control("media_state", lambda m: m["player"] == p1_name)
            assert state["title"] == "First Track", state
            assert state["artists"] == ["Artist A", "Artist B"], state
            assert state["album"] == "Album One", state
            assert state["identity"] == "Player One", state
            assert state["status"] == "Playing", state
            assert state["art_revision"] > 0, state

            art = viewer.control("media_art", lambda m: m["revision"] == state["art_revision"])
            assert art["mime"] == "image/png", art
            decoded = Image.open(io.BytesIO(base64.b64decode(art["data"])))
            assert decoded.size == (256, 128), decoded.size

            # Pause via media_control -> reaches real D-Bus player, updates state, does NOT resend artwork.
            viewer.send(
                type="media_control",
                epoch=state["epoch"],
                player=p1_name,
                generation=state["generation"],
                action="pause",
            )
            wait_call(lambda c: c["player"] == p1_name and c["method"] == "Pause")
            paused = viewer.control("media_state", lambda m: m["status"] == "Paused")
            assert paused["art_revision"] == state["art_revision"], paused
            viewer.send(type="ping", nonce=101)
            while True:
                kind, value = viewer.receive()
                assert kind != "control" or value["type"] != "media_art", "unchanged art retransmitted"
                if kind == "control" and value["type"] == "pong" and value["nonce"] == 101:
                    break

            # Exercise remaining actions: play, next, previous, set_position, stop.
            for action_name, method_name in [
                ("play", "Play"),
                ("next", "Next"),
                ("previous", "Previous"),
                ("play_pause", "PlayPause"),
                ("stop", "Stop"),
            ]:
                viewer.send(
                    type="media_control",
                    epoch=state["epoch"],
                    player=p1_name,
                    generation=state["generation"],
                    action=action_name,
                )
                wait_call(lambda c, m=method_name: c["player"] == p1_name and c["method"] == m)

            viewer.send(
                type="media_control",
                epoch=state["epoch"],
                player=p1_name,
                generation=state["generation"],
                action="set_position",
                track_id=state["track_id"],
                position_us=42_000_000,
            )
            wait_call(
                lambda c: c["player"] == p1_name
                and c["method"] == "SetPosition"
                and c["args"] == [state["track_id"], 42_000_000]
            )
            seeked = viewer.control("media_state", lambda m: m.get("position_us") == 42_000_000)

            # Extensionless JPEG artwork (like Chromium) updates art_revision and mime.
            update_player(p1_ctrl, {"status": "Paused", "art_url": f"file://{jpg_path}"})
            jpg_state = viewer.control(
                "media_state", lambda m: m["art_revision"] != seeked["art_revision"]
            )
            jpg_art = viewer.control(
                "media_art", lambda m: m["revision"] == jpg_state["art_revision"]
            )
            assert jpg_art["mime"] == "image/jpeg", jpg_art

            # Second player starts Playing while Player 1 is Paused -> takes priority.
            p2_name = "org.mpris.MediaPlayer2.waybridge_two"
            p2_ctrl = "org.waybridge.MprisFixture.Two"
            p2 = start_player(
                p2_name,
                p2_ctrl,
                {
                    "identity": "Player Two",
                    "status": "Playing",
                    "title": "Second Track",
                    "artists": ["Artist Two"],
                    "album": "Album Two",
                },
            )
            s2 = viewer.control("media_state", lambda m: m["player"] == p2_name)
            assert s2["title"] == "Second Track", s2

            # Read-only mode blocks media_control server-side while metadata updates still flow.
            before_pause = len([c for c in calls() if c["player"] == p2_name and c["method"] == "Pause"])
            viewer.send(
                type="stream_config", request_id=1, fps=60, bitrate=30000, codec="png", read_only=True
            )
            viewer.control("stream_config")
            viewer.send(
                type="media_control",
                epoch=s2["epoch"],
                player=p2_name,
                generation=s2["generation"],
                action="pause",
            )
            viewer.send(type="ping", nonce=202)
            viewer.control("pong")
            time.sleep(0.25)
            assert (
                len([c for c in calls() if c["player"] == p2_name and c["method"] == "Pause"])
                == before_pause
            ), "read_only failed to block media_control"
            update_player(p2_ctrl, {"title": "Second Track Updated"})
            viewer.control("media_state", lambda m: m["title"] == "Second Track Updated")
            viewer.send(
                type="stream_config", request_id=2, fps=60, bitrate=30000, codec="png", read_only=False
            )
            viewer.control("stream_config")

            # Player 2 exits -> falls back to Player 1.
            p2.terminate()
            p2.wait(timeout=3)
            back = viewer.control("media_state", lambda m: m["player"] == p1_name)

            # Replacing bus name owner bumps generation and rejects stale generation commands.
            before_next = len([c for c in calls() if c["player"] == p1_name and c["method"] == "Next"])
            p1_replacement = start_player(
                p1_name,
                p1_ctrl,
                {
                    "identity": "Player One Replaced",
                    "status": "Playing",
                    "title": "Fresh Owner Track",
                },
                replace=True,
            )
            viewer.send(
                type="media_control",
                epoch=back["epoch"],
                player=p1_name,
                generation=back["generation"],
                action="next",
            )
            replaced = viewer.control(
                "media_state",
                lambda m: m["player"] == p1_name and m["generation"] != back["generation"],
            )
            time.sleep(0.2)
            assert (
                len([c for c in calls() if c["player"] == p1_name and c["method"] == "Next"])
                == before_next
            ), "stale generation acted on replaced player"

            # Disabling media_controls_enabled sends media_reset; re-enabling restores state with new epoch.
            viewer.send(type="media_controls_enabled", enabled=False)
            viewer.control("media_reset")
            viewer.send(
                type="media_control",
                epoch=replaced["epoch"],
                player=p1_name,
                generation=replaced["generation"],
                action="next",
            )
            viewer.send(type="ping", nonce=303)
            viewer.control("pong")
            time.sleep(0.2)
            assert (
                len([c for c in calls() if c["player"] == p1_name and c["method"] == "Next"])
                == before_next
            ), "disabled media controls still executed action"

            viewer.send(type="media_controls_enabled", enabled=True)
            resumed = viewer.control("media_state", lambda m: m["player"] == p1_name)
            assert resumed["epoch"] != replaced["epoch"], resumed

            # Reconnect bumps epoch and rejects commands with the old epoch.
            old_epoch = resumed["epoch"]
            viewer.sock.close()
            time.sleep(0.2)
            viewer = MediaViewer(port, token)
            reconnected = viewer.control("media_state", lambda m: m["player"] == p1_name)
            assert reconnected["epoch"] != old_epoch, reconnected
            viewer.send(
                type="media_control",
                epoch=old_epoch,
                player=p1_name,
                generation=reconnected["generation"],
                action="next",
            )
            viewer.send(type="ping", nonce=404)
            viewer.control("pong")
            time.sleep(0.2)
            assert (
                len([c for c in calls() if c["player"] == p1_name and c["method"] == "Next"])
                == before_next
            ), "stale epoch from previous connection executed action"

            # All players exit -> media_reset.
            p1_replacement.terminate()
            p1_replacement.wait(timeout=3)
            p1.terminate()
            p1.wait(timeout=3)
            viewer.control("media_reset")
            viewer.sock.close()
            print("PASS: media_controls_v1 MPRIS2 metadata, artwork, controls, priority, read-only, and lifecycle")
        finally:
            for child in children:
                if child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        child.kill()
            log.close()


if __name__ == "__main__":
    main()
