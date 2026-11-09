//! Relay only the host keyring services into the private application bus.
//! Each private caller keeps its own host connection (Secret Service sessions
//! are connection-owned). Message bodies are never logged or persisted.
use anyhow::{bail, Context, Result};
use std::{
    collections::{BTreeMap, BTreeSet},
    sync::{Arc, Mutex},
    thread::{self, JoinHandle},
};
use zbus::{
    blocking::{connection::Builder, Connection, MessageIterator},
    message::{Builder as MessageBuilder, Flags, Type},
    Message,
};

const NAMES: &[&str] = &[
    "org.freedesktop.secrets",
    "org.kde.kwalletd",
    "org.kde.kwalletd5",
    "org.kde.kwalletd6",
    "org.kde.ksecretd",
    "org.kde.secretservicecompat",
];
const DBUS: &str = "org.freedesktop.DBus";
const PATH: &str = "/org/freedesktop/DBus";
type Pending = Arc<Mutex<BTreeMap<u32, Message>>>;

fn copy_body(builder: MessageBuilder<'_>, message: &Message) -> Result<Message> {
    let body = message.body();
    let fds = body
        .data()
        .fds()
        .iter()
        .map(|fd| fd.try_to_owned().map(Into::into))
        .collect::<std::result::Result<Vec<_>, _>>()?;
    // Same validated signature, bytes, endian and descriptor order as received;
    // D-Bus body alignment is eight bytes in both messages.
    Ok(unsafe {
        builder
            .endian(body.data().context().endian())
            .build_raw_body(body.data(), body.signature(), fds)?
    })
}
fn connect(address: &str) -> Result<Connection> {
    Ok(Builder::address(address)?.max_queued(256).build()?)
}
fn add_match(connection: &Connection, rule: &str) -> Result<()> {
    connection.call_method(Some(DBUS), PATH, Some(DBUS), "AddMatch", &rule)?;
    Ok(())
}
fn list(connection: &Connection, method: &str) -> Result<Vec<String>> {
    Ok(connection
        .call_method(Some(DBUS), PATH, Some(DBUS), method, &())?
        .body()
        .deserialize()?)
}

struct Host {
    connection: Connection,
    pending: Pending,
    worker: Option<JoinHandle<()>>,
}
impl Host {
    fn new(address: &str, private: Connection, caller: String, names: &[String]) -> Result<Self> {
        let connection = connect(address)?;
        let mut stream = MessageIterator::from(&connection);
        let mut owners = BTreeMap::<String, String>::new();
        for name in names {
            // Subscribe before querying so a wallet restart cannot leave a stale
            // unique owner and silently discard all subsequent prompt signals.
            add_match(
                &connection,
                &format!("type='signal',sender='{DBUS}',member='NameOwnerChanged',arg0='{name}'"),
            )?;
            if let Ok(reply) =
                connection.call_method(Some(DBUS), PATH, Some(DBUS), "GetNameOwner", &name)
            {
                owners.insert(name.clone(), reply.body().deserialize::<String>()?);
            }
            add_match(&connection, &format!("type='signal',sender='{name}'"))?;
        }
        let pending: Pending = Arc::default();
        let responses = pending.clone();
        let worker = thread::spawn(move || {
            while let Some(Ok(message)) = stream.next() {
                let header = message.header();
                if matches!(message.message_type(), Type::MethodReturn | Type::Error) {
                    let request = header
                        .reply_serial()
                        .and_then(|serial| responses.lock().unwrap().remove(&serial.get()));
                    if let Some(request) = request {
                        let builder = if message.message_type() == Type::Error {
                            Message::error(&request.header(), header.error_name().unwrap().as_str())
                        } else {
                            Message::method_return(&request.header())
                        };
                        if let Ok(reply) = builder
                            .map_err(anyhow::Error::from)
                            .and_then(|b| copy_body(b, &message))
                        {
                            let _ = private.send(&reply);
                        }
                    }
                } else if message.message_type() == Type::Signal {
                    if header.interface().is_some_and(|i| i.as_str() == DBUS) {
                        if header
                            .member()
                            .is_some_and(|m| m.as_str() == "NameOwnerChanged")
                        {
                            if let Ok((name, _, owner)) =
                                message.body().deserialize::<(String, String, String)>()
                            {
                                if owners.contains_key(&name) || names_contains(&name) {
                                    owners.insert(name, owner);
                                }
                            }
                        }
                        continue;
                    }
                    if !header
                        .sender()
                        .is_some_and(|s| owners.values().any(|o| o == s.as_str()))
                    {
                        continue;
                    }
                    if let (Some(path), Some(interface), Some(member)) =
                        (header.path(), header.interface(), header.member())
                    {
                        let result = Message::signal(path, interface, member)
                            .and_then(|b| b.destination(caller.as_str()))
                            .and_then(|b| b.sender(private.unique_name().unwrap().as_str()))
                            .map_err(anyhow::Error::from)
                            .and_then(|b| copy_body(b, &message));
                        if let Ok(signal) = result {
                            let _ = private.send(&signal);
                        }
                    }
                }
            }
        });
        Ok(Self {
            connection,
            pending,
            worker: Some(worker),
        })
    }
    fn forward(&self, message: Message, destination: &str) -> Result<()> {
        let header = message.header();
        if message.body().len() > 1024 * 1024 {
            bail!("keyring request exceeds limit");
        }
        let mut builder = Message::method_call(
            header.path().context("missing keyring path")?,
            header.member().context("missing keyring method")?,
        )?
        .destination(destination)?
        .sender(self.connection.unique_name().unwrap().as_str())?;
        if let Some(interface) = header.interface() {
            builder = builder.interface(interface)?;
        }
        let no_reply = message
            .primary_header()
            .flags()
            .contains(Flags::NoReplyExpected);
        if no_reply {
            builder = builder.with_flags(Flags::NoReplyExpected)?;
        }
        let outgoing = copy_body(builder, &message)?;
        let serial = outgoing.primary_header().serial_num().get();
        {
            let mut pending = self.pending.lock().unwrap();
            if pending.len() >= 64 {
                bail!("too many pending keyring requests");
            }
            if !no_reply {
                pending.insert(serial, message);
            }
        }
        if let Err(error) = self.connection.send(&outgoing) {
            self.pending.lock().unwrap().remove(&serial);
            return Err(error.into());
        }
        Ok(())
    }
}
impl Drop for Host {
    fn drop(&mut self) {
        let _ = self.connection.clone().close();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}
fn names_contains(name: &str) -> bool {
    NAMES.contains(&name)
}

pub struct Bridge {
    connection: Connection,
    worker: Option<JoinHandle<()>>,
    desktop: String,
    kde_version: Option<String>,
}
impl Bridge {
    pub fn new(private_address: &str) -> Result<Option<Self>> {
        let host_address = std::env::var("DBUS_SESSION_BUS_ADDRESS")
            .unwrap_or_else(|_| format!("unix:path=/run/user/{}/bus", unsafe { libc::getuid() }));
        Self::with_host(&host_address, private_address)
    }
    fn with_host(host_address: &str, private_address: &str) -> Result<Option<Self>> {
        let host = connect(host_address).context("connecting host keyring bus")?;
        let available: BTreeSet<_> = list(&host, "ListNames")?
            .into_iter()
            .chain(list(&host, "ListActivatableNames")?)
            .collect();
        let names: Vec<String> = NAMES
            .iter()
            .filter(|n| available.contains(**n))
            .map(|n| n.to_string())
            .collect();
        if names.is_empty() {
            return Ok(None);
        }
        let desktop = std::env::var("XDG_CURRENT_DESKTOP")
            .ok()
            .filter(|s| !s.is_empty())
            .unwrap_or_else(|| {
                if names.iter().any(|n| n.starts_with("org.kde.")) {
                    "KDE".into()
                } else {
                    "GNOME".into()
                }
            });
        // SSH shells often lack KDE_SESSION_VERSION. Chromium otherwise treats
        // XDG_CURRENT_DESKTOP=KDE as KDE4 and selects the wrong wallet backend.
        let kde_version = desktop
            .split(':')
            .any(|d| d == "KDE")
            .then(|| {
                std::env::var("KDE_SESSION_VERSION")
                    .ok()
                    .filter(|v| !v.is_empty())
                    .or_else(|| infer_kde_version(&names).map(String::from))
            })
            .flatten();
        let connection = connect(private_address)?;
        let mut messages = MessageIterator::from(&connection);
        for name in &names {
            connection.request_name(name.as_str())?;
        }
        add_match(
            &connection,
            "type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged'",
        )?;
        let proxy = connection.clone();
        let host_address = host_address.to_string();
        let service_count = names.len();
        let worker = thread::spawn(move || {
            let mut clients = BTreeMap::<String, Host>::new();
            while let Some(Ok(message)) = messages.next() {
                let header = message.header();
                if message.message_type() == Type::Signal
                    && header
                        .member()
                        .is_some_and(|m| m.as_str() == "NameOwnerChanged")
                {
                    if let Ok((name, _, owner)) =
                        message.body().deserialize::<(String, String, String)>()
                    {
                        if owner.is_empty() {
                            clients.remove(&name);
                        }
                    }
                    continue;
                }
                if message.message_type() != Type::MethodCall {
                    continue;
                }
                let Some(sender) = header.sender().map(|s| s.to_string()) else {
                    continue;
                };
                let mut destination = header
                    .destination()
                    .map(|d| d.to_string())
                    .unwrap_or_default();
                if !names.contains(&destination) {
                    let path = header.path().map(|p| p.as_str()).unwrap_or("");
                    destination = if path.starts_with("/org/freedesktop/secrets") {
                        "org.freedesktop.secrets".into()
                    } else {
                        names
                            .iter()
                            .find(|n| path.contains(n.rsplit('.').next().unwrap()))
                            .cloned()
                            .unwrap_or_default()
                    };
                }
                let result = (|| -> Result<()> {
                    if !names.contains(&destination) {
                        bail!("unknown keyring destination");
                    }
                    if !clients.contains_key(&sender) {
                        if clients.len() >= 32 {
                            bail!("too many keyring clients");
                        }
                        clients.insert(
                            sender.clone(),
                            Host::new(&host_address, proxy.clone(), sender.clone(), &names)?,
                        );
                    }
                    clients[&sender].forward(message.clone(), &destination)
                })();
                if result.is_err()
                    && !message
                        .primary_header()
                        .flags()
                        .contains(Flags::NoReplyExpected)
                {
                    let _ = proxy.reply_error(
                        &header,
                        "org.freedesktop.DBus.Error.Failed",
                        &"Host keyring bridge unavailable",
                    );
                }
            }
        });
        tracing::info!(
            services = service_count,
            "host keyring services shared with private application bus"
        );
        Ok(Some(Self {
            connection,
            worker: Some(worker),
            desktop,
            kde_version,
        }))
    }
    pub fn desktop(&self) -> &str {
        &self.desktop
    }
    pub fn kde_version(&self) -> Option<&str> {
        self.kde_version.as_deref()
    }
}
fn infer_kde_version(names: &[String]) -> Option<&'static str> {
    if names
        .iter()
        .any(|n| n == "org.kde.kwalletd6" || n == "org.kde.ksecretd")
    {
        Some("6")
    } else if names.iter().any(|n| n == "org.kde.kwalletd5") {
        Some("5")
    } else if names.iter().any(|n| n == "org.kde.kwalletd") {
        Some("4")
    } else {
        None
    }
}
impl Drop for Bridge {
    fn drop(&mut self) {
        let _ = self.connection.clone().close();
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        io::{BufRead, BufReader},
        os::fd::AsFd,
        process::{Child, Command, Stdio},
    };
    struct Bus(Child, String);
    impl Bus {
        fn new() -> Self {
            let mut child = Command::new("dbus-daemon")
                .args(["--session", "--nofork", "--print-address=1"])
                .stdout(Stdio::piped())
                .spawn()
                .unwrap();
            let mut address = String::new();
            BufReader::new(child.stdout.take().unwrap())
                .read_line(&mut address)
                .unwrap();
            Self(child, address.trim().to_string())
        }
    }
    impl Drop for Bus {
        fn drop(&mut self) {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }
    struct FakeWallet {
        notes: Arc<std::sync::atomic::AtomicUsize>,
    }
    #[test]
    fn kde_version_hint_matches_available_wallet_backend() {
        for (name, expected) in [
            ("org.kde.ksecretd", Some("6")),
            ("org.kde.kwalletd6", Some("6")),
            ("org.kde.kwalletd5", Some("5")),
            ("org.kde.kwalletd", Some("4")),
            ("org.freedesktop.secrets", None),
        ] {
            assert_eq!(infer_kde_version(&[name.into()]), expected);
        }
    }
    #[zbus::interface(name = "org.kde.KWallet")]
    impl FakeWallet {
        fn echo(&self, text: &str, bytes: Vec<u8>) -> (String, Vec<u8>) {
            (text.into(), bytes)
        }
        fn owner(&self, #[zbus(header)] header: zbus::message::Header<'_>) -> String {
            header.sender().unwrap().to_string()
        }
        fn fail(&self) -> zbus::fdo::Result<()> {
            Err(zbus::fdo::Error::NotSupported("fixture".into()))
        }
        fn echo_fd(&self, fd: zbus::zvariant::OwnedFd) -> zbus::zvariant::OwnedFd {
            fd
        }
        fn note(&self) {
            self.notes
                .fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        }
    }
    #[test]
    fn private_bus_keyring_relay_preserves_sessions_errors_and_signals() {
        let host = Bus::new();
        let private = Bus::new();
        let _unrelated = Builder::address(host.1.as_str())
            .unwrap()
            .name("org.kde.dolphin")
            .unwrap()
            .build()
            .unwrap();
        let notes = Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let service = Builder::address(host.1.as_str())
            .unwrap()
            .name("org.kde.kwalletd6")
            .unwrap()
            .serve_at(
                "/modules/kwalletd6",
                FakeWallet {
                    notes: notes.clone(),
                },
            )
            .unwrap()
            .build()
            .unwrap();
        let bridge = Bridge::with_host(&host.1, &private.1).unwrap().unwrap();
        let a = connect(&private.1).unwrap();
        let b = connect(&private.1).unwrap();
        let call = |c: &Connection, method: &str| {
            c.call_method(
                Some("org.kde.kwalletd6"),
                "/modules/kwalletd6",
                Some("org.kde.KWallet"),
                method,
                &(),
            )
        };
        let payload = ("test界面", vec![0u8, 12, 128, 255]);
        let reply = a
            .call_method(
                Some("org.kde.kwalletd6"),
                "/modules/kwalletd6",
                Some("org.kde.KWallet"),
                "Echo",
                &payload,
            )
            .unwrap();
        assert_eq!(
            reply.body().deserialize::<(String, Vec<u8>)>().unwrap(),
            (payload.0.into(), payload.1)
        );
        use std::io::{Read, Seek, Write};
        let mut file = tempfile::tempfile().unwrap();
        file.write_all(b"descriptor round trip").unwrap();
        file.rewind().unwrap();
        let reply = a
            .call_method(
                Some("org.kde.kwalletd6"),
                "/modules/kwalletd6",
                Some("org.kde.KWallet"),
                "EchoFd",
                &zbus::zvariant::Fd::from(file.as_fd()),
            )
            .unwrap();
        let fd = reply
            .body()
            .deserialize::<zbus::zvariant::OwnedFd>()
            .unwrap();
        let owned: std::os::fd::OwnedFd = fd.into();
        let mut contents = String::new();
        std::fs::File::from(owned)
            .read_to_string(&mut contents)
            .unwrap();
        assert_eq!(contents, "descriptor round trip");
        for _ in 0..100 {
            let message = Message::method_call("/modules/kwalletd6", "Note")
                .unwrap()
                .destination("org.kde.kwalletd6")
                .unwrap()
                .interface("org.kde.KWallet")
                .unwrap()
                .with_flags(Flags::NoReplyExpected)
                .unwrap()
                .build(&())
                .unwrap();
            a.send(&message).unwrap();
        }
        let owner_a = call(&a, "Owner")
            .unwrap()
            .body()
            .deserialize::<String>()
            .unwrap();
        let unique = a
            .call_method(
                Some(DBUS),
                PATH,
                Some(DBUS),
                "GetNameOwner",
                &"org.kde.kwalletd6",
            )
            .unwrap()
            .body()
            .deserialize::<String>()
            .unwrap();
        let via_unique = a
            .call_method(
                Some(unique.as_str()),
                "/modules/kwalletd6",
                Some("org.kde.KWallet"),
                "Owner",
                &(),
            )
            .unwrap()
            .body()
            .deserialize::<String>()
            .unwrap();
        assert_eq!(
            via_unique, owner_a,
            "calls to a resolved unique owner keep the same host session"
        );
        assert_eq!(notes.load(std::sync::atomic::Ordering::Relaxed), 100);
        let owner_b = call(&b, "Owner")
            .unwrap()
            .body()
            .deserialize::<String>()
            .unwrap();
        assert_ne!(
            owner_a, owner_b,
            "host Secret Service sessions must remain per private caller"
        );
        assert!(
            matches!(call(&a,"Fail"),Err(zbus::Error::MethodError(name,_,_)) if name.as_str()=="org.freedesktop.DBus.Error.NotSupported")
        );
        let rule = zbus::MatchRule::builder()
            .msg_type(Type::Signal)
            .sender("org.kde.kwalletd6")
            .unwrap()
            .interface("org.kde.KWallet")
            .unwrap()
            .member("Changed")
            .unwrap()
            .build();
        let mut signals = MessageIterator::for_match_rule(rule, &a, Some(4)).unwrap();
        service
            .emit_signal(
                Some(owner_a.as_str()),
                "/modules/kwalletd6",
                "org.kde.KWallet",
                "Changed",
                &(77u32,),
            )
            .unwrap();
        assert_eq!(
            signals
                .next()
                .unwrap()
                .unwrap()
                .body()
                .deserialize::<u32>()
                .unwrap(),
            77
        );
        assert!(!list(&a, "ListNames")
            .unwrap()
            .contains(&"org.kde.dolphin".into()));
        drop(signals);
        a.close().unwrap();
        b.close().unwrap();
        drop(bridge);
    }
}
