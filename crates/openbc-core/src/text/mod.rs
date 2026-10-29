//! Pure text comparison, normalization, and fuzzy matching support.

pub mod diff;
pub mod highlight;
pub mod models;
pub mod normalize;

pub use diff::{compute_inline_diff, TextCompareEngine};
pub use highlight::{
    highlight_to_html, highlight_to_spans, language_catalog, language_catalog_text, normalize_user_key,
    plain_text_language, set_user_mappings, set_user_mappings_text, HighlightSpan, LanguageInfo,
};
pub use models::{ChangeKind, CharChangeKind, CharDiff, CompareOptions, InlineDiff, LineDiff};
pub use normalize::LogNormalizer;
