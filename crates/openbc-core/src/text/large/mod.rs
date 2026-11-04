//! Large-file text indexing and chunked source access.
//!
//! The UI compares huge local files without first materializing the full document in memory. The
//! core primitive here provides a bounded chunk cache along with a newline index that is small enough
//! to keep in memory even for multi-gigabyte inputs.

pub mod hashing;
pub mod line_index;
pub mod source;

pub use hashing::{hash_line, normalize_line_bytes};
pub use line_index::LineIndex;
pub use source::{LargeTextSource, SourceChanged};
