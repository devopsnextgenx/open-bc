//! Non-blocking, level-at-a-time folder scanning orchestration.

use openbc_compute::ComputeDispatcher;
use openbc_core::{DirectoryEntry, EntryPath};
use openbc_vfs::{AsyncVfs, VfsError};
use std::sync::Arc;
use thiserror::Error;
use tokio::io::AsyncReadExt;
use tokio::sync::mpsc;

/// Events consumed by UI adapters and other observers.
#[derive(Clone, Debug)]
pub enum ScanEvent {
    /// One directory level has been discovered.
    Directory {
        path: EntryPath,
        entries: Vec<DirectoryEntry>,
    },
    /// A file's phase-two content digest is ready.
    ContentHash { path: EntryPath, digest: Vec<u8> },
    /// A path could not be scanned without terminating sibling work.
    EntryError { path: EntryPath, message: String },
    /// The requested level has completed.
    LevelComplete { path: EntryPath },
}

/// Errors that terminate a scan pipeline.
#[derive(Debug, Error)]
pub enum ScanError {
    /// VFS operation failure for the requested level.
    #[error("VFS error: {0}")]
    Vfs(#[from] VfsError),
    /// A background compute task could not be joined.
    #[error("compute task failed: {0}")]
    Compute(#[from] tokio::task::JoinError),
    /// The receiver was dropped before the scan could publish an event.
    #[error("scan event receiver was dropped")]
    ReceiverClosed,
}

/// Scan exactly one expanded directory level and schedule file hashing.
///
/// The caller decides when to invoke this function for child directories, preserving lazy loading.
pub async fn scan_level(
    vfs: Arc<dyn AsyncVfs>,
    path: EntryPath,
    compute: Arc<ComputeDispatcher>,
    events: mpsc::Sender<ScanEvent>,
) -> Result<(), ScanError> {
    let entries = vfs.read_dir(&path).await?;
    events
        .send(ScanEvent::Directory {
            path: path.clone(),
            entries: entries.clone(),
        })
        .await
        .map_err(|_| ScanError::ReceiverClosed)?;

    for entry in entries {
        if entry.metadata.is_dir || entry.metadata.size == 0 {
            continue;
        }
        let vfs = Arc::clone(&vfs);
        let compute = Arc::clone(&compute);
        let events = events.clone();
        tokio::spawn(async move {
            match hash_entry(vfs, compute, entry.clone()).await {
                Ok(digest) => {
                    let _ = events
                        .send(ScanEvent::ContentHash {
                            path: entry.path,
                            digest,
                        })
                        .await;
                }
                Err(error) => {
                    let _ = events
                        .send(ScanEvent::EntryError {
                            path: entry.path,
                            message: error.to_string(),
                        })
                        .await;
                }
            }
        });
    }
    events
        .send(ScanEvent::LevelComplete { path })
        .await
        .map_err(|_| ScanError::ReceiverClosed)
}

async fn hash_entry(
    vfs: Arc<dyn AsyncVfs>,
    compute: Arc<ComputeDispatcher>,
    entry: DirectoryEntry,
) -> Result<Vec<u8>, ScanError> {
    let mut reader = vfs.open_file(&entry.path).await?;
    let mut bytes = Vec::with_capacity(entry.metadata.size as usize);
    reader
        .read_to_end(&mut bytes)
        .await
        .map_err(|source| VfsError::Io {
            operation: "read_file",
            path: entry.path.0.clone(),
            source,
        })?;
    let result = tokio::task::spawn_blocking(move || compute.hash_bytes(&bytes)).await?;
    Ok(result.digest.to_vec())
}
