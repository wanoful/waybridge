use anyhow::{bail, Context, Result};
use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use ring::{
    digest, hmac,
    rand::{SecureRandom, SystemRandom},
};
use serde::{Deserialize, Serialize};
use std::{
    collections::BTreeMap,
    fs::{self, DirBuilder, OpenOptions},
    io::{Read, Write},
    os::unix::fs::{DirBuilderExt, MetadataExt, OpenOptionsExt, PermissionsExt},
    path::{Path, PathBuf},
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

pub const MAX_CLIENTS: usize = 32;

pub fn random_secret() -> Result<String> {
    let mut bytes = [0u8; 32];
    SystemRandom::new()
        .fill(&mut bytes)
        .map_err(|_| anyhow::anyhow!("secure random generation failed"))?;
    Ok(URL_SAFE_NO_PAD.encode(bytes))
}

pub fn fingerprint(bytes: &[u8]) -> String {
    digest::digest(&digest::SHA256, bytes)
        .as_ref()
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect()
}

pub fn private_directory(path: &Path) -> Result<()> {
    DirBuilder::new().recursive(true).mode(0o700).create(path)?;
    let metadata = fs::symlink_metadata(path)?;
    if !metadata.is_dir()
        || metadata.uid() != unsafe { libc::getuid() }
        || metadata.permissions().mode() & 0o077 != 0
    {
        bail!("service directory must be private (0700), owned by the current user, and not a symlink");
    }
    Ok(())
}

pub fn read_private(path: &Path, limit: usize) -> Result<Vec<u8>> {
    let file = OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK)
        .open(path)?;
    let metadata = file.metadata()?;
    if !metadata.is_file()
        || metadata.uid() != unsafe { libc::getuid() }
        || metadata.permissions().mode() & 0o077 != 0
    {
        bail!("service state must be a private regular file owned by the current user");
    }
    let mut bytes = Vec::new();
    file.take(limit as u64 + 1).read_to_end(&mut bytes)?;
    if bytes.len() > limit {
        bail!("service state exceeds limit");
    }
    Ok(bytes)
}

pub fn atomic_private(path: &Path, bytes: &[u8]) -> Result<()> {
    let temporary = path.with_extension(format!("tmp-{}", random_secret()?));
    let result = (|| {
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW)
            .open(&temporary)?;
        file.write_all(bytes)?;
        file.sync_all()?;
        fs::rename(&temporary, path)?;
        fs::File::open(path.parent().context("state has no parent")?)?.sync_all()?;
        Ok(())
    })();
    if result.is_err() {
        let _ = fs::remove_file(temporary);
    }
    result
}

#[derive(Serialize, Deserialize, Clone)]
pub struct Client {
    pub label: String,
    pub created_unix: u64,
    token_tag: String,
}

#[derive(Serialize, Deserialize)]
struct Persistent {
    version: u8,
    token_key: String,
    clients: BTreeMap<String, Client>,
}

pub struct Store {
    path: PathBuf,
    state: Persistent,
    pairing: BTreeMap<String, Instant>,
}

impl Store {
    pub fn load(directory: &Path) -> Result<Self> {
        private_directory(directory)?;
        let path = directory.join("trust.json");
        let state: Persistent = match read_private(&path, 128 * 1024) {
            Ok(bytes) => serde_json::from_slice(&bytes).context("invalid service trust state")?,
            Err(error)
                if error
                    .downcast_ref::<std::io::Error>()
                    .is_some_and(|error| error.kind() == std::io::ErrorKind::NotFound) =>
            {
                Persistent {
                    version: 1,
                    token_key: random_secret()?,
                    clients: BTreeMap::new(),
                }
            }
            Err(error) => return Err(error),
        };
        if state.version != 1
            || state.clients.len() > MAX_CLIENTS
            || URL_SAFE_NO_PAD.decode(&state.token_key)?.len() != 32
        {
            bail!("invalid service trust state");
        }
        let store = Self {
            path,
            state,
            pairing: BTreeMap::new(),
        };
        store.persist()?;
        Ok(store)
    }

    fn key(&self) -> Result<hmac::Key> {
        Ok(hmac::Key::new(
            hmac::HMAC_SHA256,
            &URL_SAFE_NO_PAD.decode(&self.state.token_key)?,
        ))
    }

    fn persist(&self) -> Result<()> {
        atomic_private(&self.path, &serde_json::to_vec(&self.state)?)
    }

    pub fn pairing_code(&mut self, ttl: Duration) -> Result<String> {
        self.pairing.retain(|_, expires| *expires > Instant::now());
        if self.pairing.len() >= 8 {
            bail!("too many pending pairing codes");
        }
        let code = random_secret()?;
        self.pairing.insert(code.clone(), Instant::now() + ttl);
        Ok(code)
    }

    pub fn pair(&mut self, code: &str, label: &str) -> Result<(String, String)> {
        self.pairing.retain(|_, expires| *expires > Instant::now());
        if code.len() > 128 || !self.pairing.contains_key(code) {
            bail!("invalid or expired pairing code");
        }
        if label.is_empty() || label.len() > 128 || label.chars().any(char::is_control) {
            bail!("invalid client label");
        }
        if self.state.clients.len() >= MAX_CLIENTS {
            bail!("client limit reached");
        }
        let id = random_secret()?;
        let token = random_secret()?;
        let token_tag = URL_SAFE_NO_PAD.encode(hmac::sign(&self.key()?, token.as_bytes()).as_ref());
        self.state.clients.insert(
            id.clone(),
            Client {
                label: label.into(),
                created_unix: SystemTime::now().duration_since(UNIX_EPOCH)?.as_secs(),
                token_tag,
            },
        );
        if let Err(error) = self.persist() {
            self.state.clients.remove(&id);
            return Err(error);
        }
        self.pairing.remove(code);
        Ok((id, token))
    }

    pub fn authorized(&self, id: &str, token: &str) -> bool {
        if id.len() > 128 || token.len() > 128 {
            return false;
        }
        let Some(client) = self.state.clients.get(id) else {
            return false;
        };
        let Ok(tag) = URL_SAFE_NO_PAD.decode(&client.token_tag) else {
            return false;
        };
        let Ok(key) = self.key() else {
            return false;
        };
        hmac::verify(&key, token.as_bytes(), &tag).is_ok()
    }

    pub fn clients(&self) -> serde_json::Value {
        serde_json::Value::Array(self.state.clients.iter().map(|(id, client)| serde_json::json!({"client_id":id,"label":client.label,"created_unix":client.created_unix})).collect())
    }

    pub fn revoke(&mut self, id: &str) -> Result<bool> {
        let previous = self.state.clients.remove(id);
        if let Err(error) = self.persist() {
            if let Some(previous) = previous {
                self.state.clients.insert(id.into(), previous);
            }
            return Err(error);
        }
        Ok(previous.is_some())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn pairing_is_single_use_persistent_and_revocable() {
        let directory = tempfile::tempdir().unwrap();
        fs::set_permissions(directory.path(), fs::Permissions::from_mode(0o700)).unwrap();
        let mut store = Store::load(directory.path()).unwrap();
        let code = store.pairing_code(Duration::from_secs(60)).unwrap();
        let (id, token) = store.pair(&code, "Windows 平板").unwrap();
        assert!(store.pair(&code, "second").is_err());
        assert!(store.authorized(&id, &token));
        assert!(!store.authorized(&id, "wrong"));
        let mut reloaded = Store::load(directory.path()).unwrap();
        assert!(reloaded.authorized(&id, &token));
        assert!(!String::from_utf8(
            read_private(&directory.path().join("trust.json"), 65536).unwrap()
        )
        .unwrap()
        .contains(&token));
        assert!(reloaded.revoke(&id).unwrap());
        assert!(!Store::load(directory.path())
            .unwrap()
            .authorized(&id, &token));
    }
    #[test]
    fn expired_codes_and_symlink_state_are_rejected() {
        let directory = tempfile::tempdir().unwrap();
        fs::set_permissions(directory.path(), fs::Permissions::from_mode(0o700)).unwrap();
        let mut store = Store::load(directory.path()).unwrap();
        let code = store.pairing_code(Duration::ZERO).unwrap();
        assert!(store.pair(&code, "test").is_err());
        fs::remove_file(directory.path().join("trust.json")).unwrap();
        std::os::unix::fs::symlink("elsewhere", directory.path().join("trust.json")).unwrap();
        assert!(Store::load(directory.path()).is_err());
    }
}
