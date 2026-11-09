//! File paths are local to each machine. Exchange bytes and materialize new paths.
use crate::network::Peer;
use anyhow::{bail, Context, Result};
use base64::{engine::general_purpose::STANDARD as B64, Engine};
use percent_encoding::{percent_decode_str, utf8_percent_encode, NON_ALPHANUMERIC};
use serde_json::{json, Value};
use std::{
    collections::BTreeMap,
    fs::{self, File, OpenOptions},
    io::{Read, Write},
    os::unix::fs::OpenOptionsExt,
    path::{Path, PathBuf},
    sync::Arc,
};
const MAX_FILE: u64 = 1024 * 1024 * 1024;

struct Upload {
    file: File,
    path: PathBuf,
    size: u64,
    received: u64,
    complete: bool,
}
pub struct Transfers {
    root: PathBuf,
    uploads: BTreeMap<u64, Upload>,
}

pub fn safe_name(name: &str) -> bool {
    !name.is_empty()
        && name != "."
        && name != ".."
        && name.len() <= 240
        && !name.chars().any(|c| c.is_control() || "/\\:".contains(c))
}
pub fn file_uri(path: &Path) -> Result<String> {
    let text = path
        .to_str()
        .context("non-UTF8 filename is not supported yet")?;
    // Escaping '/' is legal inside a file URI and simplifies exact round trips.
    Ok(format!(
        "file://{}",
        text.split('/')
            .map(|s| utf8_percent_encode(s, NON_ALPHANUMERIC).to_string())
            .collect::<Vec<_>>()
            .join("/")
    ))
}
pub fn uri_path(uri: &str) -> Option<PathBuf> {
    let path = uri.strip_prefix("file://")?;
    if !path.starts_with('/') {
        return None;
    }
    Some(PathBuf::from(
        percent_decode_str(path).decode_utf8().ok()?.as_ref(),
    ))
}

impl Transfers {
    pub fn new(root: PathBuf) -> Result<Self> {
        fs::create_dir_all(&root)?;
        Ok(Self {
            root: fs::canonicalize(root)?,
            uploads: BTreeMap::new(),
        })
    }
    pub fn begin(&mut self, id: u64, name: &str, size: u64) -> Result<()> {
        if !safe_name(name)
            || size > MAX_FILE
            || self.uploads.len() >= 128
            || self.uploads.contains_key(&id)
        {
            bail!("invalid file metadata or transfer limit exceeded");
        }
        let directory = self.root.join(id.to_string());
        fs::create_dir(&directory)?;
        let path = directory.join(name);
        let file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&path)?;
        self.uploads.insert(
            id,
            Upload {
                file,
                path,
                size,
                received: 0,
                complete: false,
            },
        );
        Ok(())
    }
    pub fn chunk(&mut self, id: u64, data: &str) -> Result<()> {
        if data.len() > 90_000 {
            bail!("file chunk too large");
        }
        let bytes = B64.decode(data)?;
        let upload = self.uploads.get_mut(&id).context("unknown transfer")?;
        if upload.complete || upload.received + bytes.len() as u64 > upload.size {
            bail!("file exceeds advertised size");
        }
        upload.file.write_all(&bytes)?;
        upload.received += bytes.len() as u64;
        Ok(())
    }
    pub fn end(&mut self, id: u64) -> Result<()> {
        let upload = self.uploads.get_mut(&id).context("unknown transfer")?;
        if upload.received != upload.size {
            bail!("incomplete file");
        }
        upload.file.flush()?;
        upload.complete = true;
        Ok(())
    }
    pub fn finish(&mut self, ids: &[u64]) -> Result<Vec<u8>> {
        if ids.is_empty()
            || ids.len() > 128
            || ids
                .iter()
                .any(|id| self.uploads.get(id).is_none_or(|u| !u.complete))
        {
            bail!("incomplete batch");
        }
        let uris: Vec<_> = ids
            .iter()
            .map(|id| file_uri(&self.uploads[id].path))
            .collect::<Result<_>>()?;
        for id in ids {
            self.uploads.remove(id);
        }
        Ok((uris.join("\r\n") + "\r\n").into_bytes())
    }
    pub fn cancel(&mut self) {
        for (_, upload) in std::mem::take(&mut self.uploads) {
            let _ = fs::remove_file(upload.path);
        }
    }
}

/// Download after a Linux application offers file URIs. Chunking never loads whole files.
pub fn export(peer: Arc<Peer>, generation: u64, uris: Vec<u8>) {
    std::thread::spawn(move || {
        let run = || -> Result<()> {
            let uris = std::str::from_utf8(&uris)?;
            let paths: Vec<_> = uris
                .lines()
                .filter(|line| !line.starts_with('#') && !line.is_empty())
                .map(|line| uri_path(line).context("only local file URIs are supported"))
                .collect::<Result<_>>()?;
            if paths.is_empty() || paths.len() > 128 {
                bail!("invalid file count");
            }
            let mut files = Vec::new();
            for path in &paths {
                let file = File::open(path)?;
                let metadata = file.metadata()?;
                let name = path
                    .file_name()
                    .and_then(|s| s.to_str())
                    .context("non-UTF8 filename")?;
                if !metadata.is_file() || metadata.len() > MAX_FILE || !safe_name(name) {
                    bail!("unsupported file or file size");
                }
                files.push((name.to_string(), metadata.len(), file));
            }
            let metadata: Vec<Value> = files
                .iter()
                .enumerate()
                .map(|(id, (name, size, _))| json!({"id":id,"name":name,"size":size}))
                .collect();
            if !peer.bulk(json!({"type":"files_begin","batch":generation,"files":metadata})) {
                return Ok(());
            }
            for (id, (_, expected, mut file)) in files.into_iter().enumerate() {
                let mut buffer = vec![0; 65536];
                let mut total = 0;
                loop {
                    let n = file.read(&mut buffer)?;
                    if n == 0 {
                        break;
                    }
                    total += n as u64;
                    if total > expected {
                        bail!("file changed during transfer");
                    }
                    if !peer.bulk(json!({"type":"file_chunk","batch":generation,"file":id,"data":B64.encode(&buffer[..n])})) {return Ok(());}
                }
                if total != expected {
                    bail!("file changed during transfer");
                }
            }
            peer.bulk(json!({"type":"files_done","batch":generation}));
            Ok(())
        };
        if let Err(e) = run() {
            peer.bulk(json!({"type":"files_error","batch":generation,"message":e.to_string()}));
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn reject_names_that_escape_inbox() {
        for name in ["../x", "..", "a/b", "a\\b", "C:x", "", "a\0b"] {
            assert!(!safe_name(name));
        }
        assert!(safe_name("图片 a.png"));
    }
    #[test]
    fn unicode_uri_roundtrip() {
        let p = Path::new("/tmp/图片 #1.png");
        assert_eq!(uri_path(&file_uri(p).unwrap()).unwrap(), p);
        assert!(uri_path("file://other-host/tmp/a").is_none());
    }
}
