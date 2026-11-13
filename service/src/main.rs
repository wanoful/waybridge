mod storage;

use anyhow::{bail, Context, Result};
use clap::{Parser, Subcommand};
use serde_json::{json, Value};
use std::{
    fs::{self, OpenOptions},
    os::{
        fd::AsRawFd,
        unix::{
            fs::{FileTypeExt, MetadataExt, OpenOptionsExt, PermissionsExt},
            process::CommandExt,
        },
    },
    path::{Path, PathBuf},
    process::Stdio,
    sync::{Arc, Mutex},
    time::Duration,
};
use tokio::{
    io::{AsyncBufRead, AsyncBufReadExt, AsyncReadExt, AsyncWrite, AsyncWriteExt, BufReader},
    net::{TcpListener, TcpStream, UnixListener, UnixStream},
    process::Command,
    sync::{watch, Semaphore},
    time::timeout,
};
use tokio_rustls::{
    rustls::{
        self,
        pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer},
    },
    TlsAcceptor,
};

const MAX_REQUEST: usize = 65536;
const MAX_RESPONSE: usize = 4 * 1024 * 1024;
const HANDSHAKE_TIMEOUT: Duration = Duration::from_secs(10);
const HELPER: &str = include_str!("../../scripts/session-control.py");

#[derive(Parser)]
#[command(
    version,
    about = "Secure direct entry point for same-user Waybridge sessions"
)]
struct Args {
    /// Replace the XDG user configuration file for serve and configuration checks.
    #[arg(long, global = true)]
    config: Option<PathBuf>,
    /// Validate configuration without creating identity/state or starting listeners.
    #[arg(long, global = true)]
    check_config: bool,
    /// Print effective configuration and sources as JSON, then exit.
    #[arg(long, global = true)]
    print_config: bool,
    #[arg(long, global = true)]
    state_dir: Option<PathBuf>,
    #[arg(long, global = true)]
    runtime_dir: Option<PathBuf>,
    #[command(subcommand)]
    command: Option<Action>,
}

#[derive(Subcommand)]
enum Action {
    /// Start the TLS listener and owner-only local administration socket.
    Serve {
        #[arg(long)]
        listen: Option<String>,
        #[arg(long)]
        compositor: Option<String>,
        #[arg(long)]
        python: Option<String>,
    },
    /// Create an expiring, one-use pairing code. Transfer its certificate pin independently.
    Pair {
        #[arg(long, default_value_t = 120, value_parser = clap::value_parser!(u64).range(10..=600))]
        expires: u64,
    },
    Status,
    Clients,
    Revoke {
        client_id: String,
    },
}

struct Shared {
    store: Mutex<storage::Store>,
    certificate_sha256: String,
    listen: String,
    compositor: String,
    python: String,
    config_path: Option<PathBuf>,
    changed: watch::Sender<u64>,
    rpc_slots: Semaphore,
}

fn locations(args: &Args) -> Result<(PathBuf, PathBuf)> {
    let state = args
        .state_dir
        .clone()
        .or_else(|| {
            std::env::var_os("XDG_STATE_HOME")
                .map(PathBuf::from)
                .map(|path| path.join("waybridge/service"))
        })
        .or_else(|| {
            std::env::var_os("HOME")
                .map(PathBuf::from)
                .map(|path| path.join(".local/state/waybridge/service"))
        })
        .context("set --state-dir or XDG_STATE_HOME")?;
    let runtime = args.runtime_dir.clone().unwrap_or_else(|| {
        std::env::var_os("XDG_RUNTIME_DIR")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from(format!("/run/user/{}", unsafe { libc::getuid() })))
            .join("waybridge-service")
    });
    Ok((state, runtime))
}

fn identity(directory: &Path) -> Result<(Vec<u8>, Vec<u8>)> {
    let cert_path = directory.join("identity.cert.der");
    let key_path = directory.join("identity.key.der");
    let cert = storage::read_private(&cert_path, 16384);
    let key = storage::read_private(&key_path, 16384);
    if cert.as_ref().err().is_some_and(is_missing) && key.as_ref().err().is_some_and(is_missing) {
        let rcgen::CertifiedKey { cert, signing_key } = rcgen::generate_simple_self_signed(vec![
            "waybridge.local".into(),
            "localhost".into(),
            "127.0.0.1".into(),
            "::1".into(),
        ])?;
        let cert = cert.der().to_vec();
        let key = signing_key.serialize_der();
        storage::atomic_private(&key_path, &key)?;
        storage::atomic_private(&cert_path, &cert)?;
        return Ok((cert, key));
    }
    Ok((
        cert.context("cannot read service certificate; refusing implicit identity reset")?,
        key.context("cannot read service key; refusing implicit identity reset")?,
    ))
}

fn is_missing(error: &anyhow::Error) -> bool {
    error
        .downcast_ref::<std::io::Error>()
        .is_some_and(|error| error.kind() == std::io::ErrorKind::NotFound)
}

fn lock_state(directory: &Path) -> Result<fs::File> {
    let file = OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW)
        .open(directory.join("service.lock"))?;
    let metadata = file.metadata()?;
    if !metadata.is_file()
        || metadata.uid() != unsafe { libc::getuid() }
        || metadata.permissions().mode() & 0o077 != 0
    {
        bail!("invalid service lock file");
    }
    if unsafe { libc::flock(file.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 {
        bail!("another service instance owns this state directory");
    }
    Ok(file)
}

async fn read_request<R: AsyncBufRead + Unpin>(reader: &mut R) -> Result<Value> {
    let mut bytes = Vec::new();
    loop {
        let available = reader.fill_buf().await?;
        if available.is_empty() {
            bail!("request ended before newline");
        }
        let newline = available.iter().position(|byte| *byte == b'\n');
        let used = newline.map_or(available.len(), |position| position + 1);
        if bytes.len() + used > MAX_REQUEST {
            bail!("request exceeds 65536-byte limit");
        }
        bytes.extend_from_slice(&available[..used]);
        reader.consume(used);
        if newline.is_some() {
            break;
        }
    }
    let value: Value = serde_json::from_slice(&bytes).context("invalid JSON request")?;
    if !value.is_object() {
        bail!("request must be an object");
    }
    Ok(value)
}

async fn write_response<W: AsyncWrite + Unpin>(
    writer: &mut W,
    id: &Value,
    result: std::result::Result<Value, (&str, String)>,
) -> Result<()> {
    let response = match result {
        Ok(result) => json!({"id":id,"ok":true,"result":result}),
        Err((code, message)) => json!({"id":id,"ok":false,"error":{"code":code,"message":message}}),
    };
    let mut bytes = serde_json::to_vec(&response)?;
    if bytes.len() > MAX_RESPONSE {
        bail!("response exceeds limit");
    }
    bytes.push(b'\n');
    timeout(HANDSHAKE_TIMEOUT, async {
        writer.write_all(&bytes).await?;
        writer.flush().await
    })
    .await??;
    Ok(())
}

fn text_field<'a>(request: &'a Value, field: &str) -> Result<&'a str> {
    request
        .get(field)
        .and_then(Value::as_str)
        .filter(|value| !value.is_empty() && value.len() <= 8192)
        .with_context(|| format!("missing or invalid {field}"))
}

fn authenticated(shared: &Shared, request: &Value) -> bool {
    let (Some(id), Some(token)) = (
        request["client_id"].as_str(),
        request["client_token"].as_str(),
    ) else {
        return false;
    };
    shared.store.lock().unwrap().authorized(id, token)
}

async fn helper(shared: &Shared, request: &Value) -> Result<Value> {
    let _slot = timeout(Duration::from_secs(2), shared.rpc_slots.acquire())
        .await
        .context("session manager is busy")??;
    let mut request = request.clone();
    let map = request.as_object_mut().context("invalid request")?;
    map.remove("client_id");
    map.remove("client_token");
    // A remote peer cannot choose configuration, programs, or server limits.
    map.remove("config");
    if let Some(path) = &shared.config_path {
        map.insert("config".into(), json!(path));
    }
    map.insert("binary".into(), Value::String(shared.compositor.clone()));
    map.insert("id".into(), json!(1));
    let mut bytes = serde_json::to_vec(&request)?;
    if bytes.len() >= MAX_REQUEST {
        bail!("session request exceeds limit");
    }
    bytes.push(b'\n');
    let mut command = Command::new(&shared.python);
    command
        .args(["-I", "-c", HELPER])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .kill_on_drop(true);
    // Session startup spawns detached owned children; dropping the entry point
    // must not terminate the surviving compositor or applications.
    unsafe {
        command.as_std_mut().pre_exec(|| {
            if libc::setsid() == -1 {
                return Err(std::io::Error::last_os_error());
            }
            Ok(())
        });
    }
    let mut child = command.spawn().context("starting local session helper")?;
    let mut stdin = child.stdin.take().context("helper stdin unavailable")?;
    let stdout = child.stdout.take().context("helper stdout unavailable")?;
    let operation = async {
        stdin.write_all(&bytes).await?;
        stdin.shutdown().await?;
        drop(stdin);
        let mut response = Vec::new();
        stdout
            .take(MAX_RESPONSE as u64 + 1)
            .read_to_end(&mut response)
            .await?;
        if response.len() > MAX_RESPONSE {
            bail!("session response exceeds limit");
        }
        let status = child.wait().await?;
        if !status.success() {
            bail!("local session helper failed");
        }
        let message: Value =
            serde_json::from_slice(&response).context("invalid local session response")?;
        if message["ok"] != true {
            bail!(
                "{}",
                message["error"]
                    .as_str()
                    .unwrap_or("session operation failed")
            );
        }
        Ok(message["result"].clone())
    };
    timeout(Duration::from_secs(35), operation)
        .await
        .context("session operation timed out")?
}

async fn remote(
    shared: Arc<Shared>,
    mut connection: BufReader<tokio_rustls::server::TlsStream<TcpStream>>,
) -> Result<()> {
    let request = match timeout(HANDSHAKE_TIMEOUT, read_request(&mut connection)).await {
        Ok(Ok(request)) => request,
        Ok(Err(error)) => {
            write_response(
                &mut connection,
                &Value::Null,
                Err(("bad_request", error.to_string())),
            )
            .await?;
            return Ok(());
        }
        Err(_) => return Ok(()),
    };
    let id = request.get("id").cloned().unwrap_or(Value::Null);
    let op = request["op"].as_str().unwrap_or("");
    if op == "hello" {
        return write_response(&mut connection, &id, Ok(json!({"version":1,"certificate_sha256":shared.certificate_sha256,"features":["pairing","sessions","launch","applications","stream","revoke"]}))).await;
    }
    if op == "pair" {
        let result = (|| {
            let code = text_field(&request, "pair_code")?;
            let label = text_field(&request, "label")?;
            let (client_id, client_token) = shared.store.lock().unwrap().pair(code, label)?;
            Ok(
                json!({"client_id":client_id,"client_token":client_token,"certificate_sha256":shared.certificate_sha256}),
            )
        })();
        return write_response(
            &mut connection,
            &id,
            result.map_err(|_: anyhow::Error| {
                (
                    "unauthorized",
                    "Pairing failed: invalid/expired code, label, or client capacity".into(),
                )
            }),
        )
        .await;
    }
    if !authenticated(&shared, &request) {
        return write_response(
            &mut connection,
            &id,
            Err(("unauthorized", "Client is not authorized".into())),
        )
        .await;
    }
    if op == "clients" {
        let result = shared.store.lock().unwrap().clients();
        return write_response(&mut connection, &id, Ok(result)).await;
    }
    if op == "revoke" {
        let result = revoke(&shared, &request);
        return write_response(
            &mut connection,
            &id,
            result.map_err(|error| ("bad_request", error.to_string())),
        )
        .await;
    }
    if op == "stream" {
        let session = match text_field(&request, "session") {
            Ok(value) => value,
            Err(error) => {
                write_response(
                    &mut connection,
                    &id,
                    Err(("bad_request", error.to_string())),
                )
                .await?;
                return Ok(());
            }
        };
        let backend = helper(
            &shared,
            &json!({"op":"ensure","session":session,"mode":"attach"}),
        )
        .await;
        let backend = match backend {
            Ok(value) => value,
            Err(error) => {
                write_response(
                    &mut connection,
                    &id,
                    Err(("operation_failed", error.to_string())),
                )
                .await?;
                return Ok(());
            }
        };
        let Some(port) = backend["port"]
            .as_u64()
            .filter(|value| *value > 0 && *value <= 65535)
        else {
            bail!("invalid backend port");
        };
        let mut backend = match timeout(
            Duration::from_secs(3),
            TcpStream::connect(("127.0.0.1", port as u16)),
        )
        .await
        {
            Ok(Ok(backend)) => backend,
            _ => {
                write_response(
                    &mut connection,
                    &id,
                    Err(("operation_failed", "Session endpoint is unavailable".into())),
                )
                .await?;
                return Ok(());
            }
        };
        backend.set_nodelay(true)?;
        let mut changes = shared.changed.subscribe();
        if !authenticated(&shared, &request) {
            return write_response(
                &mut connection,
                &id,
                Err(("unauthorized", "Client was revoked".into())),
            )
            .await;
        }
        write_response(
            &mut connection,
            &id,
            Ok(json!({"stream_ready":true,"version":3})),
        )
        .await?;
        let transfer = tokio::io::copy_bidirectional(&mut connection, &mut backend);
        tokio::pin!(transfer);
        loop {
            tokio::select! {
                result = &mut transfer => {let _ = result; break;},
                changed = changes.changed() => {
                    if changed.is_err() || !authenticated(&shared, &request) {break;}
                }
            }
        }
        return Ok(());
    }
    if !matches!(
        op,
        "list" | "ensure" | "info" | "applications" | "launch" | "launch-plan" | "end"
    ) {
        return write_response(
            &mut connection,
            &id,
            Err(("bad_request", "Unknown operation".into())),
        )
        .await;
    }
    let result = helper(&shared, &request)
        .await
        .map_err(|error| ("operation_failed", error.to_string()));
    write_response(&mut connection, &id, result).await
}

fn revoke(shared: &Shared, request: &Value) -> Result<Value> {
    let target = text_field(request, "revoke_client_id")?;
    let revoked = shared.store.lock().unwrap().revoke(target)?;
    if revoked {
        shared
            .changed
            .send_modify(|generation| *generation = generation.wrapping_add(1));
    }
    Ok(json!({"revoked":revoked}))
}

async fn local(shared: Arc<Shared>, connection: UnixStream) -> Result<()> {
    if connection.peer_cred()?.uid() != unsafe { libc::getuid() } {
        bail!("local administrator UID does not match");
    }
    let mut connection = BufReader::new(connection);
    let request = timeout(HANDSHAKE_TIMEOUT, read_request(&mut connection)).await??;
    let id = request.get("id").cloned().unwrap_or(Value::Null);
    let result = (|| -> Result<Value> {
        match request["op"].as_str().unwrap_or("") {
            "pair" => {
                let expires = request["expires"]
                    .as_u64()
                    .filter(|value| (10..=600).contains(value))
                    .unwrap_or(120);
                let code = shared
                    .store
                    .lock()
                    .unwrap()
                    .pairing_code(Duration::from_secs(expires))?;
                Ok(
                    json!({"pair_code":code,"expires_in":expires,"certificate_sha256":shared.certificate_sha256,"listen":shared.listen}),
                )
            }
            "status" => Ok(
                json!({"version":1,"certificate_sha256":shared.certificate_sha256,"listen":shared.listen}),
            ),
            "clients" => Ok(shared.store.lock().unwrap().clients()),
            "revoke" => revoke(&shared, &request),
            _ => bail!("Unknown local operation"),
        }
    })();
    write_response(
        &mut connection,
        &id,
        result.map_err(|error| ("operation_failed", error.to_string())),
    )
    .await
}

async fn serve(
    state: PathBuf,
    runtime: PathBuf,
    listen: String,
    compositor: String,
    python: String,
    config_path: Option<PathBuf>,
) -> Result<()> {
    storage::private_directory(&state)?;
    storage::private_directory(&runtime)?;
    let state = state.canonicalize()?;
    let runtime = runtime.canonicalize()?;
    let _lock = lock_state(&state)?;
    let _runtime_lock = if runtime != state {
        Some(lock_state(&runtime)?)
    } else {
        None
    };
    let (cert, key) = identity(&state)?;
    let certificate_sha256 = storage::fingerprint(&cert);
    let configuration = rustls::ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(
            vec![CertificateDer::from(cert)],
            PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(key)),
        )?;
    let acceptor = TlsAcceptor::from(Arc::new(configuration));
    let listener = TcpListener::bind(&listen)
        .await
        .context("binding TLS listener")?;
    let listen = listener.local_addr()?.to_string();
    let socket = runtime.join("control.sock");
    match fs::symlink_metadata(&socket) {
        Ok(metadata)
            if metadata.file_type().is_socket() && metadata.uid() == unsafe { libc::getuid() } =>
        {
            fs::remove_file(&socket)?
        }
        Ok(_) => bail!("local control path exists and is not an owned socket"),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(error) => return Err(error.into()),
    }
    let local_listener = UnixListener::bind(&socket)?;
    fs::set_permissions(&socket, fs::Permissions::from_mode(0o600))?;
    let socket_metadata = fs::symlink_metadata(&socket)?;
    let (changed, _) = watch::channel(0);
    let shared = Arc::new(Shared {
        store: Mutex::new(storage::Store::load(&state)?),
        certificate_sha256,
        listen,
        compositor,
        python,
        config_path,
        changed,
        rpc_slots: Semaphore::new(4),
    });
    let ready = json!({"version":1,"listen":shared.listen,"certificate_sha256":shared.certificate_sha256,"control_socket":socket});
    storage::atomic_private(&runtime.join("ready.json"), &serde_json::to_vec(&ready)?)?;
    println!("{ready}");
    let slots = Arc::new(Semaphore::new(64));
    let admin_slots = Arc::new(Semaphore::new(8));
    let mut terminate = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate())?;
    loop {
        tokio::select! {
            result = listener.accept() => {
                let (connection, _) = result?;
                let Ok(slot) = slots.clone().try_acquire_owned() else {continue;};
                connection.set_nodelay(true)?;
                let shared = shared.clone(); let acceptor = acceptor.clone();
                tokio::spawn(async move {
                    let _slot = slot;
                    if let Ok(Ok(connection)) = timeout(HANDSHAKE_TIMEOUT, acceptor.accept(connection)).await {
                        // Do not log request bodies, tokens, pairing codes or session credentials.
                        let _ = remote(shared, BufReader::new(connection)).await;
                    }
                });
            },
            result = local_listener.accept() => {
                let (connection, _) = result?;
                let Ok(slot) = admin_slots.clone().try_acquire_owned() else {continue;};
                let shared = shared.clone();
                tokio::spawn(async move {let _slot = slot; let _ = local(shared, connection).await;});
            },
            _ = tokio::signal::ctrl_c() => break,
            _ = terminate.recv() => break,
        }
    }
    if let Ok(current) = fs::symlink_metadata(&socket) {
        if current.ino() == socket_metadata.ino() && current.dev() == socket_metadata.dev() {
            let _ = fs::remove_file(&socket);
            let _ = fs::remove_file(runtime.join("ready.json"));
        }
    }
    Ok(())
}

#[tokio::main]
async fn main() -> Result<()> {
    let _ = rustls::crypto::ring::default_provider().install_default();
    let args = Args::parse();
    let is_serve = matches!(&args.command, Some(Action::Serve { .. }));
    // Administrative commands address the running service, whose configuration
    // may differ from a file being edited for its next restart.
    if is_serve || args.check_config || args.print_config {
        let mut configuration = waybridge_config::Loaded::load(args.config.as_deref())?;
        if let Some(Action::Serve {
            listen,
            compositor,
            python,
        }) = &args.command
        {
            configuration.apply(waybridge_config::Overrides {
                service_listen: listen.clone(),
                compositor: compositor.clone(),
                python: python.clone(),
                ..Default::default()
            })?;
        }
        if args.print_config {
            println!("{}", serde_json::to_string_pretty(&configuration.report())?);
            return Ok(());
        }
        if args.check_config {
            println!(
                "{}",
                json!({"valid":true,"config_file":configuration.report()["config_file"]})
            );
            return Ok(());
        }
        let (state, runtime) = locations(&args)?;
        let service = configuration.config.service;
        // A selected explicit file remains mandatory. The default XDG file
        // remains optional and is discovered identically by helper children.
        let config_path = configuration
            .explicit
            .then_some(configuration.path)
            .flatten();
        return serve(
            state,
            runtime,
            service.listen,
            service.compositor,
            service.python,
            config_path,
        )
        .await;
    }
    let action = args.command.as_ref().context(
        "specify serve, pair, status, clients or revoke (or --check-config/--print-config)",
    )?;
    let (_, runtime) = locations(&args)?;
    let request = match action {
        Action::Pair { expires } => json!({"op":"pair","expires":expires}),
        Action::Status => json!({"op":"status"}),
        Action::Clients => json!({"op":"clients"}),
        Action::Revoke { client_id } => json!({"op":"revoke","revoke_client_id":client_id}),
        Action::Serve { .. } => unreachable!(),
    };
    let metadata = fs::symlink_metadata(&runtime)?;
    if !metadata.is_dir()
        || metadata.uid() != unsafe { libc::getuid() }
        || metadata.permissions().mode() & 0o077 != 0
    {
        bail!("invalid service runtime directory");
    }
    let mut connection = UnixStream::connect(runtime.join("control.sock"))
        .await
        .context("service is not running")?;
    let mut bytes = serde_json::to_vec(&request)?;
    bytes.push(b'\n');
    connection.write_all(&bytes).await?;
    let mut reader = BufReader::new(connection);
    let response = timeout(HANDSHAKE_TIMEOUT, read_request(&mut reader)).await??;
    println!("{}", serde_json::to_string_pretty(&response)?);
    if response["ok"] != true {
        bail!("local administration request failed");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[tokio::test]
    async fn bounded_request_keeps_stream_prefix_buffered() {
        let mut reader = BufReader::new(&b"{\"op\":\"stream\"}\n\0\0\0\x02hi"[..]);
        assert_eq!(read_request(&mut reader).await.unwrap()["op"], "stream");
        let mut rest = Vec::new();
        reader.read_to_end(&mut rest).await.unwrap();
        assert_eq!(rest, b"\0\0\0\x02hi");
        assert!(
            read_request(&mut BufReader::new(&vec![b'x'; MAX_REQUEST + 1][..]))
                .await
                .is_err()
        );
        assert!(read_request(&mut BufReader::new(&b"[]\n"[..]))
            .await
            .is_err());
    }
}
