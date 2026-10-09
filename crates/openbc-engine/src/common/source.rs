//! Where compare inputs come from.

use std::io::{self, SeekFrom};
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

use async_trait::async_trait;
pub use openbc_compute::media::ReadSeek;
use tokio::io::{AsyncReadExt, AsyncSeekExt};

#[derive(Clone, Copy, Debug)]
pub struct FileStat {
    pub size: u64,
    pub modified: Option<SystemTime>,
}

impl FileStat {
    pub fn modified_unix_ms(&self) -> Option<i64> {
        self.modified.and_then(unix_ms)
    }
}

/// Milliseconds since the Unix epoch (`None` for times before it).
pub fn unix_ms(time: SystemTime) -> Option<i64> {
    time.duration_since(UNIX_EPOCH).ok().map(|d| d.as_millis() as i64)
}

/// A whole local file plus the metadata it was read with.
pub struct LocalRead {
    pub bytes: Vec<u8>,
    pub stat: FileStat,
}

/// Reads a whole local regular file, refusing anything above `max_bytes` before reading it.
/// `stat.size` is the number of bytes actually read.
pub async fn read_local_limited(path: &Path, max_bytes: u64) -> io::Result<LocalRead> {
    let meta = tokio::fs::metadata(path).await?;
    if !meta.is_file() {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, "not a file"));
    }
    if meta.len() > max_bytes {
        return Err(io::Error::new(io::ErrorKind::InvalidInput, format!("file is {} bytes, limit is {max_bytes}", meta.len())));
    }
    let bytes = tokio::fs::read(path).await?;
    let stat = FileStat { size: bytes.len() as u64, modified: meta.modified().ok() };
    Ok(LocalRead { bytes, stat })
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
        Ok(read_local_limited(path, max_bytes).await?.bytes)
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
