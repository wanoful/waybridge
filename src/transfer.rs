//! File paths are local to each machine. Exchange bytes and materialize new paths.
use crate::network::Peer;
use anyhow::{bail, Context, Result};
use base64::{engine::general_purpose::STANDARD as B64, Engine};
use percent_encoding::{percent_decode_str, utf8_percent_encode, NON_ALPHANUMERIC};
use serde_json::json;
use std::{
    collections::BTreeMap,
    fs::{self, File, OpenOptions},
    io::Write,
    os::unix::fs::{DirBuilderExt, OpenOptionsExt},
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
    export: Option<crate::readonly_export::Export>,
    uploads: BTreeMap<u64, Upload>,
    trees: BTreeMap<u64, (crate::file_tree::Inbox, Option<u64>)>,
}

/// Protocol IDs can restart when a viewer reconnects. Never overwrite an old
/// successful cache; use a fresh namespace while preserving its published URI.
pub fn batch_directory(root: &Path, name: &str) -> Result<PathBuf> {
    let directory = root.join(name);
    match fs::DirBuilder::new().mode(0o700).create(&directory) {
        Ok(()) => Ok(directory),
        Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => {
            Ok(tempfile::Builder::new()
                .prefix(&format!("{name}-"))
                .tempdir_in(root)?
                .keep())
        }
        Err(error) => Err(error.into()),
    }
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
            export: None,
            root: fs::canonicalize(root)?,
            uploads: BTreeMap::new(),
            trees: BTreeMap::new(),
        })
    }
    fn readonly(&mut self) -> Result<&crate::readonly_export::Export> {
        if self.export.is_none() {
            self.export = Some(crate::readonly_export::Export::new(&self.root)?);
        }
        Ok(self.export.as_ref().unwrap())
    }
    pub fn begin_tree(
        &mut self,
        batch: u64,
        entries: Vec<crate::file_tree::Entry>,
        epoch: Option<u64>,
    ) -> Result<()> {
        if self.trees.len() >= 2 || self.trees.contains_key(&batch) {
            bail!("tree upload limit exceeded");
        }
        let inbox = crate::file_tree::Inbox::new(&self.root, batch, entries)?;
        self.trees.insert(batch, (inbox, epoch));
        Ok(())
    }
    pub fn tree_chunk(&mut self, batch: u64, file: usize, data: &str) -> Result<()> {
        if data.len() > 90000 {
            bail!("tree chunk too large");
        }
        self.trees
            .get_mut(&batch)
            .context("unknown tree upload")?
            .0
            .chunk(file, &B64.decode(data)?)
    }
    pub fn tree_end(&mut self, batch: u64, file: usize) -> Result<()> {
        self.trees
            .get_mut(&batch)
            .context("unknown tree upload")?
            .0
            .end(file)
    }
    pub fn finish_tree(&mut self, batch: u64, epoch: u64, drop: bool) -> Result<Vec<u8>> {
        let (mut inbox, selection) = self.trees.remove(&batch).context("unknown tree upload")?;
        if selection.is_none() != drop || selection.is_some_and(|selection| selection != epoch) {
            bail!("tree upload purpose changed or clipboard superseded");
        }
        let paths = inbox.finish_paths()?;
        let view = self.readonly()?.publish(inbox.directory())?;
        let uris = paths
            .iter()
            .map(|path| file_uri(&view.join(path.strip_prefix(inbox.directory())?)))
            .collect::<Result<Vec<_>>>()?;
        inbox.commit();
        Ok((uris.join("\r\n") + "\r\n").into_bytes())
    }
    pub fn has_tree(&self, batch: u64) -> bool {
        self.trees.contains_key(&batch)
    }
    pub fn cancel_tree(&mut self, batch: u64) {
        self.trees.remove(&batch);
    }
    pub fn begin(&mut self, id: u64, name: &str, size: u64) -> Result<()> {
        if !safe_name(name)
            || size > MAX_FILE
            || self.uploads.len() >= 128
            || self.uploads.contains_key(&id)
        {
            bail!("invalid file metadata or transfer limit exceeded");
        }
        let directory = batch_directory(&self.root, &id.to_string())?;
        let path = directory.join(name);
        let file = match OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&path)
        {
            Ok(file) => file,
            Err(error) => {
                let _ = fs::remove_dir(&directory);
                return Err(error.into());
            }
        };
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
            || ids.iter().collect::<std::collections::BTreeSet<_>>().len() != ids.len()
            || ids
                .iter()
                .any(|id| self.uploads.get(id).is_none_or(|u| !u.complete))
        {
            bail!("incomplete batch");
        }
        let paths = ids
            .iter()
            .map(|id| self.uploads[id].path.clone())
            .collect::<Vec<_>>();
        let directories = paths
            .iter()
            .map(|p| p.parent().context("missing batch directory"))
            .collect::<Result<Vec<_>>>()?;
        let views = self.readonly()?.publish_batch(&directories)?;
        let uris = paths
            .iter()
            .zip(views)
            .map(|(path, view)| file_uri(&view.join(path.file_name().context("missing filename")?)))
            .collect::<Result<Vec<_>>>()?;
        for id in ids {
            self.uploads.remove(id);
        }
        Ok((uris.join("\r\n") + "\r\n").into_bytes())
    }
    pub fn cancel(&mut self) {
        self.trees.clear();
        for (_, upload) in std::mem::take(&mut self.uploads) {
            if let Some(directory) = upload.path.parent() {
                let _ = fs::remove_dir_all(directory);
            }
        }
    }
}

/// Destination-local bytes; publish only complete batches. Superseded workers
/// stop, and a bounded worker count prevents repeated clipboard copies leaking fds.
static EXPORTS: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
pub fn export(peer: Arc<Peer>, generation: u64, uris: Vec<u8>) {
    use std::sync::atomic::Ordering;
    peer.begin_file_export(generation);
    if EXPORTS
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| {
            (n < 4).then_some(n + 1)
        })
        .is_err()
    {
        peer.control(
            json!({"type":"error","message":"File clipboard export is busy; retry the copy"}),
        );
        return;
    }
    std::thread::spawn(move || {
        struct Worker;
        impl Drop for Worker {
            fn drop(&mut self) {
                EXPORTS.fetch_sub(1, Ordering::Relaxed);
            }
        }
        let _worker = Worker;
        let epoch = peer.file_epoch();
        let mut tree = false;
        let mut began = false;
        let mut run = || -> Result<()> {
            let paths = std::str::from_utf8(&uris)?
                .lines()
                .filter(|s| !s.is_empty() && !s.starts_with('#'))
                .take(129)
                .map(|s| uri_path(s).context("only local UTF-8 file URIs are supported"))
                .collect::<Result<Vec<_>>>()?;
            if !peer.file_export_current(generation) {
                return Ok(());
            }
            let source = crate::file_tree::Source::open(paths, peer.file_tree())?;
            tree = source.is_tree() || peer.has_file_lane();
            let begin = if tree {
                json!({"type":"tree_begin","batch":generation,"epoch":epoch,"entries":source.entries})
            } else {
                json!({"type":"files_begin","batch":generation,"epoch":epoch,"files":source.legacy_metadata()})
            };
            if !peer.file_export_current(generation) || !peer.bulk(begin) {
                return Ok(());
            }
            began = true;
            for entry in &source.entries {
                if entry.kind == crate::file_tree::Kind::Directory {
                    continue;
                }
                let mut offset = 0;
                while offset < entry.size {
                    if !peer.file_export_current(generation) {
                        return Ok(());
                    }
                    let bytes = source.read(entry.id, offset, 65536)?;
                    offset += bytes.len() as u64;
                    if !peer.bulk(json!({"type":if tree {"tree_chunk"} else {"file_chunk"},"batch":generation,"file":entry.id,"data":B64.encode(bytes)})) { return Ok(()); }
                }
            }
            if peer.file_export_current(generation) {
                peer.bulk(
                    json!({"type":if tree {"tree_done"} else {"files_done"},"batch":generation}),
                );
            }
            Ok(())
        };
        if let Err(error) = run() {
            if peer.file_export_current(generation) {
                if !began {
                    peer.control(
                        json!({"type":"error","message":format!("File clipboard: {error}")}),
                    );
                } else {
                    peer.bulk(json!({"type":if tree {"tree_error"} else {"files_error"},"batch":generation,"message":error.to_string()}));
                }
            }
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
    fn unavailable_readonly_delivery_never_publishes_or_deletes_saved_copy() {
        let root = tempfile::tempdir().unwrap();
        fs::write(root.path().join("readonly"), b"mount cannot be created").unwrap();
        let saved = root.path().join("user-saved");
        fs::write(&saved, b"independent copy").unwrap();
        let mut transfers = Transfers::new(root.path().to_owned()).unwrap();
        transfers.begin(42, "project.blend", 4).unwrap();
        transfers.chunk(42, "ZGF0YQ==").unwrap();
        transfers.end(42).unwrap();
        assert!(transfers.finish(&[42]).is_err());
        transfers.cancel();
        assert!(!root.path().join("42").exists());
        assert_eq!(fs::read(&saved).unwrap(), b"independent copy");
    }
    #[test]
    fn unicode_uri_roundtrip() {
        let p = Path::new("/tmp/图片 #1.png");
        assert_eq!(uri_path(&file_uri(p).unwrap()).unwrap(), p);
        assert!(uri_path("file://other-host/tmp/a").is_none());
    }
}
