//! [`ArchiveAwareVfs`]: lets paths pass through archive files on any provider.

use std::collections::HashMap;
use std::path::{Component, Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use async_trait::async_trait;
use openbc_core::{DirectoryEntry, EntryMetadata, EntryPath};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWriteExt};

use super::{classify_name, ArchiveOptions, ArchiveVfs, Workspace};
use crate::{AsyncVfs, VfsError};

type Signature = (u64, Option<std::time::SystemTime>);

struct Mounted {
    vfs: Arc<ArchiveAwareVfs>,
    sig: Signature,
}

#[derive(Default)]
struct Slot {
    state: tokio::sync::Mutex<Option<Mounted>>,
    last_used: AtomicU64,
}

/// Decorator that resolves `<anything>/x.zip/inner/path` through the archive `x.zip` of the
/// wrapped provider. Paths without an archive file in them are passed straight through (no I/O).
pub struct ArchiveAwareVfs {
    inner: Arc<dyn AsyncVfs>,
    opts: Arc<ArchiveOptions>,
    depth: usize,
    mounts: Mutex<HashMap<PathBuf, Arc<Slot>>>,
    tick: AtomicU64,
}

/// A resolved route into an archive.
struct Route {
    /// Path of the archive file in the wrapped provider's namespace.
    archive: PathBuf,
    /// Path below the archive root.
    rest: EntryPath,
    vfs: Arc<ArchiveAwareVfs>,
}

impl ArchiveAwareVfs {
    #[must_use]
    pub fn new(inner: Arc<dyn AsyncVfs>, opts: ArchiveOptions) -> Self {
        Self::with_depth(inner, Arc::new(opts), 0)
    }

    fn with_depth(inner: Arc<dyn AsyncVfs>, opts: Arc<ArchiveOptions>, depth: usize) -> Self {
        Self { inner, opts, depth, mounts: Mutex::new(HashMap::new()), tick: AtomicU64::new(1) }
    }

    /// Finds the first archive *file* on the path. `list` = the path itself may be the archive
    /// (only `read_dir` descends into it; `stat`/`open_file` of the archive are plain file ops).
    async fn route(&self, path: &EntryPath, list: bool) -> Result<Option<Route>, VfsError> {
        if self.depth >= self.opts.max_nesting {
            return Ok(None);
        }
        let comps: Vec<Component> = path.0.components().collect();
        let mut prefix = PathBuf::new();
        for (i, c) in comps.iter().enumerate() {
            prefix.push(c.as_os_str());
            let Component::Normal(name) = c else { continue };
            if !(i + 1 < comps.len() || list) || !name.to_str().is_some_and(|s| classify_name(s).is_some()) {
                continue;
            }
            // A real directory that merely has an archive-like name is not an archive.
            let Ok(meta) = self.inner.stat(&EntryPath(prefix.clone())).await else { return Ok(None) };
            if meta.is_dir {
                continue;
            }
            let rest: PathBuf = comps[i + 1..].iter().map(|c| c.as_os_str()).collect();
            let vfs = self.mount(&prefix, &meta).await?;
            return Ok(Some(Route { archive: prefix, rest: EntryPath(rest), vfs }));
        }
        Ok(None)
    }

    async fn mount(&self, archive: &Path, meta: &EntryMetadata) -> Result<Arc<ArchiveAwareVfs>, VfsError> {
        let slot = {
            let mut map = self.mounts.lock().unwrap_or_else(|p| p.into_inner());
            if map.len() >= self.opts.max_mounts && !map.contains_key(archive) {
                if let Some(oldest) = map.iter().min_by_key(|(_, s)| s.last_used.load(Ordering::Relaxed)).map(|(k, _)| k.clone()) {
                    map.remove(&oldest);
                }
            }
            Arc::clone(map.entry(archive.to_path_buf()).or_default())
        };
        slot.last_used.store(self.tick.fetch_add(1, Ordering::Relaxed), Ordering::Relaxed);
        let sig: Signature = (meta.size, meta.modified);
        let mut state = slot.state.lock().await;
        if let Some(m) = state.as_ref().filter(|m| m.sig == sig) {
            return Ok(Arc::clone(&m.vfs));
        }
        let opened = self.open_archive(archive, meta).await?;
        let vfs = Arc::new(ArchiveAwareVfs::with_depth(opened, Arc::clone(&self.opts), self.depth + 1));
        *state = Some(Mounted { vfs: Arc::clone(&vfs), sig });
        Ok(vfs)
    }

    async fn open_archive(&self, archive: &Path, meta: &EntryMetadata) -> Result<Arc<ArchiveVfs>, VfsError> {
        let opts = Arc::clone(&self.opts);
        let name = archive.file_name().and_then(|n| n.to_str()).unwrap_or("archive").to_owned();
        let entry = EntryPath(archive.to_path_buf());
        let io = |op: &'static str, e: std::io::Error| VfsError::Io { operation: op, path: archive.to_path_buf(), source: e };
        let workspace = Workspace::new(&opts).map_err(|e| io("open_archive", e))?;
        let local = self.inner.local_path(&entry).filter(|p| p.is_file());
        let file = match local {
            Some(p) => p,
            None => {
                // Remote or nested: copy to a temp file first (formats need a seekable file).
                if meta.size > opts.max_spill_bytes {
                    return Err(io("open_archive", std::io::Error::other(format!("archive is {} MiB, over the {} MiB copy limit", meta.size >> 20, opts.max_spill_bytes >> 20))));
                }
                let target = workspace.path().join("source");
                let mut reader = self.inner.open_file(&entry).await?.take(opts.max_spill_bytes + 1);
                let mut out = tokio::fs::File::create(&target).await.map_err(|e| io("open_archive", e))?;
                let copied = tokio::io::copy(&mut reader, &mut out).await.map_err(|e| io("open_archive", e))?;
                out.flush().await.map_err(|e| io("open_archive", e))?;
                if copied > opts.max_spill_bytes {
                    return Err(io("open_archive", std::io::Error::other("archive is larger than the copy limit")));
                }
                target
            }
        };
        let started = std::time::Instant::now();
        let opened = tokio::task::spawn_blocking(move || ArchiveVfs::open_blocking(&file, &name, workspace, &opts))
            .await
            .map_err(|e| io("open_archive", std::io::Error::other(e)))??;
        openbc_observability::record("openbc-vfs", "open_archive", started.elapsed(), None, None, Some(meta.size), None, Some(archive.display().to_string()));
        Ok(Arc::new(opened))
    }
}

fn read_only(op: &'static str) -> VfsError {
    VfsError::Unsupported(op)
}

#[async_trait]
impl AsyncVfs for ArchiveAwareVfs {
    async fn read_dir(&self, path: &EntryPath) -> Result<Vec<DirectoryEntry>, VfsError> {
        match self.route(path, true).await? {
            None => self.inner.read_dir(path).await,
            Some(r) => {
                let mut entries = r.vfs.read_dir(&r.rest).await?;
                for e in &mut entries {
                    e.path = EntryPath(r.archive.join(&e.path.0));
                }
                Ok(entries)
            }
        }
    }

    async fn stat(&self, path: &EntryPath) -> Result<EntryMetadata, VfsError> {
        match self.route(path, false).await? {
            None => self.inner.stat(path).await,
            Some(r) => r.vfs.stat(&r.rest).await,
        }
    }

    async fn open_file(&self, path: &EntryPath) -> Result<Box<dyn AsyncRead + Unpin + Send>, VfsError> {
        match self.route(path, false).await? {
            None => self.inner.open_file(path).await,
            Some(r) => r.vfs.open_file(&r.rest).await,
        }
    }

    async fn read_file_limited(&self, path: &EntryPath, limit: u64) -> Result<Option<Vec<u8>>, VfsError> {
        match self.route(path, false).await? {
            None => self.inner.read_file_limited(path, limit).await,
            Some(r) => r.vfs.read_file_limited(&r.rest, limit).await,
        }
    }

    fn local_path(&self, path: &EntryPath) -> Option<PathBuf> {
        // Lexical: anything under an archive is not a real file.
        let through_archive = path.0.components().rev().skip(1).any(|c| matches!(c, Component::Normal(n) if n.to_str().is_some_and(|s| classify_name(s).is_some())));
        if through_archive { None } else { self.inner.local_path(path) }
    }

    async fn write_file(&self, path: &EntryPath, bytes: &[u8]) -> Result<(), VfsError> {
        match self.route(path, false).await? {
            None => self.inner.write_file(path, bytes).await,
            Some(r) => {
                r.vfs.stat(&r.rest).await?;
                let Some(local_archive) = self.inner.local_path(&EntryPath(r.archive.clone())) else {
                    return Err(read_only("write_file (only local archive members can be saved)"));
                };
                let container = r.archive.file_name().and_then(|n| n.to_str()).unwrap_or_default();
                if super::classify_name(container).is_none_or(|class| class.container != super::Container::Zip) {
                    return Err(read_only("write_file (only ZIP archive members can be saved)"));
                }
                if super::archive_depth(&r.rest.0) > 0 {
                    return Err(read_only("write_file (nested archive members cannot be saved)"));
                }
                let key = super::key_of(&r.rest).ok_or_else(|| super::not_found("write_file", &path.0))?;
                #[cfg(feature = "archive-zip")]
                {
                    let bytes = bytes.to_vec();
                    tokio::task::spawn_blocking(move || super::backends::replace_zip_entry(&local_archive, &key, &bytes))
                        .await
                        .map_err(|e| VfsError::Io {
                            operation: "write_file",
                            path: path.0.clone(),
                            source: std::io::Error::other(e),
                        })?
                        .map_err(|source| VfsError::Io { operation: "write_file", path: path.0.clone(), source })?;
                    self.mounts.lock().unwrap_or_else(|p| p.into_inner()).remove(&r.archive);
                    Ok(())
                }
                #[cfg(not(feature = "archive-zip"))]
                {
                    let _ = (local_archive, key, bytes);
                    Err(read_only("write_file (this build has no ZIP support)"))
                }
            }
        }
    }

    async fn create_dir(&self, path: &EntryPath) -> Result<(), VfsError> {
        match self.route(path, false).await? {
            None => self.inner.create_dir(path).await,
            Some(_) => Err(read_only("create_dir (archives are read-only)")),
        }
    }

    async fn rename(&self, from: &EntryPath, to: &EntryPath) -> Result<(), VfsError> {
        if self.route(from, false).await?.is_some() || self.route(to, false).await?.is_some() {
            return Err(read_only("rename (archives are read-only)"));
        }
        self.inner.rename(from, to).await
    }

    async fn remove_file(&self, path: &EntryPath) -> Result<(), VfsError> {
        match self.route(path, false).await? {
            None => self.inner.remove_file(path).await,
            Some(_) => Err(read_only("remove_file (archives are read-only)")),
        }
    }

    async fn remove_dir(&self, path: &EntryPath) -> Result<(), VfsError> {
        match self.route(path, false).await? {
            None => self.inner.remove_dir(path).await,
            Some(_) => Err(read_only("remove_dir (archives are read-only)")),
        }
    }
}

#[cfg(all(test, feature = "archive-zip"))]
mod tests {
    use std::io::Write;
    use std::sync::Arc;

    use super::*;
    use crate::LocalVfs;

    #[tokio::test]
    async fn save_rewrites_zip_member_and_refreshes_mounted_archive() {
        let directory = tempfile::tempdir().unwrap();
        let archive_path = directory.path().join("source.zip");
        let file = std::fs::File::create(&archive_path).unwrap();
        let mut writer = zip::ZipWriter::new(file);
        let options = zip::write::SimpleFileOptions::default();
        writer.start_file("src/diff_algorithms.h", options).unwrap();
        writer.write_all(b"old contents").unwrap();
        writer.start_file("keep.txt", options).unwrap();
        writer.write_all(b"keep this").unwrap();
        writer.finish().unwrap();

        let vfs: Arc<dyn AsyncVfs> = Arc::new(LocalVfs::new("/"));
        let vfs = ArchiveAwareVfs::new(vfs, ArchiveOptions::default());
        let member = EntryPath(archive_path.join("src/diff_algorithms.h"));

        assert_eq!(vfs.read_file_limited(&member, 1024).await.unwrap().unwrap(), b"old contents");
        vfs.write_file(&member, b"updated contents").await.unwrap();

        assert_eq!(vfs.read_file_limited(&member, 1024).await.unwrap().unwrap(), b"updated contents");
        let kept = EntryPath(archive_path.join("keep.txt"));
        assert_eq!(vfs.read_file_limited(&kept, 1024).await.unwrap().unwrap(), b"keep this");
    }
}
