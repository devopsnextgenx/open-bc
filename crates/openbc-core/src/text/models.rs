//! Public data models for text comparison.

/// The kind of change represented by a diff row.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ChangeKind {
    /// The normalized lines are equal.
    Unchanged,
    /// A line exists only in the right-hand input.
    Added,
    /// A line exists only in the left-hand input.
    Deleted,
    /// Two lines are sufficiently similar to be treated as one modification.
    Modified {
        /// Similarity score from 0 through 100.
        similarity_score: u8,
    },
}

/// One aligned row in a text comparison result.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct LineDiff {
    /// One-based line number in the left input, when present.
    pub left_line_num: Option<usize>,
    /// One-based line number in the right input, when present.
    pub right_line_num: Option<usize>,
    /// Original, unnormalized left-hand line text.
    pub left_text: String,
    /// Original, unnormalized right-hand line text.
    pub right_text: String,
    /// Classification of this aligned row.
    pub kind: ChangeKind,
}

/// Classification for a character-level difference inside an aligned row.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CharChangeKind {
    /// The character range is identical on both sides.
    Equal,
    /// The range contains changed non-whitespace content.
    Mismatch,
    /// The range differs only by spaces or tabs.
    WhitespaceMismatch,
}

/// A UTF-8 byte range in one side of an aligned row.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CharDiff {
    /// Zero-based UTF-8 byte offset.
    pub start: usize,
    /// Length in UTF-8 bytes.
    pub length: usize,
    /// Meaning of this range.
    pub kind: CharChangeKind,
}

/// Character-level differences for both sides of one aligned row.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct InlineDiff {
    /// Ranges in the left line.
    pub left: Vec<CharDiff>,
    /// Ranges in the right line.
    pub right: Vec<CharDiff>,
}

/// Policies applied before comparing lines.
#[derive(Clone, Debug, PartialEq)]
pub struct CompareOptions {
    /// Treat runs of whitespace as insignificant.
    pub ignore_whitespace: bool,
    /// Mask supported timestamp formats before comparison.
    pub ignore_timestamps: bool,
    /// Mask pod names and hexadecimal container identifiers before comparison.
    pub ignore_container_ids: bool,
    /// Minimum Jaro-Winkler score for pairing deleted and added lines.
    pub fuzzy_threshold: f64,
}

impl Default for CompareOptions {
    fn default() -> Self {
        Self {
            ignore_whitespace: false,
            ignore_timestamps: false,
            ignore_container_ids: false,
            fuzzy_threshold: 0.95,
        }
    }
}
