//! Level-at-a-time folder scanning: directory listing plus bounded background hashing.

use std::sync::{Arc, OnceLock};
use std::time::Instant;

use openbc_compute::ComputeDispatcher;
use openbc_core::{DirectoryEntry, EntryPath};
use openbc_vfs::{read_small_file, AsyncVfs, VfsError};
use thiserror::Error;
use tokio::sync::mpsc;

use crate::metrics::depth_of;

/// Files hashed concurrently within one scanned level.
const MAX_CONCURRENT_HASHES: usize = 2;
/// Workers in the shared pool used for whole-file content comparison.
const SHARED_HASH_WORKERS: usize = 2;

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
    let started = Instant::now();
    let entries = vfs.read_dir(&path).await?;
    record_op!(
        "scan_level",
        started,
        io: None,
        bytes: Some(entries.iter().map(|entry| entry.metadata.size).sum()),
        depth: Some(depth_of(&path)),
        note: "Tokio + bounded events",
    );
    publish(&events, ScanEvent::Directory { path: path.clone(), entries: entries.clone() }).await?;

    let mut hash_tasks = tokio::task::JoinSet::new();
    for entry in entries {
        if entry.metadata.is_dir || entry.metadata.size == 0 {
            continue;
        }
        while hash_tasks.len() >= MAX_CONCURRENT_HASHES {
            if let Some(result) = hash_tasks.join_next().await {
                publish_hash_result(result, &events).await?;
            }
        }
        let vfs = Arc::clone(&vfs);
        let compute = Arc::clone(&compute);
        hash_tasks.spawn(async move {
            let path = entry.path.clone();
            (path, hash_entry(vfs, compute, entry).await)
        });
    }
    while let Some(result) = hash_tasks.join_next().await {
        publish_hash_result(result, &events).await?;
    }
    publish(&events, ScanEvent::LevelComplete { path }).await
}

async fn publish(events: &mpsc::Sender<ScanEvent>, event: ScanEvent) -> Result<(), ScanError> {
    events.send(event).await.map_err(|_| ScanError::ReceiverClosed)
}

async fn publish_hash_result(
    result: Result<(EntryPath, Result<Vec<u8>, ScanError>), tokio::task::JoinError>,
    events: &mpsc::Sender<ScanEvent>,
) -> Result<(), ScanError> {
    let (path, result) = result?;
    let event = match result {
        Ok(digest) => ScanEvent::ContentHash { path, digest },
        Err(error) => ScanEvent::EntryError { path, message: error.to_string() },
    };
    publish(events, event).await
}

/// Compare two files by SHA-256 digest without exposing their contents to callers.
pub async fn compare_file_content_by_hash(
    left_vfs: &dyn AsyncVfs,
    left_path: &EntryPath,
    right_vfs: &dyn AsyncVfs,
    right_path: &EntryPath,
) -> Result<bool, ScanError> {
    // One file in memory at a time: read + hash the left side and free it before the right side
    // is read (both at once doubled the peak, up to 2 x the in-memory limit). Remote providers
    // serialize on one connection anyway, so nothing is lost by not downloading in parallel.
    let left_digest = digest_of(left_vfs, left_path).await?;
    let right_digest = digest_of(right_vfs, right_path).await?;
    Ok(left_digest == right_digest)
}

/// Reads one file (bounded by [`read_small_file`]), hashes it on the compute pool and frees the
/// bytes there; only the digest comes back.
async fn digest_of(vfs: &dyn AsyncVfs, path: &EntryPath) -> Result<Vec<u8>, ScanError> {
    let bytes = read_small_file(vfs, path).await?;
    Ok(tokio::task::spawn_blocking(move || shared_dispatcher().hash_bytes(&bytes).digest.to_vec()).await?)
}

fn shared_dispatcher() -> &'static ComputeDispatcher {
    static COMPUTE: OnceLock<ComputeDispatcher> = OnceLock::new();
    COMPUTE.get_or_init(|| {
        ComputeDispatcher::new(SHARED_HASH_WORKERS).expect("the shared CPU hash pool should initialize")
    })
}

async fn hash_entry(
    vfs: Arc<dyn AsyncVfs>,
    compute: Arc<ComputeDispatcher>,
    entry: DirectoryEntry,
) -> Result<Vec<u8>, ScanError> {
    let started = Instant::now();
    // Bounded read straight into one buffer. The old path went through `open_file` (a second
    // full copy inside the provider) and pre-allocated the size the SERVER reported.
    let bytes = read_small_file(vfs.as_ref(), &entry.path).await?;
    let byte_count = bytes.len();
    let result = tokio::task::spawn_blocking(move || compute.hash_bytes(&bytes)).await?;
    record_op!(
        "file_hash",
        started,
        io: Some((byte_count, 0)),
        bytes: None,
        depth: Some(depth_of(&entry.path)),
        note: "Tokio read + compute",
    );
    Ok(result.digest.to_vec())
}