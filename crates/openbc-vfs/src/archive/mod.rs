//! Archives as folders.
//!
//! * [`ArchiveVfs`] - one opened archive as an [`AsyncVfs`]. Paths are relative to the archive
//!   root. One backend per format (`backends`), all behind their own cargo feature.
//! * [`ArchiveAwareVfs`] - a decorator for ANY provider (local, SFTP, network, even another
//!   archive) that lets paths pass *through* archive files: `/data/x.zip/src/main.rs` resolves,
//!   and archives nest (`/data/x.zip/inner.7z/a.txt`). Everything above the VFS (folder compare,
//!   text compare, copy out) works unchanged.
//!
//! Contract worth remembering: `stat("x.zip")` is a regular FILE, so copy/delete/rename of the
//! archive itself keep working. Only `read_dir("x.zip")` (and anything below it) enters it. The UI
//! decides when to *show* an archive as a folder (see [`is_archive_name`]).
//!
//! Licence note: the `archive-rar` feature links RARLAB's UnRAR sources (via the `unrar` crate),
//! which are NOT under an OSI-approved licence (extraction is allowed; re-creating the RAR
//! compressor is not). Build without `archive-rar` if you cannot accept that.
//!
//! Resource policy ([`ArchiveOptions`]): local archives are read in place; remote or nested ones
//! are first copied to a temp file (capped). Compressed tarballs, single-file compressions, 7z
//! and rar are expanded once into a per-archive temp folder (capped) because they cannot be read
//! randomly; zip, plain tar, cab and ar are read in place. Temp folders are removed when the
//! archive is closed; a hard crash can leave `openbc-archive-*` folders behind. Save-in-place is
//! supported for members of local ZIP archives; other archive formats remain read-only.

mod aware;
mod backends;
mod index;

pub use aware::ArchiveAwareVfs;

use std::io::Cursor;
use std::path::{Component, Path, PathBuf};
use std::sync::Arc;
use std::time::SystemTime;

use async_trait::async_trait;
use openbc_core::{DirectoryEntry, EntryMetadata, EntryPath};
use tokio::io::AsyncRead;

use crate::{AsyncVfs, VfsError, MAX_SMALL_FILE_BYTES};
use index::{Index, Node};

/// Physical container formats.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Container {
    Zip,
    Tar,
    Gzip,
    Bzip2,
    Xz,
    Zstd,
    Lz4,
    SevenZ,
    Rar,
    Cab,
    Ar,
}

/// What a file name says about being an archive.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct NameClass {
    pub container: Container,
    /// Offer it as a folder in the UI. False for zip-based documents (docx, xlsx, epub, ...):
    /// they still resolve by path, but have their own viewers.
    pub auto_folder: bool,
}

/// Classifies a file name by extension (case-insensitive). Content is sniffed again on open, so
/// a wrong extension only affects whether the name is tried.
pub fn classify_name(name: &str) -> Option<NameClass> {
    use Container::*;
    let n = name.to_ascii_lowercase();
    let ext = n.rsplit_once('.').map(|(_, e)| e)?;
    let (container, auto_folder) = match ext {
        "zip" | "zipx" | "jar" | "war" | "ear" | "apk" | "aar" | "xpi" | "crx" | "whl" | "egg" | "nupkg"
        | "vsix" | "ipa" | "aab" | "jmod" | "mrpack" => (Zip, true),
        "docx" | "docm" | "dotx" | "xlsx" | "xlsm" | "xltx" | "pptx" | "pptm" | "potx" | "odt" | "ods" | "odp"
        | "odg" | "epub" | "3mf" | "kmz" | "xpsx" | "msix" | "appx" => (Zip, false),
        "tar" => (Tar, true),
        "tgz" | "taz" | "tbz" | "tbz2" | "tb2" | "txz" | "tzst" | "tlz4" => (Tar, true),
        "gz" | "gzip" | "svgz" => (Gzip, true),
        "bz2" | "bz" => (Bzip2, true),
        "xz" => (Xz, true),
        "zst" => (Zstd, true),
        "lz4" => (Lz4, true),
        "7z" => (SevenZ, true),
        "rar" => (Rar, true),
        "cab" => (Cab, true),
        "deb" | "ar" => (Ar, true),
        "a" => (Ar, false),
        _ => return None,
    };
    Some(NameClass { container, auto_folder })
}

/// True when the UI should offer this file name as a folder.
pub fn is_archive_name(name: &str) -> bool {
    classify_name(name).is_some_and(|c| c.auto_folder)
}

/// Every extension [`classify_name`] knows with `auto_folder == true` (for filters and docs).
pub fn folder_extensions() -> Vec<&'static str> {
    const ALL: &[&str] = &[
        "zip", "zipx", "jar", "war", "ear", "apk", "aar", "xpi", "crx", "whl", "egg", "nupkg", "vsix", "ipa",
        "aab", "jmod", "mrpack", "tar", "tgz", "taz", "tbz", "tbz2", "tb2", "txz", "tzst", "tlz4", "gz", "gzip",
        "svgz", "bz2", "bz", "xz", "zst", "lz4", "7z", "rar", "cab", "deb", "ar",
    ];
    ALL.iter().copied().filter(|e| is_archive_name(&format!("x.{e}"))).collect()
}

/// Number of path components that look like archive files (a lexical check, no I/O).
pub fn archive_depth(path: &Path) -> usize {
    path.components()
        .filter(|c| matches!(c, Component::Normal(n) if n.to_str().is_some_and(|s| classify_name(s).is_some())))
        .count()
}

/// Limits that keep a hostile or huge archive from exhausting memory or disk.
#[derive(Clone, Debug)]
pub struct ArchiveOptions {
    /// Largest remote/nested archive copied to a temp file.
    pub max_spill_bytes: u64,
    /// Largest total expansion for formats that must be expanded (tar.*, 7z, rar, single-file).
    pub max_expanded_bytes: u64,
    /// Entry count cap (index memory).
    pub max_entries: usize,
    /// Archives inside archives, deepest allowed.
    pub max_nesting: usize,
    /// Archives kept open at once per provider (least recently used are closed).
    pub max_mounts: usize,
    /// Where temp folders go (default: the OS temp dir).
    pub temp_dir: Option<PathBuf>,
}

impl Default for ArchiveOptions {
    fn default() -> Self {
        Self {
            max_spill_bytes: 4 << 30,
            max_expanded_bytes: 8 << 30,
            max_entries: 1_000_000,
            max_nesting: 4,
            max_mounts: 16,
            temp_dir: None,
        }
    }
}

/// Per-archive scratch folder, deleted on drop.
pub(crate) struct Workspace {
    dir: tempfile::TempDir,
}

impl Workspace {
    pub fn new(opts: &ArchiveOptions) -> std::io::Result<Self> {
        let mut b = tempfile::Builder::new();
        b.prefix("openbc-archive-");
        let dir = match &opts.temp_dir {
            Some(base) => b.tempdir_in(base)?,
            None => b.tempdir()?,
        };
        Ok(Self { dir })
    }
    pub fn path(&self) -> &Path {
        self.dir.path()
    }
}

/// Reads one entry's bytes. `Ok(None)` = bigger than `limit`.
pub(crate) trait Backend: Send + Sync {
    fn read(&self, id: usize, limit: u64) -> std::io::Result<Option<Vec<u8>>>;
}

/// One opened archive.
pub struct ArchiveVfs {
    index: Index,
    backend: Box<dyn Backend>,
    modified: Option<SystemTime>,
    skipped: usize,
    _workspace: Workspace,
}

fn io_err(operation: &'static str, path: &Path, source: std::io::Error) -> VfsError {
    VfsError::Io { operation, path: path.to_path_buf(), source }
}

fn not_found(operation: &'static str, path: &Path) -> VfsError {
    io_err(operation, path, std::io::Error::new(std::io::ErrorKind::NotFound, "no such entry in the archive"))
}

/// Archive-relative lookup key: `/a//b` -> `a/b`, root -> "". `..` never resolves.
fn key_of(path: &EntryPath) -> Option<String> {
    let mut parts = Vec::new();
    for c in path.0.components() {
        match c {
            Component::Normal(n) => parts.push(n.to_str()?.to_owned()),
            Component::RootDir | Component::CurDir | Component::Prefix(_) => {}
            Component::ParentDir => return None,
        }
    }
    Some(parts.join("/"))
}

impl ArchiveVfs {
    /// Opens `file` (a real file on disk, owned by `workspace` if it is a spilled copy) and
    /// indexes it. Blocking: call from `spawn_blocking`.
    pub(crate) fn open_blocking(
        file: &Path,
        display_name: &str,
        workspace: Workspace,
        opts: &ArchiveOptions,
    ) -> Result<Self, VfsError> {
        let modified = std::fs::metadata(file).and_then(|m| m.modified()).ok();
        let (index, backend) = backends::open(file, display_name, workspace.path(), opts, modified)
            .map_err(|e| io_err("open_archive", Path::new(display_name), std::io::Error::other(e)))?;
        let skipped = index.skipped;
        Ok(Self { index, backend, modified, skipped, _workspace: workspace })
    }

    /// Entries ignored while indexing (unsafe names, shadowing files).
    pub fn skipped_entries(&self) -> usize {
        self.skipped
    }

    fn meta(&self, node: &Node) -> EntryMetadata {
        EntryMetadata { size: if node.dir { 0 } else { node.size }, modified: node.modified.or(self.modified), is_dir: node.dir, is_link: node.link }
    }

    fn read_entry(&self, path: &EntryPath, limit: u64) -> Result<Option<Vec<u8>>, VfsError> {
        let node = key_of(path).and_then(|k| self.index.get(&k)).ok_or_else(|| not_found("read_file", &path.0))?;
        if node.dir {
            return Err(io_err("read_file", &path.0, std::io::Error::other("is a directory")));
        }
        if node.link {
            return Err(VfsError::Unsupported("reading symbolic links inside archives"));
        }
        let backend: &dyn Backend = self.backend.as_ref();
        backend.read(node.id, limit).map_err(|e| io_err("read_file", &path.0, e))
    }
}

#[async_trait]
impl AsyncVfs for ArchiveVfs {
    async fn read_dir(&self, path: &EntryPath) -> Result<Vec<DirectoryEntry>, VfsError> {
        let key = key_of(path).ok_or_else(|| not_found("read_dir", &path.0))?;
        if !key.is_empty() && !self.index.get(&key).is_some_and(|n| n.dir) {
            return Err(not_found("read_dir", &path.0));
        }
        let kids = self.index.list(&key).unwrap_or_default();
        Ok(kids
            .into_iter()
            .map(|(name, full, node)| DirectoryEntry { path: EntryPath(PathBuf::from(full)), name, metadata: self.meta(node) })
            .collect())
    }

    async fn stat(&self, path: &EntryPath) -> Result<EntryMetadata, VfsError> {
        let key = key_of(path).ok_or_else(|| not_found("stat", &path.0))?;
        if key.is_empty() {
            return Ok(EntryMetadata { size: 0, modified: self.modified, is_dir: true, is_link: false });
        }
        self.index.get(&key).map(|n| self.meta(n)).ok_or_else(|| not_found("stat", &path.0))
    }

    async fn open_file(&self, path: &EntryPath) -> Result<Box<dyn AsyncRead + Unpin + Send>, VfsError> {
        let bytes = self
            .read_entry(path, MAX_SMALL_FILE_BYTES)?
            .ok_or_else(|| io_err("open_file", &path.0, std::io::Error::other("entry is larger than the in-memory limit")))?;
        Ok(Box::new(Cursor::new(bytes)))
    }

    async fn read_file_limited(&self, path: &EntryPath, limit: u64) -> Result<Option<Vec<u8>>, VfsError> {
        self.read_entry(path, limit)
    }

    async fn write_file(&self, _: &EntryPath, _: &[u8]) -> Result<(), VfsError> {
        Err(VfsError::Unsupported("write_file (archives are read-only)"))
    }
    async fn create_dir(&self, _: &EntryPath) -> Result<(), VfsError> {
        Err(VfsError::Unsupported("create_dir (archives are read-only)"))
    }
    async fn rename(&self, _: &EntryPath, _: &EntryPath) -> Result<(), VfsError> {
        Err(VfsError::Unsupported("rename (archives are read-only)"))
    }
    async fn remove_file(&self, _: &EntryPath) -> Result<(), VfsError> {
        Err(VfsError::Unsupported("remove_file (archives are read-only)"))
    }
    async fn remove_dir(&self, _: &EntryPath) -> Result<(), VfsError> {
        Err(VfsError::Unsupported("remove_dir (archives are read-only)"))
    }
}

/// Convenience for callers that want a mounted archive without the decorator.
pub async fn open_archive(file: PathBuf, opts: ArchiveOptions) -> Result<Arc<ArchiveVfs>, VfsError> {
    let name = file.file_name().and_then(|n| n.to_str()).unwrap_or("archive").to_owned();
    tokio::task::spawn_blocking(move || {
        let ws = Workspace::new(&opts).map_err(|e| io_err("open_archive", &file, e))?;
        ArchiveVfs::open_blocking(&file, &name, ws, &opts).map(Arc::new)
    })
    .await
    .map_err(|e| io_err("open_archive", Path::new(""), std::io::Error::other(e)))?
}

#[cfg(test)]
mod name_tests {
    use super::*;

    #[test]
    fn classification() {
        assert!(is_archive_name("a.ZIP") && is_archive_name("x.tar.gz") && is_archive_name("x.tgz"));
        assert!(is_archive_name("lib.jar") && is_archive_name("a.7z") && is_archive_name("a.rar"));
        assert!(!is_archive_name("report.docx"), "documents resolve by path but are not folders");
        assert_eq!(classify_name("report.docx").unwrap().container, Container::Zip);
        for not in ["libfoo.so", "a.dll", "run.exe", "noext", "x.txt", ".gz.bak"] {
            assert!(classify_name(not).is_none(), "{not}");
        }
        assert_eq!(archive_depth(Path::new("/a/x.zip/in/y.7z/f.txt")), 2);
        assert!(folder_extensions().contains(&"7z") && !folder_extensions().contains(&"docx"));
    }
}
