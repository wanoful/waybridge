#!/usr/bin/env python3
"""Read-only CLI configuration contract; no GPU, bus, or live user state."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BINARIES = [ROOT / "target/debug/waybridge", ROOT / "target/debug/waybridge-service"]


def main():
    with tempfile.TemporaryDirectory(prefix="waybridge-config-") as temporary:
        root = Path(temporary)
        home, config, runtime, state, cwd = [root / name for name in ("home", "config", "runtime", "state", "cwd")]
        for path in (home, config, runtime, state, cwd):
            path.mkdir(mode=0o700)
        env = dict(os.environ, HOME=str(home), XDG_CONFIG_HOME=str(config),
                   XDG_RUNTIME_DIR=str(runtime), XDG_STATE_HOME=str(state))

        def run(binary, *args, success=True, environment=None):
            result = subprocess.run([binary, *args], cwd=cwd, env=environment or env,
                                    capture_output=True, text=True, timeout=10)
            assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
            if success:
                return json.loads(result.stdout)
            assert not result.stdout
            return result.stderr

        # No implicit configuration discovery in an arbitrary working directory.
        (cwd / "config.toml").write_text("this is deliberately invalid")
        defaults = [run(binary, "--print-config") for binary in BINARIES]
        assert defaults[0]["values"] == defaults[1]["values"]
        assert defaults[0]["values"]["session"]["fps"] == 60
        assert not defaults[0]["config_file"]["loaded"]
        assert defaults[0]["sources"]["session.fps"] == {"kind": "default"}

        user_file = config / "waybridge/config.toml"
        user_file.parent.mkdir()
        original = """[service]
listen = '127.0.0.1:49992'
compositor = '/usr/local/bin/waybridge'
[session]
fps = 37
bitrate = 17000
codec = 'png'
[limits]
max_fps = 75
max_bitrate = 55000
allowed_codecs = ['png', 'h264']
"""
        user_file.write_text(original)
        for binary in BINARIES:
            report = run(binary, "--print-config")
            assert report["values"]["session"]["fps"] == 37, report
            assert report["values"]["session"]["bitrate"] == 17000
            assert report["sources"]["session.fps"] == {"kind": "file", "path": str(user_file)}
            assert report["sources"]["session.layout"] == {"kind": "default"}
            assert report["apply"]["service.listen"]["phase"] == "service_restart"
            assert report["apply"]["session.fps"]["runtime_request"]
            assert run(binary, "--check-config")["valid"]
        report = run(BINARIES[0], "--fps", "49", "--print-config")
        assert report["values"]["session"]["fps"] == 49
        assert report["values"]["session"]["bitrate"] == 17000
        assert report["sources"]["session.fps"] == {"kind": "cli"}
        for args in [("--fps", "76"), ("--bitrate", "55001"), ("--codec", "hevc-nvenc")]:
            run(BINARIES[0], *args, "--check-config", success=False)
        report = run(BINARIES[1], "serve", "--listen", "127.0.0.1:49993", "--print-config")
        assert report["values"]["service"]["listen"] == "127.0.0.1:49993"
        assert report["sources"]["service.listen"] == {"kind": "cli"}

        explicit = cwd / "explicit.toml"
        explicit.write_text("[session]\nfps = 42\ncodec = 'png'\nsocket = '/tmp/waybridge-config-test'\n")
        for binary in BINARIES:
            report = run(binary, "--config", "explicit.toml", "--print-config")
            assert report["values"]["session"]["fps"] == 42
            assert report["values"]["session"]["bitrate"] == 30000
            assert report["values"]["session"]["socket"] == "/tmp/waybridge-config-test"
            assert report["config_file"] == {"path": str(explicit), "explicit": True, "loaded": True}
            run(binary, "--config", cwd / "missing.toml", "--check-config", success=False)
            for invalid in ["[session]\nunknown = 'credential-must-not-appear'",
                            "[session]\nfps = 'credential-must-not-appear'",
                            "[session]\nfps = 0", "[limits]\nmax_fps = 30"]:
                explicit.write_text(invalid)
                error = run(binary, "--config", explicit, "--check-config", success=False)
                assert "credential-must-not-appear" not in error
            explicit.write_text("[session]\nfps = 42\ncodec = 'png'\nsocket = '/tmp/waybridge-config-test'\n")

        fallback = home / ".config/waybridge/config.toml"
        fallback.parent.mkdir(parents=True)
        fallback.write_text("[session]\nfps = 43\n")
        for invalid_xdg in ("", "relative"):
            fallback_env = dict(env, XDG_CONFIG_HOME=invalid_xdg)
            for binary in BINARIES:
                report = run(binary, "--print-config", environment=fallback_env)
                assert report["values"]["session"]["fps"] == 43
                assert report["config_file"]["path"] == str(fallback)

        user_file.write_text("[session]\nfps = 'broken-next-session-value'\n")
        error = run(BINARIES[1], "status", success=False)
        assert "configuration" not in error and "No such file" in error
        error = run(BINARIES[0], "--launch", "--socket", "absent", "--", "/bin/true", success=False)
        assert "configuration" not in error, error
        # With no explicit socket, configuration is still needed to select it.
        error = run(BINARIES[0], "--launch", "--", "/bin/true", success=False)
        assert "configuration" in error, error
        user_file.write_text(original)

        assert user_file.read_text() == original, "configuration checks rewrote user settings"
        assert not list(runtime.iterdir()), "read-only commands created runtime state"
        assert not list(state.iterdir()), "read-only commands created persistent state"
    print("configuration CLI defaults, XDG, precedence, limits, provenance and read-only checks passed")


if __name__ == "__main__":
    main()
