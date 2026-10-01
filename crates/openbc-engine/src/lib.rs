//! Non-blocking, level-at-a-time folder scanning orchestration.

pub mod text_runner;

use openbc_compute::ComputeDispatcher;
use openbc_core::{
    compare_directory_entries, compare_file_content, ComparisonStatus, DirectoryEntry, EntryPath,
    FolderComparison,
};
use openbc_vfs::{read_folder_level, read_small_file, AsyncVfs, VfsError};
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

/// Compare one visible folder level from two providers.
pub async fn compare_folder_level(
    left_vfs: Arc<dyn AsyncVfs>,
    left_path: EntryPath,
    right_vfs: Arc<dyn AsyncVfs>,
    right_path: EntryPath,
) -> Result<FolderComparison, ScanError> {
    compare_folder_level_with_options(left_vfs, left_path, right_vfs, right_path, false, false)
        .await
}

/// Compare one directory level and optionally compare matching file bytes.
pub async fn compare_folder_level_with_options(
    left_vfs: Arc<dyn AsyncVfs>,
    left_path: EntryPath,
    right_vfs: Arc<dyn AsyncVfs>,
    right_path: EntryPath,
    check_content: bool,
    ignore_timestamps: bool,
) -> Result<FolderComparison, ScanError> {
    let started = std::time::Instant::now();
    let (left_metadata, left_entries) = read_folder_level(left_vfs.as_ref(), &left_path).await?;
    let (right_metadata, right_entries) =
        read_folder_level(right_vfs.as_ref(), &right_path).await?;
    let mut entries = compare_directory_entries(&left_entries, &right_entries);
    for entry in &mut entries {
        let (Some(left), Some(right)) = (&entry.left, &entry.right) else {
            continue;
        };
        if left.metadata.is_dir || right.metadata.is_dir || left.metadata.is_dir != right.metadata.is_dir {
            continue;
        }
        if left.metadata.size != right.metadata.size {
            entry.status = ComparisonStatus::Different;
            continue;
        }
        let timestamps_match = match (left.metadata.modified, right.metadata.modified) {
            (Some(left_time), Some(right_time)) => {
                left_time
                    .duration_since(right_time)
                    .unwrap_or_else(|error| error.duration())
                    <= std::time::Duration::from_secs(2)
            }
            (None, None) => true,
            _ => false,
        };
        if check_content {
            match (
                read_small_file(left_vfs.as_ref(), &left.path).await,
                read_small_file(right_vfs.as_ref(), &right.path).await,
            ) {
                (Ok(left_bytes), Ok(right_bytes)) => {
                    if compare_file_content(&left_bytes, &right_bytes) {
                        entry.status = ComparisonStatus::Equal;
                    } else if !timestamps_match {
                        entry.status = newer_status(left.metadata.modified, right.metadata.modified);
                    } else {
                        entry.status = ComparisonStatus::Different;
                    }
                }
                (Err(error), _) | (_, Err(error)) => {
                    entry.status = ComparisonStatus::Error(error.to_string());
                }
            }
        } else if ignore_timestamps || timestamps_match {
            entry.status = ComparisonStatus::Equal;
        } else {
            entry.status = newer_status(left.metadata.modified, right.metadata.modified);
        }
    }
    let comparison = FolderComparison {
        left: left_metadata,
        right: right_metadata,
        entries,
    };
    openbc_observability::log(
        "openbc-engine",
        "compare_folder_level",
        "INFO",
        "folder comparison complete",
    );
    openbc_observability::record(
        "openbc-engine",
        "folder_compare",
        started.elapsed(),
        None,
        None,
        Some(comparison.left.size + comparison.right.size),
        Some(
            left_path
                .0
                .components()
                .count()
                .max(right_path.0.components().count()),
        ),
        Some("Tokio orchestration".to_owned()),
    );
    Ok(comparison)
}

fn newer_status(
    left: Option<std::time::SystemTime>,
    right: Option<std::time::SystemTime>,
) -> ComparisonStatus {
    match (left, right) {
        (Some(left_time), Some(right_time)) if left_time > right_time => ComparisonStatus::LeftNewer,
        (Some(_), Some(_)) => ComparisonStatus::RightNewer,
        _ => ComparisonStatus::Different,
    }
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
    let started = std::time::Instant::now();
    let entries = vfs.read_dir(&path).await?;
    openbc_observability::record(
        "openbc-engine",
        "scan_level",
        started.elapsed(),
        None,
        None,
        Some(entries.iter().map(|entry| entry.metadata.size).sum()),
        Some(path.0.components().count()),
        Some("Tokio + bounded events".to_owned()),
    );
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

/// Replace file contents through the selected VFS provider.
pub async fn write_file(
    vfs: &dyn AsyncVfs,
    path: &EntryPath,
    bytes: &[u8],
) -> Result<(), VfsError> {
    vfs.write_file(path, bytes).await
}

/// Read file bytes through the selected VFS provider.
pub async fn read_file(vfs: &dyn AsyncVfs, path: &EntryPath) -> Result<Vec<u8>, VfsError> {
    read_small_file(vfs, path).await
}

/// Create one directory through the selected VFS provider.
pub async fn create_directory(vfs: &dyn AsyncVfs, path: &EntryPath) -> Result<(), VfsError> {
    vfs.create_dir(path).await
}

/// Rename a file or directory through the selected VFS provider.
pub async fn rename_entry(
    vfs: &dyn AsyncVfs,
    from: &EntryPath,
    to: &EntryPath,
) -> Result<(), VfsError> {
    vfs.rename(from, to).await
}

async fn hash_entry(
    vfs: Arc<dyn AsyncVfs>,
    compute: Arc<ComputeDispatcher>,
    entry: DirectoryEntry,
) -> Result<Vec<u8>, ScanError> {
    let started = std::time::Instant::now();
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
    let byte_count = bytes.len();
    let result = tokio::task::spawn_blocking(move || compute.hash_bytes(&bytes)).await?;
    openbc_observability::record(
        "openbc-engine",
        "file_hash",
        started.elapsed(),
        Some((byte_count, 0)),
        None,
        None,
        Some(entry.path.0.components().count()),
        Some("Tokio read + compute".to_owned()),
    );
    Ok(result.digest.to_vec())
}

#[cfg(test)]
mod tests {
    use super::{compare_folder_level_with_options, create_directory, read_file, rename_entry, write_file};
    use openbc_core::{ComparisonStatus, EntryPath};
    use openbc_vfs::{AsyncVfs, LocalVfs};
    use std::sync::Arc;

    #[tokio::test]
    async fn compares_and_mutates_files_through_engine_and_vfs() {
        let unique = format!(
            "openbc-engine-test-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        );
        let root = std::env::temp_dir().join(unique);
        let left_root = root.join("left");
        let right_root = root.join("right");
        tokio::fs::create_dir_all(&left_root).await.unwrap();
        tokio::fs::create_dir_all(&right_root).await.unwrap();
        let left: Arc<dyn AsyncVfs> = Arc::new(LocalVfs::new(&left_root));
        let right: Arc<dyn AsyncVfs> = Arc::new(LocalVfs::new(&right_root));

        let file = EntryPath("sample.txt".into());
        write_file(left.as_ref(), &file, b"same").await.unwrap();
        write_file(right.as_ref(), &file, b"same").await.unwrap();
        let equal = compare_folder_level_with_options(
            Arc::clone(&left), EntryPath("".into()), Arc::clone(&right),
            EntryPath("".into()), true, false,
        ).await.unwrap();
        assert_eq!(equal.entries[0].status, ComparisonStatus::Equal);

        write_file(right.as_ref(), &file, b"changed").await.unwrap();
        let different = compare_folder_level_with_options(
            Arc::clone(&left), EntryPath("".into()), Arc::clone(&right),
            EntryPath("".into()), true, false,
        ).await.unwrap();
        assert_eq!(different.entries[0].status, ComparisonStatus::Different);
        assert_eq!(read_file(left.as_ref(), &file).await.unwrap(), b"same");

        let directory = EntryPath("created".into());
        create_directory(left.as_ref(), &directory).await.unwrap();
        let renamed = EntryPath("renamed.txt".into());
        rename_entry(left.as_ref(), &file, &renamed).await.unwrap();
        assert_eq!(read_file(left.as_ref(), &renamed).await.unwrap(), b"same");

        tokio::fs::remove_dir_all(root).await.unwrap();
    }
}
