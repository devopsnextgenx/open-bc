//! Non-blocking, level-at-a-time folder scanning orchestration.

pub mod text_runner;

use openbc_compute::ComputeDispatcher;
use openbc_core::{
    compare_directory_entries, ComparisonStatus, DirectoryEntry, EntryPath, FolderComparison,
    FolderMetadata,
};
use openbc_vfs::{read_folder_level, read_small_file, AsyncVfs, VfsError};
use std::sync::{Arc, OnceLock};
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
    compare_folder_sides_with_options(
        left_vfs,
        Some(left_path),
        right_vfs,
        Some(right_path),
        check_content,
        ignore_timestamps,
    )
    .await
}

/// Compare one directory level when either folder side is absent.
pub async fn compare_folder_sides_with_options(
    left_vfs: Arc<dyn AsyncVfs>,
    left_path: Option<EntryPath>,
    right_vfs: Arc<dyn AsyncVfs>,
    right_path: Option<EntryPath>,
    check_content: bool,
    ignore_timestamps: bool,
) -> Result<FolderComparison, ScanError> {
    let started = std::time::Instant::now();
    let (left_metadata, left_entries) = match &left_path {
        Some(path) => read_folder_level(left_vfs.as_ref(), path).await?,
        None => (empty_folder_metadata(), Vec::new()),
    };
    let (right_metadata, right_entries) = match &right_path {
        Some(path) => read_folder_level(right_vfs.as_ref(), path).await?,
        None => (empty_folder_metadata(), Vec::new()),
    };
    let mut entries = compare_directory_entries(&left_entries, &right_entries);
    for entry in &mut entries {
        let (Some(left), Some(right)) = (&entry.left, &entry.right) else {
            continue;
        };
        if left.metadata.is_dir
            || right.metadata.is_dir
            || left.metadata.is_dir != right.metadata.is_dir
        {
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
            match compare_file_content_by_hash(
                left_vfs.as_ref(),
                &left.path,
                right_vfs.as_ref(),
                &right.path,
            )
            .await
            {
                Ok(true) => entry.status = ComparisonStatus::Equal,
                Ok(false) => entry.status = ComparisonStatus::Different,
                Err(error) => entry.status = ComparisonStatus::Error(error.to_string()),
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
    let depth = left_path
        .as_ref()
        .map_or(0, |path| path.0.components().count())
        .max(
            right_path
                .as_ref()
                .map_or(0, |path| path.0.components().count()),
        );
    openbc_observability::record(
        "openbc-engine",
        "folder_compare",
        started.elapsed(),
        None,
        None,
        Some(comparison.left.size + comparison.right.size),
        Some(depth),
        Some("Tokio orchestration".to_owned()),
    );
    Ok(comparison)
}

fn empty_folder_metadata() -> FolderMetadata {
    FolderMetadata {
        path: EntryPath(Default::default()),
        item_count: 0,
        size: 0,
        modified: None,
        readable: true,
    }
}

fn newer_status(
    left: Option<std::time::SystemTime>,
    right: Option<std::time::SystemTime>,
) -> ComparisonStatus {
    match (left, right) {
        (Some(left_time), Some(right_time)) if left_time > right_time => {
            ComparisonStatus::LeftNewer
        }
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

    let mut hash_tasks = tokio::task::JoinSet::new();
    for entry in entries {
        if entry.metadata.is_dir || entry.metadata.size == 0 {
            continue;
        }
        while hash_tasks.len() >= 2 {
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
    events
        .send(ScanEvent::LevelComplete { path })
        .await
        .map_err(|_| ScanError::ReceiverClosed)
}

async fn publish_hash_result(
    result: Result<(EntryPath, Result<Vec<u8>, ScanError>), tokio::task::JoinError>,
    events: &mpsc::Sender<ScanEvent>,
) -> Result<(), ScanError> {
    let (path, result) = result?;
    let event = match result {
        Ok(digest) => ScanEvent::ContentHash { path, digest },
        Err(error) => ScanEvent::EntryError {
            path,
            message: error.to_string(),
        },
    };
    events
        .send(event)
        .await
        .map_err(|_| ScanError::ReceiverClosed)
}

/// Compare two files by SHA-256 digest without exposing their contents to callers.
pub async fn compare_file_content_by_hash(
    left_vfs: &dyn AsyncVfs,
    left_path: &EntryPath,
    right_vfs: &dyn AsyncVfs,
    right_path: &EntryPath,
) -> Result<bool, ScanError> {
    let (left_bytes, right_bytes) = tokio::try_join!(
        read_small_file(left_vfs, left_path),
        read_small_file(right_vfs, right_path),
    )?;
    let result = tokio::task::spawn_blocking(move || {
        let compute = compute_dispatcher();
        compute.hash_bytes(&left_bytes).digest == compute.hash_bytes(&right_bytes).digest
    })
    .await?;
    Ok(result)
}

fn compute_dispatcher() -> &'static ComputeDispatcher {
    static COMPUTE: OnceLock<ComputeDispatcher> = OnceLock::new();
    COMPUTE.get_or_init(|| {
        ComputeDispatcher::new(2).expect("the shared CPU hash pool should initialize")
    })
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

/// Copy one file or directory tree between VFS providers without UI-side filesystem access.
pub async fn copy_entry(
    source_vfs: &dyn AsyncVfs,
    source_path: &EntryPath,
    destination_vfs: &dyn AsyncVfs,
    destination_path: &EntryPath,
) -> Result<(), VfsError> {
    let mut pending = vec![(source_path.clone(), destination_path.clone())];
    while let Some((source, destination)) = pending.pop() {
        let metadata = source_vfs.stat(&source).await?;
        if metadata.is_link {
            return Err(VfsError::Unsupported("copy_symlink"));
        }
        if metadata.is_dir {
            match destination_vfs.stat(&destination).await {
                Ok(existing) if existing.is_dir => {}
                Ok(_) => {
                    return Err(VfsError::Io {
                        operation: "copy_entry",
                        path: destination.0,
                        source: std::io::Error::new(
                            std::io::ErrorKind::AlreadyExists,
                            "destination exists and is not a directory",
                        ),
                    });
                }
                Err(_) => destination_vfs.create_dir(&destination).await?,
            }
            for entry in source_vfs.read_dir(&source).await? {
                pending.push((entry.path, EntryPath(destination.0.join(entry.name))));
            }
        } else {
            let bytes = read_small_file(source_vfs, &source).await?;
            destination_vfs.write_file(&destination, &bytes).await?;
        }
    }
    Ok(())
}

/// Make one destination entry match a source entry, removing destination-only descendants.
pub async fn mirror_entry(
    source_vfs: &dyn AsyncVfs,
    source_path: &EntryPath,
    destination_vfs: &dyn AsyncVfs,
    destination_path: &EntryPath,
) -> Result<(), VfsError> {
    copy_entry(source_vfs, source_path, destination_vfs, destination_path).await?;
    let source_metadata = source_vfs.stat(source_path).await?;
    if !source_metadata.is_dir || source_metadata.is_link {
        return Ok(());
    }
    let mut folders = vec![(source_path.clone(), destination_path.clone())];
    while let Some((source, destination)) = folders.pop() {
        let source_entries = source_vfs.read_dir(&source).await?;
        let destination_entries = destination_vfs.read_dir(&destination).await?;
        let source_names: std::collections::HashSet<_> = source_entries
            .iter()
            .map(|entry| entry.name.as_str())
            .collect();
        for entry in destination_entries {
            if !source_names.contains(entry.name.as_str()) {
                delete_entry(destination_vfs, &entry.path).await?;
            }
        }
        for entry in source_entries {
            if entry.metadata.is_dir && !entry.metadata.is_link {
                folders.push((
                    entry.path.clone(),
                    EntryPath(destination.0.join(entry.name)),
                ));
            }
        }
    }
    Ok(())
}

/// Delete a file or directory tree through its provider, without following symlinks.
pub async fn delete_entry(vfs: &dyn AsyncVfs, path: &EntryPath) -> Result<(), VfsError> {
    let mut pending = vec![(path.clone(), false)];
    while let Some((current, visited)) = pending.pop() {
        let metadata = vfs.stat(&current).await?;
        if metadata.is_dir && !metadata.is_link {
            if visited {
                vfs.remove_dir(&current).await?;
            } else {
                pending.push((current.clone(), true));
                for entry in vfs.read_dir(&current).await? {
                    pending.push((entry.path, false));
                }
            }
        } else {
            vfs.remove_file(&current).await?;
        }
    }
    Ok(())
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
    use super::{
        compare_folder_level_with_options, copy_entry, create_directory, delete_entry,
        mirror_entry, read_file, rename_entry, write_file,
    };
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
            Arc::clone(&left),
            EntryPath("".into()),
            Arc::clone(&right),
            EntryPath("".into()),
            true,
            false,
        )
        .await
        .unwrap();
        assert_eq!(equal.entries[0].status, ComparisonStatus::Equal);

        write_file(right.as_ref(), &file, b"diff").await.unwrap();
        let different = compare_folder_level_with_options(
            Arc::clone(&left),
            EntryPath("".into()),
            Arc::clone(&right),
            EntryPath("".into()),
            true,
            false,
        )
        .await
        .unwrap();
        assert_eq!(different.entries[0].status, ComparisonStatus::Different);
        assert_eq!(read_file(left.as_ref(), &file).await.unwrap(), b"same");

        let directory = EntryPath("created".into());
        create_directory(left.as_ref(), &directory).await.unwrap();
        let nested_file = EntryPath("created/nested.txt".into());
        write_file(left.as_ref(), &nested_file, b"nested")
            .await
            .unwrap();
        let copied_directory = EntryPath("copied".into());
        copy_entry(left.as_ref(), &directory, right.as_ref(), &copied_directory)
            .await
            .unwrap();
        assert_eq!(
            read_file(right.as_ref(), &EntryPath("copied/nested.txt".into()))
                .await
                .unwrap(),
            b"nested"
        );
        let mirror_directory = EntryPath("mirror".into());
        create_directory(right.as_ref(), &mirror_directory)
            .await
            .unwrap();
        write_file(
            right.as_ref(),
            &EntryPath("mirror/extra.txt".into()),
            b"extra",
        )
        .await
        .unwrap();
        mirror_entry(left.as_ref(), &directory, right.as_ref(), &mirror_directory)
            .await
            .unwrap();
        assert_eq!(
            read_file(right.as_ref(), &EntryPath("mirror/nested.txt".into()))
                .await
                .unwrap(),
            b"nested"
        );
        assert!(right
            .stat(&EntryPath("mirror/extra.txt".into()))
            .await
            .is_err());
        delete_entry(right.as_ref(), &copied_directory)
            .await
            .unwrap();
        assert!(right.stat(&copied_directory).await.is_err());
        let renamed = EntryPath("renamed.txt".into());
        rename_entry(left.as_ref(), &file, &renamed).await.unwrap();
        assert_eq!(read_file(left.as_ref(), &renamed).await.unwrap(), b"same");

        tokio::fs::remove_dir_all(root).await.unwrap();
    }
}
