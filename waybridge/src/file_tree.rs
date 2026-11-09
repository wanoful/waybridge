//! One portable tree contract for clipboard, upload, and native outbound drag.
//! Paths are relative, parents precede children, and destinations are fresh trees.
use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use std::{
    collections::{BTreeMap, BTreeSet},
    ffi::CString,
    fs::{self, File, Metadata, OpenOptions},
    io::Write,
    os::{
        fd::{AsRawFd, FromRawFd},
        unix::{
            ffi::OsStrExt,
            fs::{FileExt, MetadataExt, OpenOptionsExt},
        },
    },
    path::{Path, PathBuf},
};

pub const MAX_ENTRIES: usize = 512;
pub const MAX_ROOTS: usize = 128;
pub const MAX_DEPTH: usize = 16;
pub const MAX_FILE: u64 = 1024 * 1024 * 1024;
pub const MAX_TOTAL: u64 = 8 * MAX_FILE;
pub const MAX_PATH: usize = 240;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Kind {
    File,
    Directory,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Entry {
    pub id: usize,
    #[serde(rename = "name")]
    pub path: String,
    pub kind: Kind,
    pub size: u64,
}
pub fn valid_name(name: &str) -> bool {
    let stem = name
        .split('.')
        .next()
        .unwrap_or("")
        .trim_end_matches(' ')
        .to_ascii_uppercase();
    crate::transfer::safe_name(name)
        && name.encode_utf16().count() <= MAX_PATH
        && !name.ends_with(['.', ' '])
        && !name.contains(['<', '>', '"', '|', '?', '*'])
        && !["CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"].contains(&stem.as_str())
        && !["COM", "LPT"].iter().any(|p| {
            stem.strip_prefix(p).is_some_and(|s| {
                ["1", "2", "3", "4", "5", "6", "7", "8", "9", "¹", "²", "³"].contains(&s)
            })
        })
}
pub fn validate(entries: &[Entry]) -> Result<Vec<usize>> {
    if entries.is_empty() || entries.len() > MAX_ENTRIES {
        bail!("invalid tree entry count");
    }
    let mut paths = BTreeMap::new();
    let mut folded = BTreeSet::new();
    let mut roots = Vec::new();
    let mut total = 0_u64;
    for (id, entry) in entries.iter().enumerate() {
        let parts: Vec<_> = entry.path.split('/').collect();
        if entry.id != id
            || parts.len() > MAX_DEPTH
            || parts.iter().any(|s| !valid_name(s))
            || entry.path.encode_utf16().count() > MAX_PATH
            || entry.size > MAX_FILE
            || (entry.kind == Kind::Directory && entry.size != 0)
            || !folded.insert(entry.path.to_uppercase())
        {
            bail!("invalid or colliding tree path");
        }
        total = total
            .checked_add(entry.size)
            .context("tree size overflow")?;
        if total > MAX_TOTAL {
            bail!("tree exceeds total byte limit");
        }
        if let Some((parent, _)) = entry.path.rsplit_once('/') {
            if paths.get(parent) != Some(&Kind::Directory) {
                bail!("missing directory parent");
            }
        } else {
            roots.push(id);
        }
        paths.insert(entry.path.clone(), entry.kind);
    }
    if roots.len() > MAX_ROOTS {
        bail!("too many top-level items");
    }
    Ok(roots)
}
fn same(a: &Metadata, b: &Metadata) -> bool {
    a.dev() == b.dev()
        && a.ino() == b.ino()
        && a.len() == b.len()
        && a.mtime() == b.mtime()
        && a.mtime_nsec() == b.mtime_nsec()
        && a.ctime() == b.ctime()
        && a.ctime_nsec() == b.ctime_nsec()
}
fn open_at(parent: &File, name: &std::ffi::OsStr, flags: i32, mode: u32) -> Result<File> {
    let name = CString::new(name.as_bytes())?;
    let fd = unsafe {
        libc::openat(
            parent.as_raw_fd(),
            name.as_ptr(),
            flags | libc::O_NOFOLLOW | libc::O_CLOEXEC | libc::O_NONBLOCK,
            mode,
        )
    };
    if fd < 0 {
        return Err(std::io::Error::last_os_error().into());
    }
    Ok(unsafe { File::from_raw_fd(fd) })
}
pub struct Source {
    pub entries: Vec<Entry>,
    files: Vec<Option<(File, Metadata)>>,
}
impl Source {
    pub fn open(paths: Vec<PathBuf>, allow_tree: bool) -> Result<Self> {
        if paths.is_empty() || paths.len() > MAX_ROOTS {
            bail!("invalid root count");
        }
        let mut source = Self {
            entries: Vec::new(),
            files: Vec::new(),
        };
        for path in paths {
            let name = path
                .file_name()
                .and_then(|s| s.to_str())
                .context("non-UTF-8 filename")?;
            let file = OpenOptions::new()
                .read(true)
                .custom_flags(libc::O_PATH | libc::O_NOFOLLOW | libc::O_CLOEXEC)
                .open(&path)?;
            source.visit(file, name.to_string(), allow_tree)?;
        }
        validate(&source.entries)?;
        Ok(source)
    }
    fn visit(&mut self, file: File, path: String, allow_tree: bool) -> Result<()> {
        if self.entries.len() >= MAX_ENTRIES
            || path.split('/').count() > MAX_DEPTH
            || path.encode_utf16().count() > MAX_PATH
            || path.split('/').any(|s| !valid_name(s))
        {
            bail!("tree path or entry limit exceeded");
        }
        let metadata = file.metadata()?;
        let kind = if metadata.is_dir() && allow_tree {
            Kind::Directory
        } else if metadata.is_file() {
            Kind::File
        } else {
            bail!("unsupported file type or tree capability");
        };
        let size = if kind == Kind::File {
            metadata.len()
        } else {
            0
        };
        if size > MAX_FILE {
            bail!("file exceeds 1 GiB");
        }
        self.entries.push(Entry {
            id: self.entries.len(),
            path: path.clone(),
            kind,
            size,
        });
        if kind == Kind::File {
            // O_PATH inspects special files without opening a device/FIFO.
            // Reopen only a verified regular inode through its pinned fd.
            let readable = File::open(format!("/proc/self/fd/{}", file.as_raw_fd()))?;
            self.files.push(Some((readable, metadata)));
            return Ok(());
        }
        self.files.push(None);
        // Enumerate through the pinned directory, then open each child relative
        // to its fd. Concurrent rename/reparse cannot escape into another tree.
        let mut names = fs::read_dir(format!("/proc/self/fd/{}", file.as_raw_fd()))?
            .take(MAX_ENTRIES + 1)
            .map(|entry| entry.map(|e| e.file_name()))
            .collect::<std::io::Result<Vec<_>>>()?;
        if names.len() > MAX_ENTRIES {
            bail!("directory too large");
        }
        names.sort();
        for name in names {
            let text = name.to_str().context("non-UTF-8 filename")?;
            if !valid_name(text) {
                bail!("unsupported Windows filename");
            }
            let child = open_at(&file, &name, libc::O_PATH, 0)?;
            self.visit(child, format!("{path}/{text}"), allow_tree)?;
        }
        Ok(())
    }
    pub fn is_tree(&self) -> bool {
        self.entries.iter().any(|e| e.kind == Kind::Directory)
    }
    pub fn legacy_metadata(&self) -> Vec<serde_json::Value> {
        self.entries
            .iter()
            .map(|e| serde_json::json!({"id":e.id,"name":e.path,"size":e.size}))
            .collect()
    }
    pub fn read(&self, id: usize, offset: u64, length: usize) -> Result<Vec<u8>> {
        if length == 0 || length > 65536 {
            bail!("invalid range length");
        }
        let (file, original) = self
            .files
            .get(id)
            .and_then(Option::as_ref)
            .context("not a file")?;
        if offset > original.len() || !same(original, &file.metadata()?) {
            bail!("source file changed");
        }
        let length = length.min((original.len() - offset) as usize);
        let mut bytes = vec![0; length];
        let mut n = 0;
        while n < length {
            let count = file.read_at(&mut bytes[n..], offset + n as u64)?;
            if count == 0 {
                bail!("source file truncated");
            }
            n += count;
        }
        if !same(original, &file.metadata()?) {
            bail!("source file changed");
        }
        Ok(bytes)
    }
}

struct FileUpload {
    file: File,
    remaining: u64,
    complete: bool,
}
pub struct Inbox {
    directory: PathBuf,
    entries: Vec<Entry>,
    files: BTreeMap<usize, FileUpload>,
    committed: bool,
}
impl Drop for Inbox {
    fn drop(&mut self) {
        if !self.committed {
            let _ = fs::remove_dir_all(&self.directory);
        }
    }
}
impl Inbox {
    pub fn new(root: &Path, batch: u64, entries: Vec<Entry>) -> Result<Self> {
        validate(&entries)?;
        let directory = root.join(format!("tree-{batch}"));
        fs::DirBuilder::new().mode(0o700).create(&directory)?;
        let mut inbox = Self {
            directory,
            entries,
            files: BTreeMap::new(),
            committed: false,
        };
        let mut directories = BTreeMap::from([(String::new(), File::open(&inbox.directory)?)]);
        for entry in &inbox.entries {
            let (parent, name) = entry.path.rsplit_once('/').unwrap_or(("", &entry.path));
            let parent = &directories[parent];
            let name = std::ffi::OsStr::new(name);
            if entry.kind == Kind::Directory {
                let bytes = CString::new(name.as_bytes())?;
                if unsafe { libc::mkdirat(parent.as_raw_fd(), bytes.as_ptr(), 0o700) } != 0 {
                    return Err(std::io::Error::last_os_error().into());
                }
                let child = open_at(parent, name, libc::O_RDONLY | libc::O_DIRECTORY, 0)?;
                directories.insert(entry.path.clone(), child);
            } else {
                let file = open_at(
                    parent,
                    name,
                    libc::O_WRONLY | libc::O_CREAT | libc::O_EXCL,
                    0o600,
                )?;
                inbox.files.insert(
                    entry.id,
                    FileUpload {
                        file,
                        remaining: entry.size,
                        complete: false,
                    },
                );
            }
        }
        Ok(inbox)
    }
    pub fn chunk(&mut self, id: usize, bytes: &[u8]) -> Result<()> {
        let file = self.files.get_mut(&id).context("unknown tree file")?;
        if file.complete || bytes.len() > 65536 || bytes.len() as u64 > file.remaining {
            bail!("invalid tree chunk");
        }
        file.file.write_all(bytes)?;
        file.remaining -= bytes.len() as u64;
        Ok(())
    }
    pub fn end(&mut self, id: usize) -> Result<()> {
        let file = self.files.get_mut(&id).context("unknown tree file")?;
        if file.complete || file.remaining != 0 {
            bail!("incomplete tree file");
        }
        file.file.flush()?;
        file.complete = true;
        Ok(())
    }
    pub fn finish(&mut self) -> Result<Vec<u8>> {
        if self.files.values().any(|file| !file.complete) {
            bail!("incomplete tree");
        }
        let roots = validate(&self.entries)?;
        let uris = roots
            .into_iter()
            .map(|id| crate::transfer::file_uri(&self.directory.join(&self.entries[id].path)))
            .collect::<Result<Vec<_>>>()?;
        self.files.clear();
        self.committed = true;
        Ok((uris.join("\r\n") + "\r\n").into_bytes())
    }
}
use std::os::unix::fs::DirBuilderExt;

#[cfg(test)]
mod tests {
    use super::*;
    struct Temp(PathBuf);
    impl Temp {
        fn new() -> Self {
            static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
            let path = std::env::temp_dir().join(format!(
                "waybridge-tree-unit-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
            ));
            fs::create_dir(&path).unwrap();
            Self(path)
        }
    }
    impl Drop for Temp {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
    fn e(id: usize, name: &str, kind: Kind, size: u64) -> Entry {
        Entry {
            id,
            path: name.into(),
            kind,
            size,
        }
    }
    #[test]
    fn manifest_rejects_escape_devices_collisions_and_limits() {
        for name in [
            "../escape",
            "/absolute",
            "C:x",
            "r\\x",
            "CON",
            "con .txt",
            "LPT1",
            "x.",
            "x ",
            "a:b",
            "x//y",
            "x/../y",
            "x/./y",
            "x\0y",
        ] {
            assert!(validate(&[e(0, name, Kind::File, 0)]).is_err(), "{name}");
        }
        for entries in [
            vec![e(0, "r", Kind::Directory, 1)],
            vec![e(1, "x", Kind::File, 0)],
            vec![e(0, "r/x", Kind::File, 0)],
            vec![e(0, "r", Kind::File, 0), e(1, "r/x", Kind::File, 0)],
            vec![e(0, "A", Kind::File, 0), e(1, "a", Kind::File, 0)],
            vec![e(0, "r", Kind::Directory, 0), e(1, "R/x", Kind::File, 0)],
            vec![e(0, "x", Kind::File, MAX_FILE + 1)],
        ] {
            assert!(validate(&entries).is_err());
        }
        assert!(validate(
            &(0..129)
                .map(|i| e(i, &format!("f{i}"), Kind::File, 0))
                .collect::<Vec<_>>()
        )
        .is_err());
        let total = (0..9)
            .map(|i| e(i, &format!("f{i}"), Kind::File, MAX_FILE))
            .collect::<Vec<_>>();
        assert!(validate(&total).is_err());
        let deep = (0..17)
            .map(|i| {
                e(
                    i,
                    &std::iter::repeat_n("d", i + 1)
                        .collect::<Vec<_>>()
                        .join("/"),
                    Kind::Directory,
                    0,
                )
            })
            .collect::<Vec<_>>();
        assert!(validate(&deep).is_err());
        let mut many = vec![e(0, "r", Kind::Directory, 0)];
        many.extend((1..=MAX_ENTRIES).map(|i| e(i, &format!("r/f{i}"), Kind::File, 0)));
        assert!(validate(&many).is_err());
    }
    #[test]
    fn real_tree_preserves_empty_directories_and_same_basename() {
        let temp = Temp::new();
        let root = temp.0.join("树");
        fs::create_dir_all(root.join("empty")).unwrap();
        for dir in ["a", "b"] {
            fs::create_dir(root.join(dir)).unwrap();
            fs::write(root.join(dir).join("same.txt"), dir.as_bytes()).unwrap();
        }
        fs::write(root.join("零.txt"), []).unwrap();
        let source = Source::open(vec![root.clone()], true).unwrap();
        assert!(source.is_tree());
        assert_eq!(source.entries.len(), 7);
        assert!(Source::open(vec![root], false).is_err());
        let inbox_root = temp.0.join("inbox");
        fs::create_dir(&inbox_root).unwrap();
        let mut inbox = Inbox::new(&inbox_root, 1, source.entries.clone()).unwrap();
        for entry in &source.entries {
            if entry.kind == Kind::File {
                if entry.size > 0 {
                    inbox
                        .chunk(entry.id, &source.read(entry.id, 0, 65536).unwrap())
                        .unwrap();
                }
                inbox.end(entry.id).unwrap();
            }
        }
        let uris = inbox.finish().unwrap();
        drop(inbox);
        let destination =
            crate::transfer::uri_path(std::str::from_utf8(&uris).unwrap().trim()).unwrap();
        assert!(destination.join("empty").is_dir());
        assert_eq!(fs::read(destination.join("a/same.txt")).unwrap(), b"a");
        assert_eq!(fs::read(destination.join("b/same.txt")).unwrap(), b"b");
        assert_eq!(fs::metadata(destination.join("零.txt")).unwrap().len(), 0);
        assert!(destination.exists());
        let partial = Inbox::new(&inbox_root, 2, source.entries).unwrap();
        drop(partial);
        assert!(!inbox_root.join("tree-2").exists());
    }
    #[test]
    fn never_follows_symlinks_or_blocks_on_special_files() {
        let temp = Temp::new();
        fs::write(temp.0.join("outside"), b"secret").unwrap();
        let root = temp.0.join("root");
        fs::create_dir(&root).unwrap();
        std::os::unix::fs::symlink(temp.0.join("outside"), root.join("link")).unwrap();
        assert!(Source::open(vec![root.clone()], true).is_err());
        fs::remove_file(root.join("link")).unwrap();
        let fifo = CString::new(root.join("fifo").as_os_str().as_bytes()).unwrap();
        assert_eq!(unsafe { libc::mkfifo(fifo.as_ptr(), 0o600) }, 0);
        assert!(Source::open(vec![root], true).is_err());
        assert!(Source::open(vec![PathBuf::from("/dev/null")], true).is_err());
        assert_eq!(fs::read(temp.0.join("outside")).unwrap(), b"secret");
    }
    #[test]
    fn incomplete_and_failed_inbox_cleanup_never_removes_existing_data() {
        let temp = Temp::new();
        let entries = vec![
            e(0, "root", Kind::Directory, 0),
            e(1, "root/a", Kind::File, 3),
        ];
        {
            let mut inbox = Inbox::new(&temp.0, 1, entries.clone()).unwrap();
            assert!(inbox.chunk(1, b"long").is_err());
            assert!(inbox.finish().is_err());
        }
        assert!(!temp.0.join("tree-1").exists());
        fs::create_dir(temp.0.join("tree-2")).unwrap();
        fs::write(temp.0.join("tree-2/keep"), b"keep").unwrap();
        assert!(Inbox::new(&temp.0, 2, entries).is_err());
        assert_eq!(fs::read(temp.0.join("tree-2/keep")).unwrap(), b"keep");
    }
}
