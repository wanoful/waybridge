#!/usr/bin/env python3
"""Structured, same-user session control, transported over an authenticated SSH channel."""
import configparser
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import stat
import subprocess
import sys
import time
import uuid

RUNTIME = Path(os.environ.get("XDG_RUNTIME_DIR", f"/run/user/{os.getuid()}"))
NAME = re.compile(r"^[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}$")


def name(value):
    if not isinstance(value, str) or not NAME.fullmatch(value):
        raise ValueError("Invalid session name")
    return value


def secure_read(path, limit=32768):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, "rb") as f:
        meta = os.fstat(f.fileno())
        if meta.st_uid != os.getuid() or meta.st_mode & 0o077 or not stat.S_ISREG(meta.st_mode):
            raise ValueError("Session metadata/token must be a private regular file owned by this user")
        data = f.read(limit + 1)
        if len(data) > limit:
            raise ValueError("Session metadata exceeds limit")
        return data


def identity(pid):
    return Path(f"/proc/{int(pid)}/stat").read_text().rsplit(")", 1)[1].split()[19]


def descriptor(session):
    saved = json.loads(secure_read(RUNTIME / (name(session) + ".session.json")))
    if saved["version"] != 1 or identity(saved["pid"]) != str(saved["started"]):
        raise ValueError("Session descriptor is stale")
    connection = saved.get("connection")
    if not connection:
        raise ValueError("Update/restart the Linux server to publish connection information")
    address, port = connection["listen"].rsplit(":", 1)
    if address not in ("127.0.0.1", "localhost", "0.0.0.0", "[::1]", "[::]"):
        raise ValueError("This session is not available through a loopback SSH tunnel")
    if not 1 <= int(port) <= 65535:
        raise ValueError("Invalid session port")
    return saved


def info(session, auth=False):
    saved = descriptor(session)
    result = dict(name=session, pid=saved["pid"], started=str(saved["started"]),
                  port=int(saved["connection"]["listen"].rsplit(":", 1)[1]))
    if auth:
        token = secure_read(saved["connection"]["token_file"], 4096).decode().strip()
        if not 16 <= len(token) <= 4096 or any(c.isspace() for c in token):
            raise ValueError("Invalid authentication token")
        result["token"] = token
    return result


def resolve_program(value):
    if not isinstance(value, str) or not value or len(value) > 4096 or "\0" in value:
        raise ValueError("Invalid server binary")
    value = str(Path(value).expanduser()) if "/" in value else value
    found = shutil.which(value)
    if not found:
        raise ValueError("Waybridge server not found; set the installed server binary path")
    return found


def list_sessions():
    result = []
    for path in RUNTIME.glob("*.session.json"):
        session = path.name[:-len(".session.json")]
        try:
            result.append(info(session))
        except (ValueError, OSError, KeyError, TypeError):
            continue
    return sorted(result, key=lambda v: v["name"])


def settings(request):
    fps, bitrate = request.get("fps", 60), request.get("bitrate", 30000)
    codec = request.get("codec", "auto")
    if type(fps) is not int or not 1 <= fps <= 120:
        raise ValueError("Frame rate must be 1–120")
    if type(bitrate) is not int or not 1000 <= bitrate <= 200000:
        raise ValueError("Bitrate must be 1000–200000 kbps per window")
    if codec not in ("auto", "png", "h264-nvenc", "hevc-nvenc", "av1-nvenc"):
        raise ValueError("Unsupported codec")
    return ["--fps", str(fps), "--bitrate", str(bitrate), "--codec", codec]


def owned_log(session):
    directory = RUNTIME / "waybridge-control-center"
    directory.mkdir(mode=0o700, exist_ok=True)
    metadata = directory.lstat()
    if not stat.S_ISDIR(metadata.st_mode) or metadata.st_uid != os.getuid() or metadata.st_mode & 0o077:
        raise ValueError("Session control directory must be private and owned by this user")
    path = directory / (name(session) + ".log")
    fd = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
    meta = os.fstat(fd)
    if not stat.S_ISREG(meta.st_mode) or meta.st_uid != os.getuid() or meta.st_mode & 0o077:
        os.close(fd)
        raise ValueError("Invalid session log")
    return os.fdopen(fd, "ab", buffering=0)


def ensure(request):
    session = name(request.get("session", "waybridge-0"))
    mode = request.get("mode", "auto")
    if mode not in ("auto", "attach", "new"):
        raise ValueError("Invalid session policy")
    if mode != "new":
        try:
            return {**info(session, True), "created": False}
        except FileNotFoundError:
            if mode == "attach":
                raise ValueError("Requested session is not running")
        except (ValueError, ProcessLookupError) as error:
            # Never overwrite an existing/stale descriptor and guess ownership.
            raise ValueError(str(error)) from error
    else:
        session = name(session[:48] + "-" + uuid.uuid4().hex[:8])
    binary = resolve_program(request.get("binary", "waybridge"))
    port = request.get("port", 0)
    if type(port) is not int or not 0 <= port <= 65535:
        raise ValueError("Invalid listen port")
    if mode == "new":
        port = 0
    if not port:
        with socket.socket() as free:
            free.bind(("127.0.0.1", 0))
            port = free.getsockname()[1]
    directory = RUNTIME / "waybridge-control-center"
    with owned_log(session) as log:
        token = directory / (session + ".token")
        args = [binary, "--socket", session, "--listen", f"127.0.0.1:{port}",
                "--token-file", str(token), *settings(request)]
        if request.get("gpu_input", "cpu") not in ("cpu", "cuda"):
            raise ValueError("Invalid GPU input mode")
        args += ["--gpu-input", request.get("gpu_input", "cpu")]
        if request.get("hdr", False):
            args += ["--hdr", "experimental-scrgb"]
        if request.get("audio") is False:
            args += ["--no-audio"]
        if request.get("xwayland") is False:
            args += ["--no-xwayland"]
        if request.get("udp", False):
            args += ["--udp-listen", f"0.0.0.0:{port}"]
        # New session/process group and detached stdio survive SSH disconnect.
        # Do not adopt or restart any preexisting service.
        process = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=log,
                                   stderr=subprocess.STDOUT, start_new_session=True, close_fds=True)
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise ValueError(f"Server startup failed; see {directory / (session + '.log')}")
        try:
            result = info(session, True)
            if result["pid"] != process.pid:
                raise ValueError("Another process owns the requested session")
            return {**result, "created": True}
        except FileNotFoundError:
            time.sleep(.05)
    # Only the child created here may be terminated on startup failure.
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()
    raise ValueError("Server did not become ready")


def app_catalog():
    directories = [Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share")))]
    directories += [Path(p) for p in os.environ.get("XDG_DATA_DIRS", "/usr/local/share:/usr/share").split(":") if p]
    apps, seen = [], set()
    for directory in directories:
        if len(apps) >= 1024:
            break
        directory /= "applications"
        if not directory.is_dir():
            continue
        for path in sorted(directory.rglob("*.desktop")):
            app_id = str(path.relative_to(directory)).replace("/", "-")
            if app_id in seen:
                continue
            seen.add(app_id)
            try:
                if path.stat().st_size > 65536:
                    continue
                config = configparser.ConfigParser(interpolation=None, strict=False)
                config.read(path, encoding="utf-8")
                entry = config["Desktop Entry"]
                if entry.get("Type") != "Application" or entry.getboolean("Hidden", fallback=False) or entry.getboolean("NoDisplay", fallback=False):
                    continue
                if entry.get("TryExec") and not shutil.which(entry["TryExec"]):
                    continue
                if not entry.get("Exec") and not entry.getboolean("DBusActivatable", fallback=False):
                    continue
                apps.append(dict(id=app_id, name=entry.get("Name", app_id)[:256], icon=entry.get("Icon", "")[:512], path=str(path)))
            except (OSError, UnicodeError, KeyError, ValueError, configparser.Error):
                continue
            if len(apps) >= 1024:
                break
    return sorted(apps, key=lambda item: item["name"].casefold())


def dispatch(request):
    operation = request.get("op")
    if operation == "hello":
        return dict(version=1, platform="linux", features=["sessions", "launch", "applications"])
    if operation == "list":
        return list_sessions()
    if operation == "ensure":
        return ensure(request)
    if operation == "applications":
        return [{k: v for k, v in app.items() if k != "path"} for app in app_catalog()]
    session = name(request.get("session", "waybridge-0"))
    saved = descriptor(session)
    if operation == "info":
        return info(session)
    if operation == "launch":
        binary = resolve_program(request.get("binary", "waybridge"))
        if request.get("app"):
            app = next((app for app in app_catalog() if app["id"] == request["app"]), None)
            if not app:
                raise ValueError("Application is not in the installed desktop catalog")
            if not shutil.which("gio"):
                raise ValueError("gio is required to launch desktop applications")
            argv = ["gio", "launch", app["path"]]
        else:
            argv = request.get("argv")
            if not isinstance(argv, list) or not 1 <= len(argv) <= 128 or not all(
                    isinstance(v, str) and "\0" not in v and len(v) <= 8192 for v in argv) or not argv[0]:
                raise ValueError("Launch requires an argument array or installed desktop application")
        with owned_log(session) as log:
            process = subprocess.Popen([binary, "--socket", session, "--launch", "--", *argv],
                                       stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True, close_fds=True)
        return dict(pid=process.pid)
    if operation == "end":
        # pidfd pins process identity across the /proc check and signal.
        fd = os.pidfd_open(saved["pid"])
        try:
            if identity(saved["pid"]) != str(saved["started"]):
                raise ValueError("Server process identity changed")
            signal.pidfd_send_signal(fd, signal.SIGTERM)
        finally:
            os.close(fd)
        return dict(stopping=True)
    raise ValueError("Unknown operation")


def main():
    for line in iter(lambda: sys.stdin.buffer.readline(65538), b""):
        request_id = None
        try:
            if len(line) > 65536 or not line.endswith(b"\n"):
                raise ValueError("Control request exceeds limit")
            request = json.loads(line)
            if not isinstance(request, dict):
                raise ValueError("Control request must be an object")
            request_id = request.get("id")
            result = dispatch(request)
            reply = dict(id=request_id, ok=True, result=result)
        except (ValueError, OSError, KeyError, TypeError, subprocess.SubprocessError) as error:
            reply = dict(id=request_id, ok=False, error=str(error)[:2048])
        sys.stdout.write(json.dumps(reply, ensure_ascii=False, separators=(",", ":")) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
