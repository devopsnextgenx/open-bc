//! Domain types and pure comparison contracts for OpenBC.
//!
//! # Layout
//!
//! | Module             | Contents                                                        |
//! |--------------------|-----------------------------------------------------------------|
//! | crate root         | Folder-level entry/metadata types and one-level name alignment  |
//! | [`text`]           | Line diff, log normalisation, inline diff, syntax highlighting  |
//! | [`image_compare`]  | Image options/verdicts and the reference per-pixel semantics    |
//! | [`model3d`]        | 3D mesh, options, results and the loader extension point        |
//! | [`hex_compare`]    | Byte-level difference runs                                      |
//! | [`binary_compare`] | Binary model, version diff and result tree (no parsing)         |
//! | [`hash_policy`]    | Per-file-type size/timestamp vs. hash-algorithm matching policy |
//! | [`meta_compare`]   | Generic property-table comparison                               |
//! | [`table_compare`]  | CSV / spreadsheet row alignment, cell diff and copy edits       |
//! | [`registry_compare`] | Windows `.reg` parsing, diff, copy edits and writing          |
//! | [`resource_version`] | Resource version / change-assessment types for reload tracking |
//!
//! Everything here is pure: no I/O, no threads. Compute backends live elsewhere.

/// Binary (PE / ELF / Mach-O) version comparison: model, diff and result tree.
pub mod binary_compare;
/// Byte-level comparison primitives for hex compare.
pub mod hex_compare;
/// Per-file-type content matching policy (size/timestamp vs. hash algorithm).
pub mod hash_policy;
/// Image comparison domain types (options, verdicts, pixel probe).
pub mod image_compare;
/// Feature-gated Rust memory instrumentation for large-file performance tests.
#[cfg(feature = "mem-stats")]
pub mod mem;
/// Generic property-table comparison used by media compare.
pub mod meta_compare;
/// 3D model comparison domain types (options, verdicts, mesh probe).
pub mod model3d;
/// Windows registry export (`.reg`) parsing, comparison, copy edits and writing.
pub mod registry_compare;
/// Version identity and change assessment for loaded files / directory levels.
pub mod resource_version;
/// Table (CSV / spreadsheet) comparison; CSV text handling is in [`table_compare::csv`].
pub mod table_compare;
/// Pure text and log comparison algorithms.
pub mod text;

mod diff_util;
mod encoding;
mod folder;
mod telemetry;

pub use hash_policy::{FileTypeRule, HashAlgorithm, MatchMethod, MatchPolicy};
pub use folder::{
    compare_directory_entries, compare_file_content, compare_metadata, ComparisonStatus,
    ContentDigest, DirectoryEntry, EntryMetadata, EntryPath, FolderComparison,
    FolderEntryComparison, FolderMetadata,
};