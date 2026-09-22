//! Pure text comparison, normalization, and fuzzy matching support.

pub mod diff;
pub mod models;
pub mod normalize;

pub use diff::TextCompareEngine;
pub use models::{ChangeKind, CompareOptions, LineDiff};
pub use normalize::LogNormalizer;
