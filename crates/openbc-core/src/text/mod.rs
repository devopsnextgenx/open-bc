//! Pure text comparison, normalization, and fuzzy matching support.

pub mod diff;
pub mod highlight;
pub mod models;
pub mod normalize;

pub use diff::{compute_inline_diff, TextCompareEngine};
pub use highlight::{HighlightEngine, HighlightSpan, HighlightStyle, HighlightTheme};
pub use models::{ChangeKind, CharChangeKind, CharDiff, CompareOptions, InlineDiff, LineDiff};
pub use normalize::LogNormalizer;
