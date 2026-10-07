use std::future::Future;
use std::io;
use std::pin::Pin;
use std::time::UNIX_EPOCH;

pub type SourceFuture<'a, T> = Pin<Box<dyn Future<Output = T> + Send + 'a>>;

pub struct SourceRead {
    pub bytes: Vec<u8>,
    pub size_bytes: u64,
    pub modified_unix_ms: Option<i64>,
}

/// Whole-file byte access, bounded by `max_bytes`. Implement this over
/// `AsyncVfs` to compare remote images; remote files are streamed into the
/// bounded buffer, never memory-mapped. Example adapter shape:
///
/// ```ignore
/// impl ImageByteSource for VfsSource {
///     fn read<'a>(&'a self, path: &'a str, max: u64) -> SourceFuture<'a, io::Result<SourceRead>> {
///         Box::pin(async move {
///             let meta = self.vfs.metadata(&self.parse(path)?).await.map_err(to_io)?;
///             if meta.len > max { return Err(io::Error::other("file too large")); }
///             let bytes = self.vfs.read_all(&self.parse(path)?, max).await.map_err(to_io)?;
///             Ok(SourceRead { size_bytes: bytes.len() as u64, modified_unix_ms: meta.modified_ms(), bytes })
///         })
///     }
/// }
/// ```
pub trait ImageByteSource: Send + Sync + 'static {
    fn read<'a>(&'a self, path: &'a str, max_bytes: u64) -> SourceFuture<'a, io::Result<SourceRead>>;
}

#[derive(Clone, Copy, Debug, Default)]
pub struct LocalFsSource;

impl ImageByteSource for LocalFsSource {
    fn read<'a>(&'a self, path: &'a str, max_bytes: u64) -> SourceFuture<'a, io::Result<SourceRead>> {
        Box::pin(async move {
            let meta = tokio::fs::metadata(path).await?;
            if !meta.is_file() {
                return Err(io::Error::new(io::ErrorKind::InvalidInput, "not a file"));
            }
            if meta.len() > max_bytes {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("file is {} bytes, limit is {max_bytes}", meta.len()),
                ));
            }
            let modified_unix_ms = meta
                .modified()
                .ok()
                .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
                .map(|d| d.as_millis() as i64);
            let bytes = tokio::fs::read(path).await?;
            Ok(SourceRead { size_bytes: bytes.len() as u64, modified_unix_ms, bytes })
        })
    }
}
