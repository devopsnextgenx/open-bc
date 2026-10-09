//! Phase-one folder comparison: entry/metadata types and one-level name alignment.

use std::collections::BTreeMap;
use std::path::PathBuf;
use std::time::{Instant, SystemTime};

use crate::telemetry::{self, Sample};

/// A stable identifier for a VFS entry.
#[derive(Clone, Debug, Eq, Hash, PartialEq)]
pub struct EntryPath(pub PathBuf);

/// Metadata needed for phase-one folder comparison.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EntryMetadata {
    /// Byte length of the entry when it is a regular file.
    pub size: u64,
    /// Last modification time, when the provider can supply it.
    pub modified: Option<SystemTime>,
    /// Whether this entry is a directory.
    pub is_dir: bool,
    /// Whether the entry itself is a symbolic link.
    pub is_link: bool,
}

/// A lazily discovered directory child.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DirectoryEntry {
    /// Provider-relative path.
    pub path: EntryPath,
    /// Display name supplied by the provider.
    pub name: String,
    /// Phase-one metadata.
    pub metadata: EntryMetadata,
}

/// Basic metadata for a folder shown in a comparison header or status bar.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FolderMetadata {
    /// Provider path represented by this metadata.
    pub path: EntryPath,
    /// Number of immediate children discovered for the folder.
    pub item_count: usize,
    /// Sum of immediate child sizes reported by the provider.
    pub size: u64,
    /// Last modification time for the folder, when available.
    pub modified: Option<SystemTime>,
    /// Whether the provider allowed the folder to be read.
    pub readable: bool,
}

/// A single row in a one-level folder comparison.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FolderEntryComparison {
    /// Display name shared by the row.
    pub name: String,
    /// Entry from the left folder, when present.
    pub left: Option<DirectoryEntry>,
    /// Entry from the right folder, when present.
    pub right: Option<DirectoryEntry>,
    /// Metadata-level result for this row.
    pub status: ComparisonStatus,
}

/// Metadata and rows for one lazily loaded folder level.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FolderComparison {
    /// Metadata for the left folder.
    pub left: FolderMetadata,
    /// Metadata for the right folder.
    pub right: FolderMetadata,
    /// Name-aligned rows for the visible folder level.
    pub entries: Vec<FolderEntryComparison>,
}

/// Result of comparing two entries after metadata and optional content phases.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ComparisonStatus {
    /// Both entries have equal metadata and content, when content was checked.
    Equal,
    /// The entries differ in size, timestamp, or content.
    Different,
    /// The left file is newer than the right file.
    LeftNewer,
    /// The right file is newer than the left file.
    RightNewer,
    /// An entry exists on only one side.
    Missing,
    /// The comparison could not read one side.
    Error(String),
}

/// A digest produced by a compute backend.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ContentDigest(pub Vec<u8>);

/// Compare file contents after providers have read the bytes.
#[must_use]
pub fn compare_file_content(left: &[u8], right: &[u8]) -> bool {
    left == right
}

/// Compare phase-one metadata without touching file contents.
///
/// ```
/// use openbc_core::{compare_metadata, EntryMetadata};
/// let left = EntryMetadata { size: 4, modified: None, is_dir: false, is_link: false };
/// assert!(compare_metadata(&left, &left));
/// ```
#[must_use]
pub fn compare_metadata(left: &EntryMetadata, right: &EntryMetadata) -> bool {
    left.size == right.size
        && left.modified == right.modified
        && left.is_dir == right.is_dir
        && left.is_link == right.is_link
}

/// Metadata-level status for a name present on one or both sides.
fn status_of(left: Option<&DirectoryEntry>, right: Option<&DirectoryEntry>) -> ComparisonStatus {
    match (left, right) {
        (Some(l), Some(r)) if compare_metadata(&l.metadata, &r.metadata) => ComparisonStatus::Equal,
        (Some(_), Some(_)) => ComparisonStatus::Different,
        _ => ComparisonStatus::Missing,
    }
}

/// Align two directory levels by name without recursively scanning children.
///
/// Rows are sorted by name. When a side lists the same name more than once, the first
/// entry wins.
#[must_use]
pub fn compare_directory_entries(
    left: &[DirectoryEntry],
    right: &[DirectoryEntry],
) -> Vec<FolderEntryComparison> {
    let started = Instant::now();

    let mut by_name: BTreeMap<&str, (Option<&DirectoryEntry>, Option<&DirectoryEntry>)> =
        BTreeMap::new();
    for entry in left {
        by_name.entry(entry.name.as_str()).or_default().0.get_or_insert(entry);
    }
    for entry in right {
        by_name.entry(entry.name.as_str()).or_default().1.get_or_insert(entry);
    }

    let result: Vec<_> = by_name
        .into_iter()
        .map(|(name, (left_entry, right_entry))| FolderEntryComparison {
            name: name.to_owned(),
            left: left_entry.cloned(),
            right: right_entry.cloned(),
            status: status_of(left_entry, right_entry),
        })
        .collect();

    telemetry::info("compare_directory_entries", "folder level aligned");
    telemetry::record(
        "folder_compare",
        started,
        Sample {
            total_size: Some(left.iter().chain(right).map(|e| e.metadata.size).sum::<u64>()),
            total_count: Some(left.iter().chain(right).filter(|e| e.metadata.is_dir).count()),
            detail: Some("metadata alignment".to_owned()),
            ..Sample::default()
        },
    );
    result
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entry(name: &str, size: u64) -> DirectoryEntry {
        DirectoryEntry {
            path: EntryPath(PathBuf::from(name)),
            name: name.to_owned(),
            metadata: EntryMetadata { size, modified: None, is_dir: false, is_link: false },
        }
    }

    #[test]
    fn aligns_by_name_in_sorted_order() {
        let left = [entry("b", 2), entry("a", 1)];
        let right = [entry("b", 3), entry("c", 1)];
        let rows = compare_directory_entries(&left, &right);
        let names: Vec<_> = rows.iter().map(|r| r.name.as_str()).collect();
        assert_eq!(names, ["a", "b", "c"]);
        assert_eq!(rows[0].status, ComparisonStatus::Missing);
        assert_eq!(rows[1].status, ComparisonStatus::Different);
        assert_eq!(rows[2].status, ComparisonStatus::Missing);
    }

    #[test]
    fn first_duplicate_name_wins() {
        let left = [entry("a", 1), entry("a", 9)];
        let right = [entry("a", 1)];
        let rows = compare_directory_entries(&left, &right);
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].status, ComparisonStatus::Equal);
        assert_eq!(rows[0].left.as_ref().map(|e| e.metadata.size), Some(1));
    }
}
