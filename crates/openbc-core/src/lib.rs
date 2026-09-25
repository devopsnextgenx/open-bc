//! Domain types and pure comparison contracts for OpenBC.

/// Pure text and log comparison algorithms.
pub mod text;

use std::path::PathBuf;
use std::time::SystemTime;

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
    /// An entry exists on only one side.
    Missing,
    /// The comparison could not read one side.
    Error(String),
}

/// A digest produced by a compute backend.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ContentDigest(pub Vec<u8>);

/// Compare phase-one metadata without touching file contents.
///
/// ```
/// use openbc_core::{compare_metadata, EntryMetadata};
/// let left = EntryMetadata { size: 4, modified: None, is_dir: false };
/// assert!(compare_metadata(&left, &left));
/// ```
#[must_use]
pub fn compare_metadata(left: &EntryMetadata, right: &EntryMetadata) -> bool {
    left.size == right.size && left.modified == right.modified && left.is_dir == right.is_dir
}

/// Align two directory levels by name without recursively scanning children.
#[must_use]
pub fn compare_directory_entries(
    left: &[DirectoryEntry],
    right: &[DirectoryEntry],
) -> Vec<FolderEntryComparison> {
    let started = std::time::Instant::now();
    let mut names = std::collections::BTreeSet::new();
    names.extend(left.iter().map(|entry| entry.name.clone()));
    names.extend(right.iter().map(|entry| entry.name.clone()));

    let result: Vec<_> = names
        .into_iter()
        .map(|name| {
            let left_entry = left.iter().find(|entry| entry.name == name).cloned();
            let right_entry = right.iter().find(|entry| entry.name == name).cloned();
            let status = match (&left_entry, &right_entry) {
                (Some(left_entry), Some(right_entry)) => {
                    if compare_metadata(&left_entry.metadata, &right_entry.metadata) {
                        ComparisonStatus::Equal
                    } else {
                        ComparisonStatus::Different
                    }
                }
                _ => ComparisonStatus::Missing,
            };
            FolderEntryComparison {
                name,
                left: left_entry,
                right: right_entry,
                status,
            }
        })
        .collect();
    openbc_observability::log(
        "openbc-core",
        "compare_directory_entries",
        "INFO",
        "folder level aligned",
    );
    openbc_observability::record(
        "openbc-core",
        "folder_compare",
        started.elapsed(),
        None,
        None,
        Some(
            left.iter()
                .chain(right)
                .map(|entry| entry.metadata.size)
                .sum(),
        ),
        Some(
            left.iter()
                .chain(right)
                .filter(|entry| entry.metadata.is_dir)
                .count(),
        ),
        Some("metadata alignment".to_owned()),
    );
    result
}
