#!/usr/bin/env python3
"""Structured same-user session control used by authenticated TLS and SSH entries."""
import configparser
import json
import hashlib
import io
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import tomllib
from urllib.parse import urlparse
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
        raise ValueError("This session is not available through the loopback session gateway")
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
    # Absent client preferences must not override the Linux config file.
    args = []
    for key, low, high in [("fps", 1, 120), ("bitrate", 1000, 200000)]:
        if key in request:
            value = request[key]
            if type(value) is not int or not low <= value <= high:
                raise ValueError(f"{key} must be {low}–{high}")
            args += ["--" + key, str(value)]
    if "codec" in request:
        codec = request["codec"]
        if codec not in ("auto", "png", "h264-nvenc", "hevc-nvenc", "av1-nvenc"):
            raise ValueError("Unsupported codec")
        args += ["--codec", codec]
    if "gpu_input" in request:
        if request["gpu_input"] not in ("cpu", "cuda"):
            raise ValueError("Invalid GPU input mode")
        args += ["--gpu-input", request["gpu_input"]]
    if "hdr" in request:
        if type(request["hdr"]) is not bool:
            raise ValueError("hdr must be a boolean")
        args += ["--hdr", "experimental-scrgb" if request["hdr"] else "off"]
    return args


def config_arguments(request):
    if "config" not in request:
        return []
    value = request["config"]
    if not isinstance(value, str) or not value or "\0" in value or len(value) > 4096:
        raise ValueError("Invalid config path")
    return ["--config", value]


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
    existing = list_sessions()
    if existing:
        if mode == "auto":
            return {**info(existing[0]["name"], True), "created": False}
        raise ValueError("A Waybridge session already exists. Reconnect to it, or end it before creating another session.")
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
    token = directory / (session + ".token")
    args = [binary, *config_arguments(request), "--socket", session, "--listen", f"127.0.0.1:{port}",
            "--token-file", str(token), *settings(request)]
    if request.get("audio") is False:
        args += ["--no-audio"]
    if request.get("xwayland") is False:
        args += ["--no-xwayland"]
    if request.get("udp", False):
        args += ["--udp-listen", f"0.0.0.0:{port}"]
    # The Rust resolver validates file + explicit request + limits once using
    # exactly the launch argv. Reconnect returns above without reading config.
    report = subprocess.run([*args, "--print-config"], capture_output=True, text=True, timeout=15)
    if report.returncode:
        raise ValueError("Invalid session configuration: " + report.stderr.strip()[-1536:])
    configuration = json.loads(report.stdout)
    configuration.get("values", {}).pop("service", None)
    configuration["sources"] = {key: value for key, value in configuration.get("sources", {}).items() if not key.startswith("service.")}
    for field in ("fps", "bitrate", "codec", "gpu_input", "hdr"):
        if field in request:
            configuration["sources"]["session." + field] = {"kind": "client_request"}
    for field in ("socket", "listen"):
        configuration["sources"]["session." + field] = {"kind": "session_manager"}
    configuration.update(scope="session_startup_request", persisted=False)
    check = subprocess.run([*args, "--check-session"], capture_output=True, text=True, timeout=15)
    if check.returncode:
        raise ValueError("Cannot check session availability: " + check.stderr.strip()[-1536:])
    availability = json.loads(check.stdout)
    if not availability.get("available"):
        raise ValueError(availability.get("error", "Cannot create a new Waybridge session"))
    with owned_log(session) as log:
        # Detached session/process group and stdio survive entry-point disconnect.
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
            return {**result, "created": True, "configuration": configuration}
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


def xdg_directory(key, fallback, environment=None):
    value = (os.environ if environment is None else environment).get(key)
    return Path(value) if value and Path(value).is_absolute() else Path(fallback)


def app_catalog(environment=None):
    environment = os.environ if environment is None else environment
    home = Path(environment.get("HOME", str(Path.home())))
    directories = [xdg_directory("XDG_DATA_HOME", home / ".local/share", environment)]
    directories += [Path(p) for p in (environment.get("XDG_DATA_DIRS") or "/usr/local/share:/usr/share").split(":") if Path(p).is_absolute()]
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




# The worker is embedded alongside the RPC helper: both TLS include_str! and SSH
# execute this file's text, so no separately installed Python module is needed.
GIO_WORKER = r'''
import ctypes as C, ctypes.util, json, os, sys
plan = json.load(sys.stdin)
gio = C.CDLL(C.util.find_library("gio-2.0") or "libgio-2.0.so.0")
glib = C.CDLL(C.util.find_library("glib-2.0") or "libglib-2.0.so.0")
class Error(C.Structure):
    _fields_ = [("domain", C.c_uint), ("code", C.c_int), ("message", C.c_char_p)]
error = C.POINTER(Error)()
def bind(lib, name, restype, *argtypes):
    function = getattr(lib, name)
    function.restype, function.argtypes = restype, list(argtypes)
    return function
new = bind(gio, "g_desktop_app_info_new_from_filename", C.c_void_p, C.c_char_p)
app = new(os.fsencode(plan["entry"]))
if not app:
    raise SystemExit("GIO rejected the effective Desktop Entry")
get_id = bind(gio, "g_app_info_get_id", C.c_char_p, C.c_void_p)
actual_id = get_id(app)
if not actual_id or actual_id.decode() != plan["desktop_id"]:
    raise SystemExit("GIO desktop identity differs from the original desktop ID")
context = bind(gio, "g_app_launch_context_new", C.c_void_p)()
setenv = bind(gio, "g_app_launch_context_setenv", None, C.c_void_p, C.c_char_p, C.c_char_p)
unsetenv = bind(gio, "g_app_launch_context_unsetenv", None, C.c_void_p, C.c_char_p)
for key, value in plan["environment_set"].items():
    setenv(context, key.encode(), value.encode())
for key in plan["environment_unset"]:
    unsetenv(context, key.encode())
if plan["dbus_action"]:
    launch_action = bind(gio, "g_desktop_app_info_launch_action", None, C.c_void_p, C.c_char_p, C.c_void_p)
    launch_action(app, plan["action"].encode(), context)
    # launch_action is void/asynchronous; flushing sends the request but does not
    # certify activation or a window. The caller reports activation-requested.
    bus_get = bind(gio, "g_bus_get_sync", C.c_void_p, C.c_int, C.c_void_p, C.POINTER(C.POINTER(Error)))
    connection = bus_get(2, None, C.byref(error))
    if connection:
        flush = bind(gio, "g_dbus_connection_flush_sync", C.c_int, C.c_void_p, C.c_void_p, C.POINTER(C.POINTER(Error)))
        if not flush(connection, None, C.byref(error)):
            raise SystemExit(error.contents.message.decode() if error else "D-Bus flush failed")
    elif error:
        raise SystemExit(error.contents.message.decode())
else:
    append = bind(glib, "g_list_append", C.c_void_p, C.c_void_p, C.c_void_p)
    values = [C.create_string_buffer(uri.encode()) for uri in plan["uris"]]
    uris = None
    for value in values:
        uris = append(uris, C.cast(value, C.c_void_p))
    launch = bind(gio, "g_app_info_launch_uris", C.c_int, C.c_void_p, C.c_void_p, C.c_void_p, C.POINTER(C.POINTER(Error)))
    if not launch(app, uris, context, C.byref(error)):
        raise SystemExit(error.contents.message.decode() if error else "GIO launch failed")
'''


def desktop_config(path):
    config = configparser.ConfigParser(interpolation=None, strict=False)
    config.optionxform = str
    with Path(path).open(encoding="utf-8") as stream:
        config.read_file(stream)
    return config


def text_value(value, field, limit=8192, nonempty=False):
    if not isinstance(value, str) or "\0" in value or len(value) > limit or (nonempty and not value):
        raise ValueError(f"Invalid {field}")
    return value


def string_array(value, field, limit=128):
    if not isinstance(value, list) or len(value) > limit:
        raise ValueError(f"{field} must be an array with at most {limit} strings")
    return [text_value(item, field) for item in value]


def environment_override(table):
    values = table.get("env", {})
    if not isinstance(values, dict):
        raise ValueError("env must be a string table")
    unset = string_array(table.get("unset_env", []), "unset_env")
    for key in [*values, *unset]:
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key):
            raise ValueError("Invalid environment variable name")
    if set(values) & set(unset):
        raise ValueError("env and unset_env cannot contain the same variable")
    if len(values) > 128:
        raise ValueError("Too many environment variables")
    return {key: text_value(value, "environment value") for key, value in values.items()}, unset


def override_table(table):
    fields = {"exec", "extra_argv", "cwd", "env", "unset_env", "launch_mode", "name", "icon", "terminal", "clear"}
    if not isinstance(table, dict):
        raise ValueError("Desktop override must be a table")
    if table.keys() - fields:
        raise ValueError("Unknown desktop override fields: " + ", ".join(sorted(table.keys() - fields)))
    for key in ("exec", "cwd", "name", "icon"):
        if key in table:
            text_value(table[key], key, nonempty=key in ("exec", "cwd", "name"))
            if "\n" in table[key] or "\r" in table[key]:
                raise ValueError(f"{key} must fit one Desktop Entry line")
    if "cwd" in table and not Path(table["cwd"]).is_absolute():
        raise ValueError("cwd must be an absolute directory; no shell or tilde expansion")
    if "terminal" in table and type(table["terminal"]) is not bool:
        raise ValueError("terminal must be a boolean")
    if table.get("launch_mode", "desktop") not in ("desktop", "exec"):
        raise ValueError("launch_mode must be desktop or exec")
    string_array(table.get("extra_argv", []), "extra_argv")
    clear = string_array(table.get("clear", []), "clear")
    if set(clear) - {"exec", "cwd", "icon"} or set(clear) & table.keys():
        raise ValueError("clear accepts exec/cwd/icon and cannot also replace that field")
    environment_override(table)
    return table


def desktop_override(app_id, config_home):
    # app_id comes from the installed catalog, never from an arbitrary path.
    path = Path(config_home) / "waybridge/desktop-overrides" / (app_id + ".toml")
    if not path.exists():
        return {}, None
    if path.stat().st_size > 65536:
        raise ValueError("Desktop override exceeds size limit")
    with path.open("rb") as stream:
        config = tomllib.load(stream)
    if config.keys() - {"version", "default", "actions"} or type(config.get("version")) is not int or config["version"] != 1:
        raise ValueError("Desktop override requires version=1 and only default/actions tables")
    override_table(config.get("default", {}))
    actions = config.get("actions", {})
    if not isinstance(actions, dict):
        raise ValueError("actions must be a table")
    for action, table in actions.items():
        if not re.fullmatch(r"[A-Za-z0-9-]+", action):
            raise ValueError("Invalid desktop action ID")
        override_table(table)
    return config, str(path)


def desktop_unescape(value):
    # Desktop Entry string escaping precedes Exec argument parsing.
    mapping = {"s": " ", "n": "\n", "t": "\t", "r": "\r", "\\": "\\"}
    result, index = [], 0
    while index < len(value):
        char = value[index]
        if char == "\\":
            index += 1
            if index == len(value) or value[index] not in mapping:
                raise ValueError("Invalid Desktop Entry string escape")
            char = mapping[value[index]]
        result.append(char)
        index += 1
    return "".join(result)


def desktop_escape(value):
    return value.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t").replace("\r", "\\r")


def exec_literal(value, wrap=True):
    # Percent is expanded by GIO after Exec quoting: double it even in quotes.
    value = value.replace("%", "%%")
    escaped = "".join("\\" + char if char in '\\"`$' else char for char in value)
    return '"' + escaped + '"' if wrap else escaped


def effective_exec(value, additions, source):
    command = desktop_unescape(value)
    if not command.strip():
        raise ValueError("Exec cannot be empty")
    # Find only the executable token boundary. Leave argument parsing and all
    # field expansions to GIO; this scanner never evaluates shell syntax.
    index, quoted = 0, False
    while index < len(command) and command[index] in " \t\n\r\v\f":
        index += 1
    while index < len(command):
        char = command[index]
        if char == "\\":
            index += 2
            continue
        if char == '"':
            quoted = not quoted
        if char in " \t\n\r\v\f" and not quoted:
            break
        index += 1
    if quoted:
        raise ValueError("Unterminated executable quote in Desktop Entry")
    if additions:
        command = command[:index] + " " + " ".join(exec_literal(v) for v in additions) + command[index:]
    # Derived files retain %k's *original* installed filename. Preserve %%k as
    # a literal and account for valid quoted/unquoted field placement.
    result, index, quoted = [], 0, False
    while index < len(command):
        char = command[index]
        if char == "\\" and index + 1 < len(command):
            result.extend(command[index:index + 2])
            index += 2
            continue
        if char == '"':
            quoted = not quoted
        if char == "%" and index + 1 < len(command):
            field = command[index + 1]
            if field == "%":
                result.append("%%")
                index += 2
                continue
            if field == "k":
                result.append(exec_literal(source, not quoted))
                index += 2
                continue
        result.append(char)
        index += 1
    return desktop_escape("".join(result))


def session_environment(saved):
    environment = dict(os.environ)
    for key, value in saved.get("environment", {}).items():
        if value is None:
            environment.pop(key, None)
        else:
            environment[key] = value
    return environment


def launch_plan(request, saved):
    environment = session_environment(saved)
    home = environment.get("HOME", str(Path.home()))
    if request.get("app"):
        if request.get("argv") is not None or any(key in request for key in ("cwd", "env", "unset_env")):
            raise ValueError("Desktop launches use their scoped override for cwd/env; argv is a separate launch mode")
        app = next((app for app in app_catalog(environment) if app["id"] == request["app"]), None)
        if not app:
            raise ValueError("Application is not in the installed desktop catalog")
        config = desktop_config(app["path"])
        entry = config["Desktop Entry"]
        action = request.get("action")
        if action is not None:
            text_value(action, "action", 256, nonempty=True)
            if action not in entry.get("Actions", "").split(";") or "Desktop Action " + action not in config:
                raise ValueError("Unknown desktop action")
        overlay, overlay_path = desktop_override(app["id"], xdg_directory("XDG_CONFIG_HOME", Path(home) / ".config", environment))
        unknown_actions = set(overlay.get("actions", {})) - set(entry.get("Actions", "").split(";"))
        if unknown_actions:
            raise ValueError("Override contains unknown desktop actions: " + ", ".join(sorted(unknown_actions)))
        table = overlay.get("actions", {}).get(action, {}) if action else overlay.get("default", {})
        selected = config["Desktop Action " + action] if action else entry
        changes = table.get("clear", [])
        for field, key in [("exec", "Exec"), ("name", "Name"), ("icon", "Icon")]:
            if field in table:
                # Exec is already Desktop Entry syntax; metadata is plain text.
                selected[key] = table[field] if field == "exec" else desktop_escape(table[field]).replace(" ", "\\s")
                if field in ("name", "icon"):
                    for localized in list(selected):
                        if localized.startswith(key + "["):
                            del selected[localized]
            elif field in changes:
                selected.pop(key, None)
                for localized in list(selected):
                    if localized.startswith(key + "["):
                        del selected[localized]
        if "cwd" in table:
            entry["Path"] = desktop_escape(table["cwd"])
        elif "cwd" in changes:
            entry.pop("Path", None)
        if "terminal" in table:
            entry["Terminal"] = str(table["terminal"]).lower()
        updates, unset = environment_override(table)
        process_change = bool(set(table) & {"exec", "cwd", "terminal"} or table.get("extra_argv") or updates or unset or "cwd" in changes)
        dbus = entry.getboolean("DBusActivatable", fallback=False)
        mode = table.get("launch_mode", "desktop")
        if dbus and process_change and mode != "exec":
            raise ValueError("DBusActivatable conflicts with process overrides; explicitly set launch_mode=\"exec\"")
        if mode == "exec":
            entry["DBusActivatable"] = "false"
            dbus = False
        if not dbus and not selected.get("Exec"):
            raise ValueError("Exec launch requires an effective Exec field")
        cwd = desktop_unescape(entry["Path"]) if entry.get("Path") else home
        text_value(cwd, "effective cwd", nonempty=True)
        if not Path(cwd).is_absolute() or not Path(cwd).is_dir():
            raise ValueError("Effective working directory must be an existing absolute directory")
        uris = string_array(request.get("uris", []), "uris")
        if action and uris:
            raise ValueError("Desktop actions do not accept file/URI arguments in the GIO action contract")
        for uri in uris:
            if not urlparse(uri).scheme:
                raise ValueError("uris requires absolute URI values; use file:/// for files")
        # Action Exec launch uses the same GIO field expansion as standard
        # actions (main Name/Icon/Terminal/Path), with an error-reporting API.
        derived = bool(table) or bool(action and not dbus) or Path(app["path"]).name != app["id"]
        if derived:
            if selected.get("Exec"):
                selected["Exec"] = effective_exec(selected["Exec"], table.get("extra_argv", []), app["path"])
            if action and not dbus:
                entry["Exec"] = selected["Exec"]
            entry["X-Waybridge-Original-Desktop-Id"] = desktop_escape(app["id"])
            entry["X-Waybridge-Original-Desktop-File"] = desktop_escape(app["path"])
            if overlay_path:
                entry["X-Waybridge-Override-File"] = desktop_escape(overlay_path)
            if action:
                entry["X-Waybridge-Action"] = desktop_escape(action)
        plan = dict(kind="desktop", desktop_id=app["id"], source=app["path"], override=overlay_path,
                    override_section="actions." + action if action else "default", action=action,
                    launch_mode=mode, activation="dbus" if dbus else "exec", cwd=cwd,
                    cwd_source="override" if "cwd" in table or "cwd" in changes else "desktop" if entry.get("Path") else "HOME",
                    exec=selected.get("Exec"), name=desktop_unescape(selected.get("Name", "")),
                    icon=desktop_unescape(selected.get("Icon", "")), terminal=entry.getboolean("Terminal", fallback=False),
                    extra_argv=table.get("extra_argv", []), env_keys=sorted(updates), unset_env=unset,
                    uris=uris, derived=derived, dbus_action=bool(action and dbus),
                    process_options_applied=not dbus, readiness="not-observed")
        plan["field_sources"] = {field: "override:" + plan["override_section"] if field in table or field in changes else "desktop"
                                 for field in ("exec", "name", "icon", "terminal")}
        plan["field_sources"]["cwd"] = plan["cwd_source"]
        environment.update(updates)
        for key in unset:
            environment.pop(key, None)
        return plan, environment, config
    if request.get("action") is not None or "uris" in request:
        raise ValueError("action/uris require an installed desktop application")
    argv = string_array(request.get("argv"), "argv")
    if not argv or not argv[0]:
        raise ValueError("Launch requires a nonempty argument array")
    updates, unset = environment_override(request)
    cwd = text_value(request.get("cwd", home), "cwd", nonempty=True)
    if not Path(cwd).is_absolute() or not Path(cwd).is_dir():
        raise ValueError("cwd must be an existing absolute directory")
    environment.update(updates)
    for key in unset:
        environment.pop(key, None)
    return dict(kind="argv", argv=argv, cwd=cwd, cwd_source="request" if "cwd" in request else "HOME",
                env_keys=sorted(updates), unset_env=unset, readiness="not-observed"), environment, None


def launch_application(request, saved, session, diagnostic=False):
    plan, environment, config = launch_plan(request, saved)
    if diagnostic:
        return plan
    with owned_log(session) as log:
        if plan["kind"] == "argv":
            process = subprocess.Popen(plan["argv"], env=environment, cwd=plan["cwd"],
                                       stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True, close_fds=True)
            return dict(status="spawned", pid=process.pid, pid_role="spawned-process", readiness="not-observed", plan=plan)
        path = plan["source"]
        if plan["derived"]:
            serialized = io.StringIO()
            config.write(serialized, space_around_delimiters=False)
            content = serialized.getvalue().encode()
            digest = hashlib.sha256(content).hexdigest()
            # GIO_LAUNCHED_DESKTOP_FILE references this path. Keep it for the
            # session lifetime; the compositor removes its launch directory.
            owned_runtime = saved.get("environment", {}).get("WAYBRIDGE_SESSION_RUNTIME")
            if not owned_runtime:
                raise ValueError("Restart this Waybridge session to publish its owned desktop launch directory")
            directory = Path(owned_runtime)
            if not directory.is_absolute():
                raise ValueError("Session runtime directory must be absolute")
            metadata = directory.lstat()
            if not stat.S_ISDIR(metadata.st_mode) or metadata.st_uid != os.getuid() or metadata.st_mode & 0o077:
                raise ValueError("Session runtime directory must be private and owned by this user")
            for part in ("desktop-launches", digest):
                directory /= part
                directory.mkdir(mode=0o700, exist_ok=True)
                metadata = directory.lstat()
                if not stat.S_ISDIR(metadata.st_mode) or metadata.st_uid != os.getuid() or metadata.st_mode & 0o077:
                    raise ValueError("Desktop launch directory must be private and owned by this user")
            path = str(directory / plan["desktop_id"])
            if not Path(path).exists():
                with tempfile.NamedTemporaryFile(dir=directory, delete=False) as stream:
                    temporary = Path(stream.name)
                    stream.write(content)
                os.replace(temporary, path)
            if secure_read(path, 131072) != content:
                raise ValueError("Derived Desktop Entry does not match the effective plan")
        # Application overrides go through GIO's launch context. They must not
        # change the interpreter/dynamic loader that implements the dispatcher.
        work = {**plan, "entry": path,
                "environment_set": {key: environment[key] for key in plan["env_keys"]},
                "environment_unset": plan["unset_env"]}
        process = subprocess.Popen([sys.executable, "-I", "-c", GIO_WORKER], env=session_environment(saved), cwd=plan["cwd"],
                                   stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT,
                                   start_new_session=True, close_fds=True)
        try:
            process.communicate(json.dumps(work).encode(), timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
            raise ValueError("GIO launch request timed out; application readiness is unknown") from None
        if process.returncode:
            raise ValueError(f"GIO launch failed; see {RUNTIME / 'waybridge-control-center' / (session + '.log')}")
    return dict(status="activation-requested" if plan["activation"] == "dbus" else "dispatched",
                readiness="not-observed", plan=plan)


def dispatch(request):
    operation = request.get("op")
    if operation == "hello":
        return dict(version=1, platform="linux", features=["sessions", "launch", "applications", "launch-plan", "desktop-overrides"])
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
    if operation in ("launch", "launch-plan"):
        return launch_application(request, saved, session, diagnostic=operation == "launch-plan")
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
