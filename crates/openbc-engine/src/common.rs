//! Shared plumbing for the per-tab compare actors (table, registry, hex, media).
//!
//! Every actor follows the image-compare shape: one actor per tab, commands in over a
//! small bounded channel, `*Event`s out over a bounded `tokio::sync::mpsc` channel (so a
//! slow UI back-pressures the actor), and all CPU-bound work on Rayon.

use std::io::{self, SeekFrom};
use std::path::Path;
use std::time::SystemTime;

use async_trait::async_trait;
pub use openbc_compute::media::ReadSeek;
use tokio::io::{AsyncReadExt, AsyncSeekExt};
use tokio::sync::oneshot;

/// Events buffered between an actor and the UI bridge.
pub const EVENT_CHANNEL_CAPACITY: usize = 4;
/// Commands buffered between the UI bridge and an actor (UI uses `try_send`).
pub const COMMAND_CHANNEL_CAPACITY: usize = 16;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Side {
    Left,
    Right,
}

impl Side {
    pub fn index(self) -> usize {
        self as usize
    }
    pub fn label(self) -> &'static str {
        if self == Side::Left { "Left" } else { "Right" }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct FileStat {
    pub size: u64,
    pub modified: Option<SystemTime>,
}

/// Where compare inputs come from. Local today; `AsyncVfs` adapters (remote files are
/// fetched lazily and ranged) implement the same trait later.
#[async_trait]
pub trait ByteSource: Send + Sync + 'static {
    async fn stat(&self, path: &Path) -> io::Result<FileStat>;
    /// Whole file, refusing anything above `max_bytes`.
    async fn read_all(&self, path: &Path, max_bytes: u64) -> io::Result<Vec<u8>>;
    /// Up to `len` bytes at `offset` (shorter at EOF).
    async fn read_range(&self, path: &Path, offset: u64, len: usize) -> io::Result<Vec<u8>>;
    async fn write_all(&self, path: &Path, bytes: &[u8]) -> io::Result<()>;
    /// Blocking seekable reader for header parsing; only call from a Rayon task.
    fn open_reader(&self, path: &Path) -> io::Result<Box<dyn ReadSeek + Send>>;
}

/// Local filesystem source.
pub struct LocalByteSource;

#[async_trait]
impl ByteSource for LocalByteSource {
    async fn stat(&self, path: &Path) -> io::Result<FileStat> {
        let m = tokio::fs::metadata(path).await?;
        Ok(FileStat { size: m.len(), modified: m.modified().ok() })
    }

    async fn read_all(&self, path: &Path, max_bytes: u64) -> io::Result<Vec<u8>> {
        let size = tokio::fs::metadata(path).await?.len();
        if size > max_bytes {
            return Err(io::Error::new(io::ErrorKind::InvalidInput, format!("file is {size} bytes, limit is {max_bytes}")));
        }
        tokio::fs::read(path).await
    }

    async fn read_range(&self, path: &Path, offset: u64, len: usize) -> io::Result<Vec<u8>> {
        let mut file = tokio::fs::File::open(path).await?;
        file.seek(SeekFrom::Start(offset)).await?;
        let mut buf = Vec::with_capacity(len);
        file.take(len as u64).read_to_end(&mut buf).await?;
        Ok(buf)
    }

    async fn write_all(&self, path: &Path, bytes: &[u8]) -> io::Result<()> {
        tokio::fs::write(path, bytes).await
    }

    fn open_reader(&self, path: &Path) -> io::Result<Box<dyn ReadSeek + Send>> {
        Ok(Box::new(std::io::BufReader::new(std::fs::File::open(path)?)))
    }
}

/// A Rayon task panicked or the pool shut down.
#[derive(Debug, thiserror::Error)]
#[error("background worker stopped unexpectedly")]
pub struct TaskLost;

/// Runs CPU-bound `f` on the Rayon pool without blocking a Tokio worker.
pub async fn rayon_run<T: Send + 'static>(f: impl FnOnce() -> T + Send + 'static) -> Result<T, TaskLost> {
    let (tx, rx) = oneshot::channel();
    rayon::spawn(move || {
        let _ = tx.send(f());
    });
    rx.await.map_err(|_| TaskLost)
}

/// Lower-case extension without the dot.
pub fn extension_of(path: &Path) -> String {
    path.extension().and_then(|e| e.to_str()).unwrap_or("").to_ascii_lowercase()
}

/// `path: message` error text (preserves path and operation context).
pub fn context(path: &Path, operation: &str, err: impl std::fmt::Display) -> String {
    format!("{}: {operation}: {err}", path.display())
}
