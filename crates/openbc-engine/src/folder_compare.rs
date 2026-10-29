//! One-level folder comparison: entry matching plus optional content / timestamp checks.

use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime};

use openbc_core::{
    compare_directory_entries, ComparisonStatus, DirectoryEntry, EntryPath, FolderComparison,
    FolderMetadata,
};
use openbc_vfs::{read_folder_level, AsyncVfs};

use crate::metrics::depth_of;
use crate::scan::{compare_file_content_by_hash, ScanError};

/// Same-size files whose timestamps differ are verified by content up to this size; a timestamp
/// alone must not make identical files look different (typical right after a copy, which stamps
/// the destination with the copy time). Larger files keep the timestamp-based Newer/Older status.
const TIMESTAMP_VERIFY_LIMIT: u64 = 16 * 1024 * 1024;

/// Modification times closer than this count as equal (FAT-style 2 s resolution).
const TIMESTAMP_TOLERANCE: Duration = Duration::from_secs(2);

/// Compare one visible folder level from two providers.
pub async fn compare_folder_level(
    left_vfs: Arc<dyn AsyncVfs>,
    left_path: EntryPath,
    right_vfs: Arc<dyn AsyncVfs>,
    right_path: EntryPath,
) -> Result<FolderComparison, ScanError> {
    compare_folder_level_with_options(left_vfs, left_path, right_vfs, right_path, false, false).await
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
    let started = Instant::now();
    let (left_metadata, left_entries) = read_side(left_vfs.as_ref(), left_path.as_ref()).await?;
    let (right_metadata, right_entries) = read_side(right_vfs.as_ref(), right_path.as_ref()).await?;
    let mut entries = compare_directory_entries(&left_entries, &right_entries);
    for entry in &mut entries {
        let (Some(left), Some(right)) = (&entry.left, &entry.right) else {
            continue;
        };
        if left.metadata.is_dir || right.metadata.is_dir {
            continue;
        }
        if left.metadata.size != right.metadata.size {
            entry.status = ComparisonStatus::Different;
            continue;
        }
        if check_content {
            let same = compare_file_content_by_hash(
                left_vfs.as_ref(),
                &left.path,
                right_vfs.as_ref(),
                &right.path,
            )
            .await;
            entry.status = match same {
                Ok(true) => ComparisonStatus::Equal,
                Ok(false) => ComparisonStatus::Different,
                Err(error) => ComparisonStatus::Error(error.to_string()),
            };
        } else if ignore_timestamps || modified_times_match(left.metadata.modified, right.metadata.modified) {
            entry.status = ComparisonStatus::Equal;
        } else if left.metadata.size <= TIMESTAMP_VERIFY_LIMIT
            && matches!(
                compare_file_content_by_hash(left_vfs.as_ref(), &left.path, right_vfs.as_ref(), &right.path).await,
                Ok(true)
            )
        {
            // Identical bytes: a different timestamp is not a difference.
            entry.status = ComparisonStatus::Equal;
        } else {
            entry.status = newer_status(left.metadata.modified, right.metadata.modified);
        }
    }
    let comparison = FolderComparison { left: left_metadata, right: right_metadata, entries };
    openbc_observability::log("openbc-engine", "compare_folder_level", "INFO", "folder comparison complete");
    let depth = left_path.as_ref().map_or(0, depth_of).max(right_path.as_ref().map_or(0, depth_of));
    record_op!(
        "folder_compare",
        started,
        io: None,
        bytes: Some(comparison.left.size + comparison.right.size),
        depth: Some(depth),
        note: "Tokio orchestration",
    );
    Ok(comparison)
}

/// Reads one folder level, or yields an empty folder when that side is absent.
async fn read_side(
    vfs: &dyn AsyncVfs,
    path: Option<&EntryPath>,
) -> Result<(FolderMetadata, Vec<DirectoryEntry>), ScanError> {
    match path {
        Some(path) => Ok(read_folder_level(vfs, path).await?),
        None => Ok((empty_folder_metadata(), Vec::new())),
    }
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

fn modified_times_match(left: Option<SystemTime>, right: Option<SystemTime>) -> bool {
    match (left, right) {
        (Some(left), Some(right)) => {
            left.duration_since(right).unwrap_or_else(|error| error.duration()) <= TIMESTAMP_TOLERANCE
        }
        (None, None) => true,
        _ => false,
    }
}

fn newer_status(left: Option<SystemTime>, right: Option<SystemTime>) -> ComparisonStatus {
    match (left, right) {
        (Some(left), Some(right)) if left > right => ComparisonStatus::LeftNewer,
        (Some(_), Some(_)) => ComparisonStatus::RightNewer,
        _ => ComparisonStatus::Different,
    }
}
