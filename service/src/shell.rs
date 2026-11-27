use anyhow::{bail, Context, Result};
use serde::Deserialize;
use serde_json::{json, Value};
use std::{
    collections::{BTreeMap, VecDeque},
    ffi::CStr,
    fs::{self, OpenOptions},
    io::{self, Read},
    os::{
        fd::{AsRawFd, FromRawFd, OwnedFd, RawFd},
        unix::{
            fs::{MetadataExt, OpenOptionsExt, PermissionsExt},
            process::CommandExt,
        },
    },
    path::{Path, PathBuf},
    sync::atomic::{AtomicU64, Ordering},
    time::Duration,
};
use tokio::{
    io::{unix::AsyncFd, AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt},
    process::Command,
    sync::watch,
    time::timeout,
};

pub const KIND_DATA: u8 = 1;
pub const KIND_RESIZE: u8 = 2;
pub const KIND_EXIT: u8 = 3;
pub const KIND_PING: u8 = 4;
pub const KIND_PONG: u8 = 5;

pub const REASON_EXITED: u8 = 0;
pub const REASON_SESSION_ENDED: u8 = 1;
pub const REASON_REVOKED: u8 = 2;

const MAX_FRAME_BYTES: usize = 65536;
const MAX_PENDING_STDIN: usize = 256 * 1024;
const MIN_COLS: u16 = 1;
const MAX_COLS: u16 = 1000;
const MIN_ROWS: u16 = 1;
const MAX_ROWS: u16 = 500;

static SHELL_COUNTER: AtomicU64 = AtomicU64::new(1);

#[derive(Deserialize)]
struct SavedSession {
    version: u8,
    pid: u32,
    started: String,
    environment: BTreeMap<String, Option<String>>,
}

struct TempDirGuard {
    path: PathBuf,
}

impl TempDirGuard {
    fn new(base: &Path) -> Result<Self> {
        let root = base.join("shells");
        fs::create_dir_all(&root)?;
        fs::set_permissions(&root, fs::Permissions::from_mode(0o700))?;
        let id = SHELL_COUNTER.fetch_add(1, Ordering::Relaxed);
        let path = root.join(format!("shell-{}-{id}", std::process::id()));
        fs::create_dir(&path)?;
        fs::set_permissions(&path, fs::Permissions::from_mode(0o700))?;
        Ok(Self { path })
    }
}

impl Drop for TempDirGuard {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

struct SessionTarget {
    environment: BTreeMap<String, Option<String>>,
    pidfd: OwnedFd,
}

fn valid_session_name(value: &str) -> bool {
    let bytes = value.as_bytes();
    !bytes.is_empty()
        && bytes.len() <= 64
        && bytes[0].is_ascii_alphanumeric()
        && bytes
            .iter()
            .all(|byte| byte.is_ascii_alphanumeric() || *byte == b'_' || *byte == b'-')
}

fn valid_env_key(key: &str) -> bool {
    let bytes = key.as_bytes();
    !bytes.is_empty()
        && (bytes[0].is_ascii_alphabetic() || bytes[0] == b'_')
        && bytes
            .iter()
            .all(|byte| byte.is_ascii_alphanumeric() || *byte == b'_')
}

fn valid_term(term: &str) -> bool {
    !term.is_empty()
        && term.len() <= 128
        && term
            .bytes()
            .all(|byte| byte.is_ascii_graphic() && byte != b'\'' && byte != b'"')
}

fn proc_started(pid: u32) -> Result<String> {
    let stat = fs::read_to_string(format!("/proc/{pid}/stat"))?;
    stat.rsplit_once(')')
        .and_then(|(_, tail)| tail.split_whitespace().nth(19))
        .map(str::to_owned)
        .context("reading session process start time")
}

fn open_pidfd(pid: u32) -> Result<OwnedFd> {
    let raw = unsafe { libc::syscall(libc::SYS_pidfd_open, pid as libc::c_int, 0 as libc::c_uint) };
    if raw < 0 {
        return Err(io::Error::last_os_error()).context("opening session pidfd");
    }
    Ok(unsafe { OwnedFd::from_raw_fd(raw as RawFd) })
}

fn load_session(session: &str) -> Result<SessionTarget> {
    if !valid_session_name(session) {
        bail!("Invalid session name");
    }
    let runtime = std::env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(format!("/run/user/{}", unsafe { libc::getuid() })));
    let path = runtime.join(format!("{session}.session.json"));
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW)
        .open(&path)
        .context("Session is not running")?;
    let metadata = file.metadata()?;
    if !metadata.is_file()
        || metadata.uid() != unsafe { libc::getuid() }
        || metadata.permissions().mode() & 0o077 != 0
    {
        bail!("Invalid session descriptor");
    }
    let mut bytes = Vec::new();
    file.take(32769).read_to_end(&mut bytes)?;
    if bytes.len() > 32768 {
        bail!("Session descriptor exceeds limit");
    }
    let saved: SavedSession =
        serde_json::from_slice(&bytes).context("Invalid session descriptor")?;
    if saved.version != 1 {
        bail!("Unsupported session descriptor version");
    }
    let pidfd = open_pidfd(saved.pid).context("Session compositor is no longer running")?;
    if proc_started(saved.pid).ok().as_ref() != Some(&saved.started) {
        bail!("Session descriptor is stale");
    }
    Ok(SessionTarget {
        environment: saved.environment,
        pidfd,
    })
}

fn resolve_user_shell() -> Result<PathBuf> {
    if let Some(shell) = std::env::var_os("SHELL") {
        let path = PathBuf::from(shell);
        if is_executable(&path) {
            return Ok(path);
        }
    }
    let uid = unsafe { libc::getuid() };
    let mut pwd = std::mem::MaybeUninit::<libc::passwd>::zeroed();
    let mut buffer = vec![0u8; 4096];
    let mut result = std::ptr::null_mut();
    let rc = unsafe {
        libc::getpwuid_r(
            uid,
            pwd.as_mut_ptr(),
            buffer.as_mut_ptr() as *mut libc::c_char,
            buffer.len(),
            &mut result,
        )
    };
    if rc == 0 && !result.is_null() {
        let shell_ptr = unsafe { (*result).pw_shell };
        if !shell_ptr.is_null() {
            if let Ok(text) = unsafe { CStr::from_ptr(shell_ptr) }.to_str() {
                let path = PathBuf::from(text);
                if is_executable(&path) {
                    return Ok(path);
                }
            }
        }
    }
    let fallback = PathBuf::from("/bin/sh");
    if is_executable(&fallback) {
        return Ok(fallback);
    }
    bail!("No usable shell found")
}

fn is_executable(path: &Path) -> bool {
    path.is_absolute()
        && fs::metadata(path)
            .is_ok_and(|meta| meta.is_file() && meta.permissions().mode() & 0o111 != 0)
}

fn resolve_cwd(cwd: Option<&str>) -> Result<PathBuf> {
    let path = match cwd {
        Some(value) if !value.is_empty() => {
            if value.contains('\0') || value.len() > 4096 {
                bail!("Invalid working directory");
            }
            PathBuf::from(value)
        }
        _ => std::env::var_os("HOME")
            .map(PathBuf::from)
            .context("HOME is not set")?,
    };
    if !path.is_absolute() || !path.is_dir() {
        bail!("Working directory must be an existing absolute directory");
    }
    Ok(path)
}

fn posix_quote(value: &str) -> Result<String> {
    if value.contains('\0') {
        bail!("Environment value contains NUL byte");
    }
    let mut quoted = String::with_capacity(value.len() + 2);
    quoted.push('\'');
    for ch in value.chars() {
        if ch == '\'' {
            quoted.push_str("'\\''");
        } else {
            quoted.push(ch);
        }
    }
    quoted.push('\'');
    Ok(quoted)
}

fn fish_quote(value: &str) -> Result<String> {
    if value.contains('\0') {
        bail!("Environment value contains NUL byte");
    }
    let mut quoted = String::with_capacity(value.len() + 2);
    quoted.push('\'');
    for ch in value.chars() {
        match ch {
            '\\' => quoted.push_str("\\\\"),
            '\'' => quoted.push_str("\\'"),
            _ => quoted.push(ch),
        }
    }
    quoted.push('\'');
    Ok(quoted)
}

fn build_posix_env_block(
    session: &str,
    environment: &BTreeMap<String, Option<String>>,
) -> Result<String> {
    let mut script = String::new();
    for (key, value) in environment {
        if !valid_env_key(key) {
            continue;
        }
        match value {
            Some(val) => {
                script.push_str("export ");
                script.push_str(key);
                script.push('=');
                script.push_str(&posix_quote(val)?);
                script.push('\n');
            }
            None => {
                script.push_str("unset ");
                script.push_str(key);
                script.push('\n');
            }
        }
    }
    script.push_str("export WAYBRIDGE_SHELL='1'\n");
    script.push_str("export WAYBRIDGE_SESSION=");
    script.push_str(&posix_quote(session)?);
    script.push('\n');
    script.push_str("_wb_rc=\"${XDG_CONFIG_HOME:-$HOME/.config}/waybridge/shellrc\"\n");
    script.push_str("if [ -r \"$_wb_rc\" ]; then . \"$_wb_rc\"; fi\n");
    script.push_str("unset _wb_rc\n");
    Ok(script)
}

fn build_fish_env_block(
    session: &str,
    environment: &BTreeMap<String, Option<String>>,
) -> Result<String> {
    let mut script = String::new();
    for (key, value) in environment {
        if !valid_env_key(key) {
            continue;
        }
        match value {
            Some(val) => {
                script.push_str("set -gx ");
                script.push_str(key);
                script.push(' ');
                script.push_str(&fish_quote(val)?);
                script.push('\n');
            }
            None => {
                script.push_str("set -e ");
                script.push_str(key);
                script.push('\n');
            }
        }
    }
    script.push_str("set -gx WAYBRIDGE_SHELL '1'\n");
    script.push_str("set -gx WAYBRIDGE_SESSION ");
    script.push_str(&fish_quote(session)?);
    script.push('\n');
    script.push_str("if set -q XDG_CONFIG_HOME; and test -n \"$XDG_CONFIG_HOME\"\n");
    script.push_str("    set -l _wb_rc \"$XDG_CONFIG_HOME/waybridge/shellrc.fish\"\n");
    script.push_str("else\n");
    script.push_str("    set -l _wb_rc \"$HOME/.config/waybridge/shellrc.fish\"\n");
    script.push_str("end\n");
    script.push_str("if test -r \"$_wb_rc\"\n");
    script.push_str("    source \"$_wb_rc\"\n");
    script.push_str("end\n");
    Ok(script)
}

fn write_private_file(path: &Path, content: &str) -> Result<()> {
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(path)?;
    io::Write::write_all(&mut file, content.as_bytes())?;
    Ok(())
}

fn configure_shell_command(
    shell: &Path,
    session: &str,
    environment: &BTreeMap<String, Option<String>>,
    term: &str,
    cwd: &Path,
    temp_dir: &Path,
) -> Result<Command> {
    let mut command = Command::new(shell);
    command.current_dir(cwd);
    command.env("TERM", term);
    command.env("COLORTERM", "truecolor");
    command.env("WAYBRIDGE_SHELL", "1");
    command.env("WAYBRIDGE_SESSION", session);
    for (key, value) in environment {
        if !valid_env_key(key) {
            continue;
        }
        match value {
            Some(val) => {
                command.env(key, val);
            }
            None => {
                command.env_remove(key);
            }
        }
    }

    let name = shell
        .file_name()
        .and_then(|value| value.to_str())
        .unwrap_or("");
    match name {
        "bash" => {
            let mut rc = String::new();
            rc.push_str("[ -r /etc/profile ] && . /etc/profile\n");
            rc.push_str("if [ -r \"$HOME/.bash_profile\" ]; then\n");
            rc.push_str("  . \"$HOME/.bash_profile\"\n");
            rc.push_str("elif [ -r \"$HOME/.bash_login\" ]; then\n");
            rc.push_str("  . \"$HOME/.bash_login\"\n");
            rc.push_str("else\n");
            rc.push_str("  [ -r \"$HOME/.profile\" ] && . \"$HOME/.profile\"\n");
            rc.push_str("  [ -r /etc/bash.bashrc ] && . /etc/bash.bashrc\n");
            rc.push_str("  [ -r \"$HOME/.bashrc\" ] && . \"$HOME/.bashrc\"\n");
            rc.push_str("fi\n");
            rc.push_str(&build_posix_env_block(session, environment)?);
            let rc_path = temp_dir.join("bashrc");
            write_private_file(&rc_path, &rc)?;
            command.arg("--rcfile").arg(&rc_path).arg("-i");
        }
        "zsh" => {
            let orig_zdotdir = std::env::var_os("ZDOTDIR");
            let temp_str = temp_dir
                .to_str()
                .context("non-UTF-8 runtime directory for zsh ZDOTDIR")?;
            let temp_quoted = posix_quote(temp_str)?;
            let zshenv = format!(
                "_wb_zdir=\"${{_WAYBRIDGE_ORIG_ZDOTDIR:-$HOME}}\"\n\
                 if [ \"${{_WAYBRIDGE_ORIG_ZDOTDIR_SET:-0}}\" = \"1\" ]; then export ZDOTDIR=\"$_WAYBRIDGE_ORIG_ZDOTDIR\"; else unset ZDOTDIR; fi\n\
                 [ -r \"$_wb_zdir/.zshenv\" ] && . \"$_wb_zdir/.zshenv\"\n\
                 export ZDOTDIR={temp_quoted}\n\
                 unset _wb_zdir\n"
            );
            let zprofile = format!(
                "_wb_zdir=\"${{_WAYBRIDGE_ORIG_ZDOTDIR:-$HOME}}\"\n\
                 if [ \"${{_WAYBRIDGE_ORIG_ZDOTDIR_SET:-0}}\" = \"1\" ]; then export ZDOTDIR=\"$_WAYBRIDGE_ORIG_ZDOTDIR\"; else unset ZDOTDIR; fi\n\
                 [ -r \"$_wb_zdir/.zprofile\" ] && . \"$_wb_zdir/.zprofile\"\n\
                 export ZDOTDIR={temp_quoted}\n\
                 unset _wb_zdir\n"
            );
            let mut zshrc = String::from(
                "_wb_zdir=\"${_WAYBRIDGE_ORIG_ZDOTDIR:-$HOME}\"\n\
                 if [ \"${_WAYBRIDGE_ORIG_ZDOTDIR_SET:-0}\" = \"1\" ]; then export ZDOTDIR=\"$_WAYBRIDGE_ORIG_ZDOTDIR\"; else unset ZDOTDIR; fi\n\
                 unset _WAYBRIDGE_ORIG_ZDOTDIR _WAYBRIDGE_ORIG_ZDOTDIR_SET\n\
                 [ -r \"$_wb_zdir/.zshrc\" ] && . \"$_wb_zdir/.zshrc\"\n\
                 unset _wb_zdir\n",
            );
            zshrc.push_str(&build_posix_env_block(session, environment)?);
            write_private_file(&temp_dir.join(".zshenv"), &zshenv)?;
            write_private_file(&temp_dir.join(".zprofile"), &zprofile)?;
            write_private_file(&temp_dir.join(".zshrc"), &zshrc)?;
            if let Some(orig) = orig_zdotdir {
                command.env("_WAYBRIDGE_ORIG_ZDOTDIR", orig);
                command.env("_WAYBRIDGE_ORIG_ZDOTDIR_SET", "1");
            } else {
                command.env_remove("_WAYBRIDGE_ORIG_ZDOTDIR");
                command.env("_WAYBRIDGE_ORIG_ZDOTDIR_SET", "0");
            }
            command.env("ZDOTDIR", temp_dir);
            command.args(["-l", "-i"]);
        }
        "fish" => {
            let init_path = temp_dir.join("init.fish");
            write_private_file(&init_path, &build_fish_env_block(session, environment)?)?;
            let init_str = init_path
                .to_str()
                .context("non-UTF-8 runtime directory for fish init")?;
            command
                .args(["-l", "-i", "-C"])
                .arg(format!("source {}", fish_quote(init_str)?));
        }
        _ => {
            command.arg("-i");
        }
    }
    Ok(command)
}

fn open_pty(cols: u16, rows: u16) -> Result<(OwnedFd, OwnedFd)> {
    let master_raw = unsafe { libc::posix_openpt(libc::O_RDWR | libc::O_NOCTTY | libc::O_CLOEXEC) };
    if master_raw < 0 {
        return Err(io::Error::last_os_error()).context("posix_openpt failed");
    }
    let master = unsafe { OwnedFd::from_raw_fd(master_raw) };
    if unsafe { libc::grantpt(master.as_raw_fd()) } != 0 {
        return Err(io::Error::last_os_error()).context("grantpt failed");
    }
    if unsafe { libc::unlockpt(master.as_raw_fd()) } != 0 {
        return Err(io::Error::last_os_error()).context("unlockpt failed");
    }
    let mut name_buf = [0u8; 256];
    let rc = unsafe {
        libc::ptsname_r(
            master.as_raw_fd(),
            name_buf.as_mut_ptr() as *mut libc::c_char,
            name_buf.len(),
        )
    };
    if rc != 0 {
        return Err(io::Error::from_raw_os_error(rc)).context("ptsname_r failed");
    }
    let slave_path = CStr::from_bytes_until_nul(&name_buf)
        .context("invalid slave pts path")?
        .to_owned();
    let slave_raw = unsafe {
        libc::open(
            slave_path.as_ptr(),
            libc::O_RDWR | libc::O_NOCTTY | libc::O_CLOEXEC,
        )
    };
    if slave_raw < 0 {
        return Err(io::Error::last_os_error()).context("opening slave pty failed");
    }
    let slave = unsafe { OwnedFd::from_raw_fd(slave_raw) };
    set_winsize(master.as_raw_fd(), cols, rows)?;
    let flags = unsafe { libc::fcntl(master.as_raw_fd(), libc::F_GETFL) };
    if flags < 0
        || unsafe { libc::fcntl(master.as_raw_fd(), libc::F_SETFL, flags | libc::O_NONBLOCK) } < 0
    {
        return Err(io::Error::last_os_error()).context("setting nonblocking pty master");
    }
    Ok((master, slave))
}

fn set_winsize(fd: RawFd, cols: u16, rows: u16) -> Result<()> {
    let ws = libc::winsize {
        ws_row: rows.clamp(MIN_ROWS, MAX_ROWS),
        ws_col: cols.clamp(MIN_COLS, MAX_COLS),
        ws_xpixel: 0,
        ws_ypixel: 0,
    };
    if unsafe { libc::ioctl(fd, libc::TIOCSWINSZ, &ws) } != 0 {
        return Err(io::Error::last_os_error()).context("TIOCSWINSZ failed");
    }
    Ok(())
}

async fn write_frame<W: AsyncWrite + Unpin>(
    writer: &mut W,
    kind: u8,
    payload: &[u8],
) -> Result<()> {
    let mut header = [0u8; 5];
    header[0] = kind;
    header[1..5].copy_from_slice(&(payload.len() as u32).to_be_bytes());
    writer.write_all(&header).await?;
    if !payload.is_empty() {
        writer.write_all(payload).await?;
    }
    writer.flush().await?;
    Ok(())
}

async fn write_exit_frame<W: AsyncWrite + Unpin>(
    writer: &mut W,
    exit_code: i32,
    reason: u8,
) -> Result<()> {
    let mut payload = [0u8; 5];
    payload[0..4].copy_from_slice(&exit_code.to_be_bytes());
    payload[4] = reason;
    write_frame(writer, KIND_EXIT, &payload).await
}

struct FrameDecoder {
    buffer: Vec<u8>,
}

impl FrameDecoder {
    fn new() -> Self {
        Self { buffer: Vec::new() }
    }

    fn push(&mut self, bytes: &[u8]) {
        self.buffer.extend_from_slice(bytes);
    }

    fn next_frame(&mut self) -> Result<Option<(u8, Vec<u8>)>> {
        if self.buffer.len() < 5 {
            return Ok(None);
        }
        let kind = self.buffer[0];
        let length = u32::from_be_bytes([
            self.buffer[1],
            self.buffer[2],
            self.buffer[3],
            self.buffer[4],
        ]) as usize;
        if length > MAX_FRAME_BYTES {
            bail!("shell frame exceeds limit");
        }
        if self.buffer.len() < 5 + length {
            return Ok(None);
        }
        let payload = self.buffer[5..5 + length].to_vec();
        self.buffer.drain(..5 + length);
        Ok(Some((kind, payload)))
    }
}

fn try_read_pty(fd: RawFd, buffer: &mut [u8]) -> io::Result<usize> {
    let rc = unsafe { libc::read(fd, buffer.as_mut_ptr() as *mut libc::c_void, buffer.len()) };
    if rc < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(rc as usize)
    }
}

fn try_write_pty(fd: RawFd, data: &[u8]) -> io::Result<usize> {
    let rc = unsafe { libc::write(fd, data.as_ptr() as *const libc::c_void, data.len()) };
    if rc < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(rc as usize)
    }
}

fn terminate_process_group(pid: Option<u32>, signal: libc::c_int) {
    if let Some(pid) = pid {
        let pgid = -(pid as libc::pid_t);
        unsafe {
            libc::kill(pgid, signal);
        }
    }
}

pub async fn serve<S>(
    connection: S,
    id: &Value,
    request: &Value,
    runtime_dir: &Path,
    mut changes: watch::Receiver<u64>,
    is_authorized: impl Fn() -> bool,
) -> Result<()>
where
    S: AsyncRead + AsyncWrite + Unpin,
{
    let mut connection = connection;
    let setup = (|| -> Result<_> {
        let session = request
            .get("session")
            .and_then(Value::as_str)
            .filter(|value| !value.is_empty())
            .context("missing or invalid session")?;
        let target = load_session(session)?;
        let cols = request
            .get("cols")
            .and_then(Value::as_u64)
            .unwrap_or(120)
            .clamp(MIN_COLS as u64, MAX_COLS as u64) as u16;
        let rows = request
            .get("rows")
            .and_then(Value::as_u64)
            .unwrap_or(30)
            .clamp(MIN_ROWS as u64, MAX_ROWS as u64) as u16;
        let term = match request.get("term").and_then(Value::as_str) {
            Some(value) if !value.is_empty() => {
                if !valid_term(value) {
                    bail!("Invalid TERM value");
                }
                value.to_owned()
            }
            _ => "xterm-256color".to_owned(),
        };
        let cwd = resolve_cwd(request.get("cwd").and_then(Value::as_str))?;
        let shell = resolve_user_shell()?;
        let temp_dir = TempDirGuard::new(runtime_dir)?;
        let (master, slave) = open_pty(cols, rows)?;
        let mut command = configure_shell_command(
            &shell,
            session,
            &target.environment,
            &term,
            &cwd,
            &temp_dir.path,
        )?;
        let slave_raw = slave.as_raw_fd();
        unsafe {
            command.as_std_mut().pre_exec(move || {
                if libc::setsid() == -1 {
                    return Err(io::Error::last_os_error());
                }
                if libc::ioctl(slave_raw, libc::TIOCSCTTY as _, 0) == -1 {
                    return Err(io::Error::last_os_error());
                }
                if libc::dup2(slave_raw, 0) == -1
                    || libc::dup2(slave_raw, 1) == -1
                    || libc::dup2(slave_raw, 2) == -1
                {
                    return Err(io::Error::last_os_error());
                }
                libc::signal(libc::SIGPIPE, libc::SIG_DFL);
                Ok(())
            });
        }
        let child = command.spawn().context("spawning user shell")?;
        drop(slave);
        Ok((master, target.pidfd, child, temp_dir))
    })();

    let (master, pidfd, mut child, _temp_dir) = match setup {
        Ok(values) => values,
        Err(error) => {
            let response = json!({
                "id": id,
                "ok": false,
                "error": {"code": "operation_failed", "message": error.to_string()}
            });
            let mut bytes = serde_json::to_vec(&response)?;
            bytes.push(b'\n');
            let _ = timeout(Duration::from_secs(10), async {
                connection.write_all(&bytes).await?;
                connection.flush().await
            })
            .await;
            return Ok(());
        }
    };

    if !is_authorized() {
        terminate_process_group(child.id(), libc::SIGHUP);
        let _ = child.wait().await;
        let response = json!({
            "id": id,
            "ok": false,
            "error": {"code": "unauthorized", "message": "Client was revoked"}
        });
        let mut bytes = serde_json::to_vec(&response)?;
        bytes.push(b'\n');
        let _ = timeout(Duration::from_secs(10), async {
            connection.write_all(&bytes).await?;
            connection.flush().await
        })
        .await;
        return Ok(());
    }

    let ready = json!({"id": id, "ok": true, "result": {"shell_ready": true, "version": 1}});
    let mut ready_bytes = serde_json::to_vec(&ready)?;
    ready_bytes.push(b'\n');
    timeout(Duration::from_secs(10), async {
        connection.write_all(&ready_bytes).await?;
        connection.flush().await
    })
    .await??;

    let child_pid = child.id();
    // SAFETY: `OwnedFd` owns each descriptor exclusively and keeps it valid for the lifetime of `AsyncFd`.
    let pty = unsafe { AsyncFd::register(master) }?;
    let session_exit = unsafe { AsyncFd::register(pidfd) }?;
    let (mut reader, mut writer) = tokio::io::split(connection);
    let mut pending_stdin: VecDeque<u8> = VecDeque::new();
    let mut pty_buf = [0u8; 16384];
    let mut net_buf = [0u8; 16384];
    let mut decoder = FrameDecoder::new();
    let mut pty_eof = false;
    let mut exit_reason: Option<(i32, u8)> = None;

    loop {
        if exit_reason.is_some() {
            break;
        }
        let has_stdin = !pending_stdin.is_empty();
        tokio::select! {
            readable = pty.readable(), if !pty_eof => {
                let mut guard = readable?;
                match guard.try_io(|inner| try_read_pty(inner.as_raw_fd(), &mut pty_buf)) {
                    Ok(Ok(0)) => {
                        pty_eof = true;
                    }
                    Ok(Ok(count)) => {
                        if write_frame(&mut writer, KIND_DATA, &pty_buf[..count]).await.is_err() {
                            break;
                        }
                    }
                    Ok(Err(error)) if error.raw_os_error() == Some(libc::EIO) => {
                        pty_eof = true;
                    }
                    Ok(Err(_)) => {
                        pty_eof = true;
                    }
                    Err(_would_block) => {}
                }
            }
            writable = pty.writable(), if has_stdin => {
                let mut guard = writable?;
                let (head, _) = pending_stdin.as_slices();
                if !head.is_empty() {
                    match guard.try_io(|inner| try_write_pty(inner.as_raw_fd(), head)) {
                        Ok(Ok(written)) if written > 0 => {
                            pending_stdin.drain(..written);
                        }
                        Ok(Ok(_)) => {}
                        Ok(Err(error)) if error.raw_os_error() == Some(libc::EIO) => {
                            pending_stdin.clear();
                        }
                        Ok(Err(_)) => {
                            pending_stdin.clear();
                        }
                        Err(_would_block) => {}
                    }
                }
            }
            read = reader.read(&mut net_buf), if pending_stdin.len() < MAX_PENDING_STDIN => {
                match read {
                    Ok(0) | Err(_) => break,
                    Ok(count) => {
                        decoder.push(&net_buf[..count]);
                        let mut protocol_error = false;
                        while let Ok(Some((kind, data))) = decoder.next_frame().inspect_err(|_| protocol_error = true) {
                            match kind {
                                KIND_DATA => pending_stdin.extend(data),
                                KIND_RESIZE if data.len() == 4 => {
                                    let cols = u16::from_be_bytes([data[0], data[1]]);
                                    let rows = u16::from_be_bytes([data[2], data[3]]);
                                    let _ = set_winsize(pty.as_raw_fd(), cols, rows);
                                }
                                KIND_PING => {
                                    let sent = write_frame(&mut writer, KIND_PONG, &[]).await;
                                    if sent.is_err() {
                                        protocol_error = true;
                                        break;
                                    }
                                }
                                KIND_PONG => {}
                                _ => {}
                            }
                        }
                        if protocol_error {
                            break;
                        }
                    }
                }
            }
            status = child.wait() => {
                // Briefly drain remaining buffered PTY output before delivering EXIT.
                let drain_deadline = tokio::time::Instant::now() + Duration::from_millis(120);
                while !pty_eof && tokio::time::Instant::now() < drain_deadline {
                    match timeout(Duration::from_millis(20), pty.readable()).await {
                        Ok(Ok(mut guard)) => {
                            match guard.try_io(|inner| try_read_pty(inner.as_raw_fd(), &mut pty_buf)) {
                                Ok(Ok(count)) if count > 0 => {
                                    if write_frame(&mut writer, KIND_DATA, &pty_buf[..count]).await.is_err() {
                                        break;
                                    }
                                }
                                Ok(Ok(_)) | Ok(Err(_)) => {
                                    pty_eof = true;
                                }
                                Err(_) => {}
                            }
                        }
                        _ => break,
                    }
                }
                let code = match status {
                    Ok(status) => status.code().unwrap_or_else(|| {
                        use std::os::unix::process::ExitStatusExt;
                        status.signal().map_or(1, |sig| 128 + sig)
                    }),
                    Err(_) => 1,
                };
                exit_reason = Some((code, REASON_EXITED));
            }
            _ = session_exit.readable() => {
                exit_reason = Some((0, REASON_SESSION_ENDED));
            }
            changed = changes.changed() => {
                if changed.is_err() || !is_authorized() {
                    exit_reason = Some((0, REASON_REVOKED));
                }
            }
        }
    }

    if let Some((code, reason)) = exit_reason {
        let _ = timeout(
            Duration::from_secs(2),
            write_exit_frame(&mut writer, code, reason),
        )
        .await;
    }

    // Close the PTY master first so the kernel delivers hangup/EOF immediately.
    drop(pty);

    // Ensure the shell process group is cleaned up if still alive.
    if child.try_wait().ok().flatten().is_none() {
        terminate_process_group(child_pid, libc::SIGHUP);
        if timeout(Duration::from_millis(300), child.wait())
            .await
            .is_err()
        {
            terminate_process_group(child_pid, libc::SIGKILL);
            let _ = timeout(Duration::from_millis(500), child.wait()).await;
        }
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn quotes_posix_and_fish_safely() {
        assert_eq!(posix_quote("abc").unwrap(), "'abc'");
        assert_eq!(posix_quote("a'b\"$HOME").unwrap(), "'a'\\''b\"$HOME'");
        assert!(posix_quote("bad\0val").is_err());

        assert_eq!(fish_quote("a'b\\c").unwrap(), "'a\\'b\\\\c'");
        assert!(fish_quote("bad\0val").is_err());
    }

    #[test]
    fn builds_rc_with_exports_unsets_and_user_hook() {
        let mut env = BTreeMap::new();
        env.insert("WAYLAND_DISPLAY".into(), Some("waybridge-0".into()));
        env.insert("DISPLAY".into(), Some(":1000".into()));
        env.insert("GTK_IM_MODULE".into(), None);
        env.insert("INVALID-KEY".into(), Some("ignored".into()));
        let posix = build_posix_env_block("waybridge-0", &env).unwrap();
        assert!(posix.contains("export WAYLAND_DISPLAY='waybridge-0'\n"));
        assert!(posix.contains("export DISPLAY=':1000'\n"));
        assert!(posix.contains("unset GTK_IM_MODULE\n"));
        assert!(posix.contains("export WAYBRIDGE_SHELL='1'\n"));
        assert!(posix.contains("waybridge/shellrc"));
        assert!(!posix.contains("INVALID-KEY"));

        let fish = build_fish_env_block("waybridge-0", &env).unwrap();
        assert!(fish.contains("set -gx WAYLAND_DISPLAY 'waybridge-0'\n"));
        assert!(fish.contains("set -e GTK_IM_MODULE\n"));
        assert!(fish.contains("waybridge/shellrc.fish"));
    }

    #[test]
    fn validates_session_names_and_terms() {
        assert!(valid_session_name("waybridge-0"));
        assert!(!valid_session_name("../escape"));
        assert!(!valid_session_name(""));
        assert!(valid_term("xterm-256color"));
        assert!(!valid_term("xterm;rm -rf /'"));
        assert!(!valid_term("bad\nterm"));
    }
}
