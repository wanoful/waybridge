//! A read-only filesystem view of completed receive batches. Modes describe
//! ordinary user-owned files so copies acquire normal destination permissions;
//! the mount, rather than a copied read-only bit, enforces delivery semantics.
use anyhow::{bail, Context, Result};
use fuser::{
    BackgroundSession, FileAttr, FileType, Filesystem, MountOption, ReplyAttr, ReplyData,
    ReplyDirectory, ReplyEmpty, ReplyEntry, ReplyOpen, Request,
};
use std::{
    collections::BTreeMap,
    ffi::{OsStr, OsString},
    fs::{self, File},
    os::unix::{
        ffi::OsStrExt,
        fs::{FileExt, MetadataExt},
    },
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
    time::{Duration, SystemTime},
};

const TTL: Duration = Duration::ZERO;
struct Node {
    attr: FileAttr,
    parent: u64,
    children: BTreeMap<OsString, u64>,
    file: Option<File>,
}
#[derive(Default)]
struct Tree {
    nodes: BTreeMap<u64, Node>,
}
impl Tree {
    fn add(&mut self, path: &Path, parent: u64) -> Result<u64> {
        let meta = fs::symlink_metadata(path)?;
        if !meta.is_dir() && !meta.is_file() {
            bail!("read-only delivery accepts only ordinary files and directories");
        }
        let ino = self.nodes.len() as u64 + 1;
        let kind = if meta.is_dir() {
            FileType::Directory
        } else {
            FileType::RegularFile
        };
        self.nodes.insert(
            ino,
            Node {
                parent,
                children: BTreeMap::new(),
                file: if meta.is_file() {
                    Some(File::open(path)?)
                } else {
                    None
                },
                attr: FileAttr {
                    ino,
                    size: meta.len(),
                    blocks: meta.blocks(),
                    atime: meta.accessed().unwrap_or(SystemTime::UNIX_EPOCH),
                    mtime: meta.modified().unwrap_or(SystemTime::UNIX_EPOCH),
                    ctime: meta.modified().unwrap_or(SystemTime::UNIX_EPOCH),
                    crtime: SystemTime::UNIX_EPOCH,
                    kind,
                    perm: if meta.is_dir() { 0o700 } else { 0o600 },
                    nlink: if meta.is_dir() { 2 } else { 1 },
                    uid: meta.uid(),
                    gid: meta.gid(),
                    rdev: 0,
                    blksize: 4096,
                    flags: 0,
                },
            },
        );
        if meta.is_dir() {
            for child in fs::read_dir(path)? {
                let child = child?;
                let id = self.add(&child.path(), ino)?;
                self.nodes
                    .get_mut(&ino)
                    .unwrap()
                    .children
                    .insert(child.file_name(), id);
            }
        }
        Ok(ino)
    }
}
struct View(Arc<Mutex<Tree>>);
impl Filesystem for View {
    fn lookup(&mut self, _: &Request<'_>, parent: u64, name: &OsStr, reply: ReplyEntry) {
        let tree = self.0.lock().unwrap();
        if let Some(node) = tree
            .nodes
            .get(&parent)
            .and_then(|p| p.children.get(name))
            .and_then(|id| tree.nodes.get(id))
        {
            reply.entry(&TTL, &node.attr, 0);
        } else {
            reply.error(libc::ENOENT);
        }
    }
    fn getattr(&mut self, _: &Request<'_>, ino: u64, _: Option<u64>, reply: ReplyAttr) {
        let tree = self.0.lock().unwrap();
        if let Some(node) = tree.nodes.get(&ino) {
            reply.attr(&TTL, &node.attr);
        } else {
            reply.error(libc::ENOENT);
        }
    }
    fn open(&mut self, _: &Request<'_>, ino: u64, flags: i32, reply: ReplyOpen) {
        if flags & libc::O_ACCMODE != libc::O_RDONLY || flags & libc::O_TRUNC != 0 {
            reply.error(libc::EROFS);
        } else if self
            .0
            .lock()
            .unwrap()
            .nodes
            .get(&ino)
            .is_some_and(|n| n.file.is_some())
        {
            reply.opened(0, 0);
        } else {
            reply.error(libc::EISDIR);
        }
    }
    fn read(
        &mut self,
        _: &Request<'_>,
        ino: u64,
        _: u64,
        offset: i64,
        size: u32,
        _: i32,
        _: Option<u64>,
        reply: ReplyData,
    ) {
        let tree = self.0.lock().unwrap();
        let Some(file) = tree.nodes.get(&ino).and_then(|n| n.file.as_ref()) else {
            reply.error(libc::ENOENT);
            return;
        };
        if offset < 0 {
            reply.error(libc::EINVAL);
            return;
        }
        let mut bytes = vec![0; size as usize];
        match file.read_at(&mut bytes, offset as u64) {
            Ok(n) => reply.data(&bytes[..n]),
            Err(e) => reply.error(e.raw_os_error().unwrap_or(libc::EIO)),
        }
    }
    fn readdir(
        &mut self,
        _: &Request<'_>,
        ino: u64,
        _: u64,
        offset: i64,
        mut reply: ReplyDirectory,
    ) {
        let tree = self.0.lock().unwrap();
        let Some(node) = tree.nodes.get(&ino) else {
            reply.error(libc::ENOENT);
            return;
        };
        if offset < 0 {
            reply.error(libc::EINVAL);
            return;
        }
        let items = [
            (ino, FileType::Directory, OsStr::new(".")),
            (node.parent, FileType::Directory, OsStr::new("..")),
        ]
        .into_iter()
        .chain(
            node.children
                .iter()
                .map(|(name, id)| (*id, tree.nodes[id].attr.kind, name.as_os_str())),
        );
        for (i, (id, kind, name)) in items.enumerate().skip(offset as usize) {
            if reply.add(id, (i + 1) as i64, kind, name) {
                break;
            }
        }
        reply.ok();
    }
    fn access(&mut self, _: &Request<'_>, ino: u64, mask: i32, reply: ReplyEmpty) {
        if mask & libc::W_OK != 0 {
            reply.error(libc::EROFS);
        } else if self.0.lock().unwrap().nodes.contains_key(&ino) {
            reply.ok();
        } else {
            reply.error(libc::ENOENT);
        }
    }
}

pub struct Export {
    view: PathBuf,
    tree: Arc<Mutex<Tree>>,
    _session: BackgroundSession,
}
impl Export {
    pub fn new(root: &Path) -> Result<Self> {
        let view = root.join("readonly");
        fs::create_dir_all(&view)?;
        let mut tree = Tree::default();
        // The root is synthetic: pending receive objects never enter this view.
        tree.add(&view, 1)?;
        let tree = Arc::new(Mutex::new(tree));
        let session = fuser::spawn_mount2(View(tree.clone()), &view, &[
            MountOption::RO, MountOption::NoExec, MountOption::NoDev, MountOption::NoSuid,
            MountOption::FSName("waybridge-delivery".into()),
        ]).context("Cannot mount read-only file delivery; install fusermount3 and allow /dev/fuse (writable delivery is disabled)")?;
        Ok(Self {
            view,
            tree,
            _session: session,
        })
    }
    /// Register only a complete private batch. Keep file descriptors pinned for
    /// random reads and reopening until session shutdown, across viewer detach.
    pub fn publish(&self, directory: &Path) -> Result<PathBuf> {
        Ok(self.publish_batch(&[directory])?.remove(0))
    }
    pub fn publish_batch(&self, directories: &[&Path]) -> Result<Vec<PathBuf>> {
        let mut tree = self.tree.lock().unwrap();
        let mut names = std::collections::BTreeSet::new();
        for directory in directories {
            let name = directory
                .file_name()
                .context("missing delivery batch name")?;
            if name.as_bytes().is_empty()
                || !names.insert(name.to_os_string())
                || tree.nodes[&1].children.contains_key(name)
            {
                bail!("delivery batch already published or duplicated");
            }
        }
        // No root entry becomes visible until every file in this commit opens.
        let first = tree.nodes.len() as u64 + 1;
        let staged = directories
            .iter()
            .map(|directory| {
                let id = tree.add(directory, 1)?;
                Ok((directory.file_name().unwrap().to_os_string(), id))
            })
            .collect::<Result<Vec<_>>>();
        match staged {
            Ok(items) => {
                let paths = items.iter().map(|(name, _)| self.view.join(name)).collect();
                tree.nodes.get_mut(&1).unwrap().children.extend(items);
                Ok(paths)
            }
            Err(error) => {
                tree.nodes.retain(|id, _| *id < first);
                Err(error)
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    #[ignore = "requires real /dev/fuse and fusermount3; run explicitly in filesystem acceptance"]
    fn readonly_delivery_preserves_writable_copy_semantics() {
        let temp = tempfile::tempdir().unwrap();
        let batch = temp.path().join("batch");
        fs::create_dir_all(batch.join("nested/empty")).unwrap();
        fs::write(batch.join("nested/project.blend"), b"original project").unwrap();
        fs::write(batch.join("nested/other.blend"), b"other project").unwrap();
        let export = Export::new(temp.path()).unwrap();
        let view = export.publish(&batch).unwrap();
        // Cross-reconnect reuse cannot overwrite an already published batch.
        assert!(export.publish(&batch).is_err());
        let failed = temp.path().join("failed");
        fs::create_dir(&failed).unwrap();
        fs::write(failed.join("readable"), b"partial").unwrap();
        std::os::unix::fs::symlink(&batch, failed.join("unsupported-link")).unwrap();
        let node_count = export.tree.lock().unwrap().nodes.len();
        let descriptors = fs::read_dir("/proc/self/fd").unwrap().count();
        let good = temp.path().join("would-be-partial");
        fs::create_dir(&good).unwrap();
        fs::write(good.join("file"), b"staged but unpublished").unwrap();
        assert!(export.publish_batch(&[&good, &failed]).is_err());
        assert!(!export.view.join("would-be-partial").exists());
        assert!(export.publish(&failed).is_err());
        assert_eq!(export.tree.lock().unwrap().nodes.len(), node_count);
        assert_eq!(fs::read_dir("/proc/self/fd").unwrap().count(), descriptors);
        assert!(!export.view.join("failed").exists());
        fs::remove_file(failed.join("unsupported-link")).unwrap();
        assert!(export.publish(&failed).unwrap().join("readable").is_file());
        let file = view.join("nested/project.blend");
        assert_eq!(fs::read(&file).unwrap(), b"original project");
        assert_eq!(
            fs::write(&file, b"changed").unwrap_err().raw_os_error(),
            Some(libc::EROFS)
        );
        assert_eq!(
            fs::write(view.join("nested/replace"), b"replace")
                .unwrap_err()
                .raw_os_error(),
            Some(libc::EROFS)
        );
        let replacement = temp.path().join("replace");
        fs::write(&replacement, b"replace").unwrap();
        assert!(fs::rename(&replacement, &file).is_err());
        assert_eq!(
            fs::rename(view.join("nested/other.blend"), &file)
                .unwrap_err()
                .raw_os_error(),
            Some(libc::EROFS)
        );
        assert_eq!(
            fs::remove_file(&file).unwrap_err().raw_os_error(),
            Some(libc::EROFS)
        );
        // cp -a is deliberately stricter than ordinary file-manager Copy: even
        // preserving the advertised mode must produce an editable independent copy.
        let saved = temp.path().join("saved");
        assert!(std::process::Command::new("cp")
            .arg("-a")
            .arg(&view)
            .arg(&saved)
            .status()
            .unwrap()
            .success());
        fs::write(saved.join("nested/project.blend"), b"saved edit").unwrap();
        fs::write(saved.join("nested/new"), b"new").unwrap();
        assert!(saved.join("nested/empty").is_dir());
        assert_eq!(
            fs::read(batch.join("nested/project.blend")).unwrap(),
            b"original project"
        );
        // Removing a canceled unrelated batch must leave the successful view/copy.
        fs::create_dir(temp.path().join("partial")).unwrap();
        fs::remove_dir_all(temp.path().join("partial")).unwrap();
        assert_eq!(fs::read(&file).unwrap(), b"original project");
        drop(export);
        assert_eq!(
            fs::read(saved.join("nested/project.blend")).unwrap(),
            b"saved edit"
        );
    }
}
