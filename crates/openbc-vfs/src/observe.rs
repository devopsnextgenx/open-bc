//! Resource observation: "did the thing a comparison loaded change?"
//!
//! Layers, all in this file:
//! * [`VersionProbe`]: one cheap look at a resource -> [`Observed`]. Implemented for local files
//!   and directory levels ([`LocalProbe`]) and for anything behind [`AsyncVfs`] ([`VfsProbe`]).
//! * [`WatchHub`]: one background watcher per distinct resource, shared by every subscriber
//!   (several tabs on one file cost one watcher) and torn down when the last
//!   [`Subscription`] is dropped.
//!
//! Notifications are only hints. Native filesystem events (feature `native-watch`, local paths
//! only) wake the loop early; the loop then *re-probes*, and a value is published only when the
//! observed state really differs from the previous one. Without native events, or when they
//! cannot be set up, the same loop simply polls.
//!
//! Limits, stated plainly:
//! * Remote providers here (SFTP / FTP / mounted shares) expose size + mtime through
//!   [`AsyncVfs::stat`]; none of them gives an ETag or revision id, so remote detection is
//!   polling of size+mtime every few seconds, never a download. It is not push-based.
//! * `Unavailable` (permissions, network down) is reported as such and never as a change.
//! * Two spellings of one local path (symlink alias) are two watchers.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, Weak};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use async_trait::async_trait;
use openbc_core::resource_version::{Fnv64, Observed, ResourceKind, ResourceVersion};
use openbc_core::EntryPath;
use tokio::io::AsyncReadExt;
use tokio::runtime::Handle;
use tokio::sync::{watch, Notify};
use tokio::task::JoinHandle;

use crate::{AsyncVfs, VfsError};

/// Local files up to this size may be content-hashed to confirm a suspected change.
pub const HASH_LIMIT_BYTES: u64 = 16 * 1024 * 1024;

/// Remote resources are NEVER downloaded to check them: a file is judged by size + modified
/// time (one `stat`), a folder level by the folder's own `stat` plus a listing (attributes come
/// with the listing, no per-file requests). Downloading would cost far more than the check is
/// worth and compete with the viewers for the one shared connection.
///
/// A folder's own mtime changes when entries are added, removed or renamed, but not when a file
/// inside is edited in place. So the cheap `stat` runs every poll and the full listing (which
/// also sees size/mtime changes of the entries) every this many polls, or at once when the
/// folder's own stamp moved.
pub const REMOTE_DIR_LISTING_EVERY: u32 = 6;

/// A single probe taking longer than this is reported as `Unavailable` instead of hanging the
/// watcher (and everything queued behind it on the same connection).
pub const PROBE_TIMEOUT: Duration = Duration::from_secs(45);

/// One look at a resource.
#[async_trait]
pub trait VersionProbe: Send + Sync + 'static {
    /// Current state. `with_hash` asks for a content hash too (files up to [`HASH_LIMIT_BYTES`]).
    async fn probe(&self, with_hash: bool) -> Observed;
    /// Local path, when native notifications can be used for it.
    fn local_path(&self) -> Option<&Path> {
        None
    }
    fn is_directory(&self) -> bool {
        false
    }
}

fn observed_from_io(err: &std::io::Error) -> Observed {
    if err.kind() == std::io::ErrorKind::NotFound {
        Observed::Missing
    } else {
        Observed::Unavailable(err.to_string())
    }
}

fn observed_from_vfs(err: &VfsError) -> Observed {
    match err {
        VfsError::Io { source, .. } => observed_from_io(source),
        VfsError::Unsupported(op) => Observed::Unavailable(format!("{op} is not supported by this provider")),
    }
}

fn opt_ns(time: Option<SystemTime>) -> Option<u128> {
    time.and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map(|d| d.as_nanos())
}

fn time_ns(time: std::io::Result<SystemTime>) -> Option<u128> {
    opt_ns(time.ok())
}

#[cfg(unix)]
fn file_id(meta: &std::fs::Metadata) -> Option<u64> {
    use std::os::unix::fs::MetadataExt;
    Some(meta.ino() ^ meta.dev().rotate_left(48))
}

#[cfg(not(unix))]
fn file_id(_meta: &std::fs::Metadata) -> Option<u64> {
    None
}

fn hash_rows(mut rows: Vec<(String, bool, u64, u128)>) -> ResourceVersion {
    rows.sort();
    let mut hasher = Fnv64::default();
    for (name, is_dir, size, mtime) in &rows {
        hasher.update(name.as_bytes());
        hasher.update(&[0, u8::from(*is_dir)]);
        hasher.update(&size.to_le_bytes());
        hasher.update(&mtime.to_le_bytes());
    }
    ResourceVersion {
        kind: ResourceKind::Directory,
        size: rows.len() as u64,
        modified_ns: None,
        file_id: None,
        content_hash: Some(hasher.finish()),
    }
}

/// Local file or one directory level.
pub struct LocalProbe {
    path: PathBuf,
    directory: bool,
}

impl LocalProbe {
    #[must_use]
    pub fn file(path: impl Into<PathBuf>) -> Self {
        Self { path: path.into(), directory: false }
    }

    #[must_use]
    pub fn directory(path: impl Into<PathBuf>) -> Self {
        Self { path: path.into(), directory: true }
    }

    async fn probe_file(&self, with_hash: bool) -> Observed {
        let meta = match tokio::fs::metadata(&self.path).await {
            Ok(meta) => meta,
            Err(err) => return observed_from_io(&err),
        };
        if meta.is_dir() {
            return Observed::Unavailable("expected a file but found a folder".into());
        }
        let mut version = ResourceVersion {
            kind: ResourceKind::File,
            size: meta.len(),
            modified_ns: time_ns(meta.modified()),
            file_id: file_id(&meta),
            content_hash: None,
        };
        if with_hash && meta.len() <= HASH_LIMIT_BYTES {
            match hash_local_file(&self.path).await {
                Ok(hash) => version.content_hash = Some(hash),
                Err(err) => return observed_from_io(&err),
            }
        }
        Observed::Present(version)
    }

    async fn probe_dir(&self) -> Observed {
        let mut entries = match tokio::fs::read_dir(&self.path).await {
            Ok(entries) => entries,
            Err(err) => return observed_from_io(&err),
        };
        let mut rows = Vec::new();
        loop {
            match entries.next_entry().await {
                Ok(Some(entry)) => {
                    // An entry that vanished between listing and stat is simply skipped.
                    if let Ok(meta) = entry.metadata().await {
                        rows.push((
                            entry.file_name().to_string_lossy().into_owned(),
                            meta.is_dir(),
                            meta.len(),
                            time_ns(meta.modified()).unwrap_or(0),
                        ));
                    }
                }
                Ok(None) => break,
                Err(err) => return observed_from_io(&err),
            }
        }
        Observed::Present(hash_rows(rows))
    }
}

async fn hash_local_file(path: &Path) -> std::io::Result<u64> {
    let mut file = tokio::fs::File::open(path).await?;
    let mut hasher = Fnv64::default();
    let mut buf = vec![0u8; 64 * 1024];
    loop {
        let n = file.read(&mut buf).await?;
        if n == 0 {
            return Ok(hasher.finish());
        }
        hasher.update(&buf[..n]);
    }
}

#[async_trait]
impl VersionProbe for LocalProbe {
    async fn probe(&self, with_hash: bool) -> Observed {
        if self.directory {
            self.probe_dir().await
        } else {
            self.probe_file(with_hash).await
        }
    }

    fn local_path(&self) -> Option<&Path> {
        Some(&self.path)
    }

    fn is_directory(&self) -> bool {
        self.directory
    }
}

/// Any provider behind [`AsyncVfs`] (SFTP, FTP, mounted share, ...): size + mtime via `stat`,
/// listing hash for directory levels, optional content hash for small files.
pub struct VfsProbe {
    vfs: Arc<dyn AsyncVfs>,
    path: EntryPath,
    directory: bool,
    dir: Mutex<DirCache>,
}

/// What the last full listing of a watched folder level looked like.
#[derive(Default)]
struct DirCache {
    polls: u32,
    /// (mtime, size) of the folder entry itself when the listing was taken.
    stamp: Option<(u128, u64)>,
    listing: Option<ResourceVersion>,
}

impl VfsProbe {
    #[must_use]
    pub fn new(vfs: Arc<dyn AsyncVfs>, path: EntryPath, directory: bool) -> Self {
        Self { vfs, path, directory, dir: Mutex::new(DirCache::default()) }
    }

    async fn probe_folder(&self) -> Observed {
        let (listing_due, cached_stamp, cached) = {
            let mut cache = self.dir.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
            cache.polls = cache.polls.wrapping_add(1);
            (
                cache.listing.is_none() || cache.polls % REMOTE_DIR_LISTING_EVERY == 0,
                cache.stamp,
                cache.listing.clone(),
            )
        };
        // Tier 1: one small request for the folder itself.
        let meta = match self.vfs.stat(&self.path).await {
            Ok(meta) => meta,
            Err(err) => return observed_from_vfs(&err),
        };
        if !meta.is_dir {
            return Observed::Unavailable("expected a folder but found a file".into());
        }
        let stamp = opt_ns(meta.modified).map(|mtime| (mtime, meta.size));
        if !listing_due && stamp.is_some() && stamp == cached_stamp {
            if let Some(listing) = cached {
                return Observed::Present(listing); // nothing moved: no listing needed
            }
        }
        // Tier 2: one listing request (names, sizes and mtimes of the entries come with it).
        match self.vfs.read_dir(&self.path).await {
            Ok(entries) => {
                let listing = hash_rows(
                    entries
                        .iter()
                        .map(|e| (e.name.clone(), e.metadata.is_dir, e.metadata.size, opt_ns(e.metadata.modified).unwrap_or(0)))
                        .collect(),
                );
                let mut cache = self.dir.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
                cache.stamp = stamp;
                cache.listing = Some(listing.clone());
                Observed::Present(listing)
            }
            Err(err) => observed_from_vfs(&err),
        }
    }
}

#[async_trait]
impl VersionProbe for VfsProbe {
    /// `_with_hash` is ignored on purpose: remote content is never downloaded for a check.
    async fn probe(&self, _with_hash: bool) -> Observed {
        if self.directory {
            return self.probe_folder().await;
        }
        let meta = match self.vfs.stat(&self.path).await {
            Ok(meta) => meta,
            Err(err) => return observed_from_vfs(&err),
        };
        if meta.is_dir {
            return Observed::Unavailable("expected a file but found a folder".into());
        }
        Observed::Present(ResourceVersion {
            kind: ResourceKind::File,
            size: meta.size,
            modified_ns: opt_ns(meta.modified),
            file_id: None,
            content_hash: None,
        })
    }

    fn is_directory(&self) -> bool {
        self.directory
    }
}

// ----------------------------------------------------------------------------- native hints

#[cfg(feature = "native-watch")]
mod native {
    use std::path::Path;
    use std::sync::Arc;

    use notify::{Event, RecommendedWatcher, RecursiveMode, Watcher};
    use tokio::sync::Notify;

    /// Keeps the OS watcher alive; dropping it stops the watch.
    pub struct Guard(#[allow(dead_code)] RecommendedWatcher);

    /// Watches the *parent* directory (a file watch would be lost when an editor replaces the
    /// file by rename), and only wakes `hint` for events that concern `path`.
    pub fn start(path: &Path, directory: bool, hint: Arc<Notify>) -> Option<Guard> {
        let target = path.to_path_buf();
        let root = if directory {
            target.clone()
        } else {
            match target.parent() {
                Some(p) if !p.as_os_str().is_empty() => p.to_path_buf(),
                _ => std::path::PathBuf::from("."),
            }
        };
        let mut watcher = notify::recommended_watcher(move |res: notify::Result<Event>| {
            if let Ok(event) = res {
                if directory || event.paths.is_empty() || event.paths.iter().any(|p| p == &target) {
                    hint.notify_one();
                }
            } else {
                // The watcher itself reported an error (e.g. overflow): recheck to be safe.
                hint.notify_one();
            }
        })
        .ok()?;
        watcher.watch(&root, RecursiveMode::NonRecursive).ok()?;
        Some(Guard(watcher))
    }
}

#[cfg(not(feature = "native-watch"))]
mod native {
    use std::path::Path;
    use std::sync::Arc;

    use tokio::sync::Notify;

    pub struct Guard;

    pub fn start(_path: &Path, _directory: bool, _hint: Arc<Notify>) -> Option<Guard> {
        None
    }
}

// ----------------------------------------------------------------------------------- the hub

/// Latest published state of a watched resource. `seq` increases only when `state` changes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Observation {
    pub seq: u64,
    pub state: Observed,
}

#[derive(Clone, Debug)]
pub struct WatchConfig {
    /// Poll period for local paths without native notifications.
    pub local_poll: Duration,
    /// Safety-net poll period for local paths that do have native notifications.
    pub local_poll_with_native: Duration,
    /// Poll period for remote providers.
    pub remote_poll: Duration,
    /// Upper bound of the exponential back-off while a resource is `Unavailable`.
    pub max_backoff: Duration,
    /// Quiet time after a notification before re-probing (coalesces bursts of events).
    pub debounce: Duration,
}

impl Default for WatchConfig {
    fn default() -> Self {
        Self {
            local_poll: Duration::from_secs(2),
            local_poll_with_native: Duration::from_secs(10),
            remote_poll: Duration::from_secs(5),
            max_backoff: Duration::from_secs(60),
            debounce: Duration::from_millis(75),
        }
    }
}

struct Entry {
    probe: Arc<dyn VersionProbe>,
    rx: watch::Receiver<Observation>,
    task: JoinHandle<()>,
    _native: Option<native::Guard>,
}

impl Drop for Entry {
    fn drop(&mut self) {
        self.task.abort(); // last subscriber gone: no orphaned poller
    }
}

/// A subscriber's view of one watched resource. Dropping it releases its share of the watcher.
pub struct Subscription {
    entry: Arc<Entry>,
    rx: watch::Receiver<Observation>,
}

impl Subscription {
    fn new(entry: Arc<Entry>) -> Self {
        let mut rx = entry.rx.clone();
        // Start "caught up": `changed()` must only fire for states published AFTER subscribing,
        // otherwise an older published value could be compared against a fresher baseline.
        drop(rx.borrow_and_update());
        Self { entry, rx }
    }

    /// Latest published observation.
    #[must_use]
    pub fn current(&self) -> Observation {
        self.rx.borrow().clone()
    }

    /// Waits for the next published change; `false` when the watcher is gone.
    pub async fn changed(&mut self) -> bool {
        self.rx.changed().await.is_ok()
    }

    /// Probes right now (bypassing the poll period), optionally with a content hash.
    pub async fn probe_now(&self, with_hash: bool) -> Observed {
        probe_with_timeout(self.entry.probe.as_ref(), with_hash).await
    }
}

/// Shares one watcher per distinct resource key.
pub struct WatchHub {
    rt: Handle,
    cfg: WatchConfig,
    entries: Arc<Mutex<HashMap<String, Weak<Entry>>>>,
}

impl WatchHub {
    #[must_use]
    pub fn new(rt: Handle, cfg: WatchConfig) -> Self {
        Self { rt, cfg, entries: Arc::new(Mutex::new(HashMap::new())) }
    }

    /// Number of live watchers (distinct resources with at least one subscriber).
    #[must_use]
    pub fn active_watchers(&self) -> usize {
        let map = self.entries.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        map.values().filter(|w| w.strong_count() > 0).count()
    }

    /// Subscribes to `key`, creating the watcher with `make_probe` only if none is live yet.
    /// `remote` selects the remote poll period and skips native notifications.
    pub fn subscribe(&self, key: &str, remote: bool, make_probe: impl FnOnce() -> Arc<dyn VersionProbe>) -> Subscription {
        let mut map = self.entries.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        map.retain(|_, weak| weak.strong_count() > 0);
        if let Some(entry) = map.get(key).and_then(Weak::upgrade) {
            return Subscription::new(entry);
        }
        let probe = make_probe();
        let hint = Arc::new(Notify::new());
        let guard = match (remote, probe.local_path()) {
            (false, Some(path)) => native::start(path, probe.is_directory(), Arc::clone(&hint)),
            _ => None,
        };
        let period = if remote {
            self.cfg.remote_poll
        } else if guard.is_some() {
            self.cfg.local_poll_with_native
        } else {
            self.cfg.local_poll
        };
        let (tx, rx) = watch::channel(Observation { seq: 0, state: Observed::Unavailable("not probed yet".into()) });
        let task = self.rt.spawn(poll_loop(Arc::clone(&probe), tx, hint, period, self.cfg.debounce, self.cfg.max_backoff));
        let entry = Arc::new(Entry { probe, rx, task, _native: guard });
        map.insert(key.to_owned(), Arc::downgrade(&entry));
        Subscription::new(entry)
    }

    /// Local file. Keyed by absolute path.
    pub fn watch_local(&self, path: &Path, directory: bool) -> Subscription {
        let absolute = if path.is_absolute() {
            path.to_path_buf()
        } else {
            std::env::current_dir().map(|d| d.join(path)).unwrap_or_else(|_| path.to_path_buf())
        };
        let key = format!("local:{}:{}", u8::from(directory), absolute.display());
        self.subscribe(&key, false, || {
            Arc::new(if directory { LocalProbe::directory(absolute.clone()) } else { LocalProbe::file(absolute.clone()) })
        })
    }

    /// A path behind a provider. `session_key` must identify the provider session (e.g. its id).
    pub fn watch_vfs(&self, session_key: &str, vfs: Arc<dyn AsyncVfs>, path: EntryPath, directory: bool) -> Subscription {
        let key = format!("vfs:{session_key}:{}:{}", u8::from(directory), path.0.display());
        self.subscribe(&key, true, || Arc::new(VfsProbe::new(vfs, path, directory)))
    }
}

async fn probe_with_timeout(probe: &dyn VersionProbe, with_hash: bool) -> Observed {
    match tokio::time::timeout(PROBE_TIMEOUT, probe.probe(with_hash)).await {
        Ok(state) => state,
        Err(_) => Observed::Unavailable("the check timed out".into()),
    }
}

async fn poll_loop(
    probe: Arc<dyn VersionProbe>,
    tx: watch::Sender<Observation>,
    hint: Arc<Notify>,
    base: Duration,
    debounce: Duration,
    max_backoff: Duration,
) {
    let mut period = base;
    loop {
        let state = probe_with_timeout(probe.as_ref(), false).await;
        period = if matches!(state, Observed::Unavailable(_)) { (period * 2).min(max_backoff).max(base) } else { base };
        // Publish only a real difference: repeated events for one version never wake subscribers.
        tx.send_if_modified(|current| {
            if current.state == state {
                false
            } else {
                current.seq += 1;
                current.state = state.clone();
                true
            }
        });
        tokio::select! {
            () = tokio::time::sleep(period) => {}
            () = hint.notified() => { tokio::time::sleep(debounce).await; }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp_dir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "openbc-observe-{tag}-{}-{}",
            std::process::id(),
            SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn fast() -> WatchConfig {
        WatchConfig {
            local_poll: Duration::from_millis(20),
            local_poll_with_native: Duration::from_millis(20),
            remote_poll: Duration::from_millis(20),
            max_backoff: Duration::from_millis(80),
            debounce: Duration::from_millis(5),
        }
    }

    async fn wait_until(sub: &mut Subscription, pred: impl Fn(&Observed) -> bool) -> bool {
        let deadline = tokio::time::Instant::now() + Duration::from_secs(5);
        loop {
            if pred(&sub.current().state) {
                return true;
            }
            if tokio::time::timeout_at(deadline, sub.changed()).await.is_err() {
                return false;
            }
        }
    }

    fn present(o: &Observed) -> Option<&ResourceVersion> {
        if let Observed::Present(v) = o { Some(v) } else { None }
    }

    #[tokio::test]
    async fn modification_is_detected() {
        let dir = temp_dir("modify");
        let file = dir.join("a.txt");
        std::fs::write(&file, "one").unwrap();
        let hub = WatchHub::new(Handle::current(), fast());
        let mut sub = hub.watch_local(&file, false);
        assert!(wait_until(&mut sub, |o| present(o).is_some()).await);
        let baseline = present(&sub.current().state).unwrap().clone();
        std::fs::write(&file, "two, longer").unwrap();
        assert!(wait_until(&mut sub, |o| present(o).is_some_and(|v| v.size != baseline.size)).await);
        std::fs::remove_dir_all(dir).ok();
    }

    #[tokio::test]
    async fn atomic_replace_and_delete_are_handled() {
        let dir = temp_dir("atomic");
        let file = dir.join("a.txt");
        std::fs::write(&file, "original").unwrap();
        let hub = WatchHub::new(Handle::current(), fast());
        let mut sub = hub.watch_local(&file, false);
        assert!(wait_until(&mut sub, |o| present(o).is_some()).await);
        let before = present(&sub.current().state).unwrap().clone();
        // editor-style save: write a temp file, rename it over the original
        let tmp = dir.join("a.txt.tmp");
        std::fs::write(&tmp, "replacement!!").unwrap();
        std::fs::rename(&tmp, &file).unwrap();
        assert!(wait_until(&mut sub, |o| present(o).is_some_and(|v| !v.same_stat(&before))).await);
        #[cfg(unix)]
        assert_ne!(present(&sub.current().state).unwrap().file_id, before.file_id);
        std::fs::remove_file(&file).unwrap();
        assert!(wait_until(&mut sub, |o| *o == Observed::Missing).await);
        std::fs::remove_dir_all(dir).ok();
    }

    #[tokio::test]
    async fn unchanged_file_publishes_nothing_more() {
        let dir = temp_dir("quiet");
        let file = dir.join("a.txt");
        std::fs::write(&file, "same").unwrap();
        let hub = WatchHub::new(Handle::current(), fast());
        let mut sub = hub.watch_local(&file, false);
        assert!(wait_until(&mut sub, |o| present(o).is_some()).await);
        let seq = sub.current().seq;
        tokio::time::sleep(Duration::from_millis(250)).await; // ~12 polls
        assert_eq!(sub.current().seq, seq, "polling an unchanged file must not publish");
        std::fs::remove_dir_all(dir).ok();
    }

    #[tokio::test]
    async fn one_watcher_is_shared_and_released() {
        let dir = temp_dir("share");
        let file = dir.join("a.txt");
        std::fs::write(&file, "x").unwrap();
        let hub = WatchHub::new(Handle::current(), fast());
        let a = hub.watch_local(&file, false);
        let b = hub.watch_local(&file, false);
        assert_eq!(hub.active_watchers(), 1);
        drop(a);
        assert_eq!(hub.active_watchers(), 1, "still used by the second subscriber");
        drop(b);
        assert_eq!(hub.active_watchers(), 0, "last subscriber gone: watcher released");
        std::fs::remove_dir_all(dir).ok();
    }

    #[tokio::test]
    async fn directory_listing_change_is_detected() {
        let dir = temp_dir("dir");
        std::fs::write(dir.join("one"), "1").unwrap();
        let hub = WatchHub::new(Handle::current(), fast());
        let mut sub = hub.watch_local(&dir, true);
        assert!(wait_until(&mut sub, |o| present(o).is_some()).await);
        let before = present(&sub.current().state).unwrap().clone();
        std::fs::write(dir.join("two"), "2").unwrap();
        assert!(wait_until(&mut sub, |o| present(o).is_some_and(|v| !v.same_stat(&before))).await);
        std::fs::remove_dir_all(dir).ok();
    }

    /// Provider that counts what a probe costs: remote checks must never open (download) a file.
    struct CountingVfs {
        inner: crate::LocalVfs,
        stats: std::sync::atomic::AtomicUsize,
        lists: std::sync::atomic::AtomicUsize,
        opens: std::sync::atomic::AtomicUsize,
    }

    #[async_trait]
    impl AsyncVfs for CountingVfs {
        async fn read_dir(&self, path: &EntryPath) -> Result<Vec<openbc_core::DirectoryEntry>, VfsError> {
            self.lists.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            self.inner.read_dir(path).await
        }
        async fn stat(&self, path: &EntryPath) -> Result<openbc_core::EntryMetadata, VfsError> {
            self.stats.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            self.inner.stat(path).await
        }
        async fn open_file(&self, path: &EntryPath) -> Result<Box<dyn tokio::io::AsyncRead + Unpin + Send>, VfsError> {
            self.opens.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            self.inner.open_file(path).await
        }
    }

    #[tokio::test]
    async fn remote_checks_never_download_and_folders_list_only_when_needed() {
        use std::sync::atomic::Ordering::SeqCst;
        let dir = temp_dir("remote-cost");
        std::fs::write(dir.join("a.txt"), "1").unwrap();
        let vfs = Arc::new(CountingVfs {
            inner: crate::LocalVfs::new(&dir),
            stats: Default::default(),
            lists: Default::default(),
            opens: Default::default(),
        });

        // A file: stat only, even when a hash is requested (baseline / rebase / confirm).
        let file = VfsProbe::new(vfs.clone(), EntryPath("a.txt".into()), false);
        let before = file.probe(true).await;
        assert!(matches!(&before, Observed::Present(v) if v.content_hash.is_none()));
        assert_eq!(vfs.opens.load(SeqCst), 0, "a remote file is never downloaded for a check");
        std::fs::write(dir.join("a.txt"), "12345").unwrap();
        assert_ne!(file.probe(false).await, before, "size change is seen from metadata alone");

        // A folder level: idle polls are one stat each; the listing runs on the first poll only.
        let folder = VfsProbe::new(vfs.clone(), EntryPath(PathBuf::new()), true);
        vfs.stats.store(0, SeqCst);
        let base = folder.probe(false).await;
        assert_eq!(vfs.lists.load(SeqCst), 1);
        for _ in 0..(REMOTE_DIR_LISTING_EVERY - 3) {
            assert_eq!(folder.probe(false).await, base);
        }
        assert_eq!(vfs.lists.load(SeqCst), 1, "idle folder: stat only, no listing");

        // A new entry moves the folder's own mtime: seen without waiting for the listing tick.
        tokio::time::sleep(Duration::from_millis(50)).await;
        std::fs::write(dir.join("b.txt"), "2").unwrap();
        assert_ne!(folder.probe(false).await, base);

        // An in-place edit does not move the folder's mtime: the periodic listing catches it.
        let settled = folder.probe(false).await;
        tokio::time::sleep(Duration::from_millis(50)).await;
        std::fs::write(dir.join("a.txt"), "a much longer content").unwrap();
        let mut seen = false;
        for _ in 0..REMOTE_DIR_LISTING_EVERY {
            if folder.probe(false).await != settled {
                seen = true;
                break;
            }
        }
        assert!(seen, "an edit inside the folder shows up within one listing period");
        assert_eq!(vfs.opens.load(SeqCst), 0);
        std::fs::remove_dir_all(dir).ok();
    }

    #[tokio::test]
    async fn unreadable_is_reported_unavailable_not_changed() {
        // A directory where a file is expected is not "deleted": it is reported as unavailable.
        let dir = temp_dir("kind");
        let hub = WatchHub::new(Handle::current(), fast());
        let mut sub = hub.watch_local(&dir, false);
        assert!(wait_until(&mut sub, |o| matches!(o, Observed::Unavailable(m) if m.contains("folder"))).await);
        std::fs::remove_dir_all(dir).ok();
    }
}
