//! Asynchronous VFS orchestration for text comparison.

use openbc_core::text::{CompareOptions, LineDiff, TextCompareEngine};
use openbc_core::EntryPath;
use openbc_vfs::{read_small_file, AsyncVfs, VfsError};
use rayon::{ThreadPool, ThreadPoolBuilder};
use std::sync::Arc;
use thiserror::Error;
use tokio::sync::mpsc::UnboundedSender;

/// Default number of diff rows sent in each output message.
pub const DEFAULT_DIFF_CHUNK_SIZE: usize = 256;

/// Failures produced while reading, comparing, or publishing text diffs.
#[derive(Debug, Error)]
pub enum TextCompareError {
    /// A VFS stream could not be opened or read.
    #[error("VFS error: {0}")]
    Vfs(#[from] VfsError),
    /// A VFS buffer was not valid UTF-8.
    #[error("{side} text is not valid UTF-8: {source}")]
    InvalidUtf8 {
        /// Which input contained invalid bytes.
        side: &'static str,
        #[source]
        source: std::string::FromUtf8Error,
    },
    /// The dedicated Rayon pool could not be created.
    #[error("failed to build text comparison pool: {0}")]
    Pool(#[from] rayon::ThreadPoolBuildError),
    /// The Tokio blocking task failed to join.
    #[error("text comparison task failed: {0}")]
    Compute(#[from] tokio::task::JoinError),
    /// The receiver was dropped before all diff chunks were sent.
    #[error("text diff receiver was dropped")]
    ReceiverClosed,
    /// A zero-sized chunk would make output progress impossible.
    #[error("diff chunk size must be greater than zero")]
    InvalidChunkSize,
}

/// Runs VFS-backed text comparisons and streams owned diff chunks to a receiver.
pub struct AsyncTextCompareTask {
    cpu_pool: Arc<ThreadPool>,
    chunk_size: usize,
}

impl AsyncTextCompareTask {
    /// Create a task using `worker_count` Rayon workers and the default chunk size.
    pub fn new(worker_count: usize) -> Result<Self, TextCompareError> {
        Self::with_chunk_size(worker_count, DEFAULT_DIFF_CHUNK_SIZE)
    }

    /// Create a task with explicit Rayon worker and output chunk counts.
    pub fn with_chunk_size(
        worker_count: usize,
        chunk_size: usize,
    ) -> Result<Self, TextCompareError> {
        if chunk_size == 0 {
            return Err(TextCompareError::InvalidChunkSize);
        }
        let cpu_pool = ThreadPoolBuilder::new()
            .num_threads(worker_count.max(1))
            .build()?;
        Ok(Self {
            cpu_pool: Arc::new(cpu_pool),
            chunk_size,
        })
    }

    /// Read two VFS streams, compare them off the Tokio worker threads, and send diff chunks.
    pub async fn run(
        &self,
        left_vfs: Arc<dyn AsyncVfs>,
        left_path: EntryPath,
        right_vfs: Arc<dyn AsyncVfs>,
        right_path: EntryPath,
        options: CompareOptions,
        sender: UnboundedSender<Vec<LineDiff>>,
    ) -> Result<(), TextCompareError> {
        let left_read = read_small_file(left_vfs.as_ref(), &left_path);
        let right_read = read_small_file(right_vfs.as_ref(), &right_path);
        let (left_bytes, right_bytes) = tokio::try_join!(left_read, right_read)?;
        let left =
            String::from_utf8(left_bytes).map_err(|source| TextCompareError::InvalidUtf8 {
                side: "left",
                source,
            })?;
        let right =
            String::from_utf8(right_bytes).map_err(|source| TextCompareError::InvalidUtf8 {
                side: "right",
                source,
            })?;

        let cpu_pool = Arc::clone(&self.cpu_pool);
        let diffs = tokio::task::spawn_blocking(move || {
            cpu_pool.install(|| TextCompareEngine::compare_buffers(&left, &right, &options))
        })
        .await?;

        for chunk in diffs.chunks(self.chunk_size) {
            sender
                .send(chunk.to_vec())
                .map_err(|_| TextCompareError::ReceiverClosed)?;
        }
        Ok(())
    }
}
