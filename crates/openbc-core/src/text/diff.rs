//! Line-oriented text comparison.

use std::time::Instant;

use crate::telemetry::{self, Sample};
use crate::text::models::{
    ChangeKind, CharChangeKind, CharDiff, CompareOptions, InlineDiff, LineDiff,
};
use crate::text::normalize::PreparedNormalizer;
use similar::{ChangeTag, TextDiff};
use strsim::jaro_winkler;

/// Synchronous text comparison engine with optional log normalization.
pub struct TextCompareEngine;

/// Compute word-aware ranges for two already-aligned lines.
///
/// Words and separators are diffed independently, so multiple changes on one line do not
/// highlight the unchanged text between them. Ranges are UTF-8 byte offsets.
#[must_use]
pub fn compute_inline_diff(left: &str, right: &str) -> InlineDiff {
    let left_tokens = inline_tokens(left);
    let right_tokens = inline_tokens(right);
    let left_values: Vec<&str> = left_tokens.iter().map(|token| token.text).collect();
    let right_values: Vec<&str> = right_tokens.iter().map(|token| token.text).collect();
    let diff = TextDiff::from_slices(&left_values, &right_values);
    let (mut left, mut right) = (Vec::new(), Vec::new());
    let (mut left_index, mut right_index) = (0, 0);

    for change in diff.iter_all_changes() {
        match change.tag() {
            ChangeTag::Equal => {
                append_inline_token(&mut left, left_tokens[left_index], CharChangeKind::Equal);
                append_inline_token(&mut right, right_tokens[right_index], CharChangeKind::Equal);
                left_index += 1;
                right_index += 1;
            }
            ChangeTag::Delete => {
                let token = left_tokens[left_index];
                append_inline_token(&mut left, token, token.change_kind());
                left_index += 1;
            }
            ChangeTag::Insert => {
                let token = right_tokens[right_index];
                append_inline_token(&mut right, token, token.change_kind());
                right_index += 1;
            }
        }
    }
    InlineDiff { left, right }
}

#[derive(Clone, Copy)]
struct InlineToken<'a> {
    text: &'a str,
    start: usize,
    kind: InlineTokenKind,
}

#[derive(Clone, Copy, Eq, PartialEq)]
enum InlineTokenKind {
    Word,
    Whitespace,
    Punctuation,
}

impl InlineToken<'_> {
    fn change_kind(self) -> CharChangeKind {
        if self.kind == InlineTokenKind::Whitespace {
            CharChangeKind::WhitespaceMismatch
        } else {
            CharChangeKind::Mismatch
        }
    }
}

fn inline_tokens(text: &str) -> Vec<InlineToken<'_>> {
    let mut tokens = Vec::new();
    let mut start = 0;
    let mut active_kind = None;

    for (offset, ch) in text.char_indices() {
        let kind = if ch.is_alphanumeric() || ch == '_' {
            InlineTokenKind::Word
        } else if ch.is_whitespace() {
            InlineTokenKind::Whitespace
        } else {
            InlineTokenKind::Punctuation
        };
        if active_kind == Some(kind) && kind != InlineTokenKind::Punctuation {
            continue;
        }
        if let Some(previous_kind) = active_kind {
            tokens.push(InlineToken {
                text: &text[start..offset],
                start,
                kind: previous_kind,
            });
        }
        start = offset;
        active_kind = Some(kind);
    }
    if start < text.len() {
        tokens.push(InlineToken {
            text: &text[start..],
            start,
            kind: active_kind.expect("non-empty token has a kind"),
        });
    }
    tokens
}

fn append_inline_token(ranges: &mut Vec<CharDiff>, token: InlineToken<'_>, kind: CharChangeKind) {
    let length = token.text.len();
    if let Some(previous) = ranges.last_mut() {
        if previous.kind == kind && previous.start + previous.length == token.start {
            previous.length += length;
            return;
        }
    }
    ranges.push(CharDiff {
        start: token.start,
        length,
        kind,
    });
}

/// Both inputs split into lines, with the raw text (reported) and normalized text (compared).
struct Lines<'a> {
    left_raw: Vec<&'a str>,
    right_raw: Vec<&'a str>,
    left_normalized: Vec<String>,
    right_normalized: Vec<String>,
}

impl<'a> Lines<'a> {
    fn new(left: &'a str, right: &'a str, options: &CompareOptions) -> Self {
        let normalizer = PreparedNormalizer::new(options);
        let split = |text: &'a str| -> (Vec<&'a str>, Vec<String>) {
            let raw: Vec<&str> = text.lines().collect();
            let normalized = raw.iter().map(|line| normalizer.apply(line)).collect();
            (raw, normalized)
        };
        let (left_raw, left_normalized) = split(left);
        let (right_raw, right_normalized) = split(right);
        Self { left_raw, right_raw, left_normalized, right_normalized }
    }

    fn unchanged(&self, left: usize, right: usize) -> LineDiff {
        LineDiff {
            left_line_num: Some(left + 1),
            right_line_num: Some(right + 1),
            left_text: self.left_raw[left].to_owned(),
            right_text: self.right_raw[right].to_owned(),
            kind: ChangeKind::Unchanged,
        }
    }

    fn modified(&self, left: usize, right: usize, similarity: f64) -> LineDiff {
        LineDiff {
            kind: ChangeKind::Modified {
                similarity_score: (similarity * 100.0).round().clamp(0.0, 100.0) as u8,
            },
            ..self.unchanged(left, right)
        }
    }

    fn deleted(&self, left: usize) -> LineDiff {
        LineDiff {
            left_line_num: Some(left + 1),
            right_line_num: None,
            left_text: self.left_raw[left].to_owned(),
            right_text: String::new(),
            kind: ChangeKind::Deleted,
        }
    }

    fn added(&self, right: usize) -> LineDiff {
        LineDiff {
            left_line_num: None,
            right_line_num: Some(right + 1),
            left_text: String::new(),
            right_text: self.right_raw[right].to_owned(),
            kind: ChangeKind::Added,
        }
    }

    /// Emits one change block: deleted/inserted lines are paired positionally, a pair
    /// becomes a modification when similar enough, and leftovers stay deleted/added.
    fn flush_block(
        &self,
        results: &mut Vec<LineDiff>,
        deleted: &[usize],
        inserted: &[usize],
        fuzzy_threshold: f64,
    ) {
        for (&left, &right) in deleted.iter().zip(inserted) {
            let similarity = jaro_winkler(&self.left_normalized[left], &self.right_normalized[right]);
            if similarity >= fuzzy_threshold {
                results.push(self.modified(left, right, similarity));
            } else {
                results.push(self.deleted(left));
                results.push(self.added(right));
            }
        }
        results.extend(deleted.iter().skip(inserted.len()).map(|&left| self.deleted(left)));
        results.extend(inserted.iter().skip(deleted.len()).map(|&right| self.added(right)));
    }
}

impl TextCompareEngine {
    /// Compare two UTF-8 buffers and return aligned rows containing original text.
    #[must_use]
    pub fn compare_buffers(left: &str, right: &str, options: &CompareOptions) -> Vec<LineDiff> {
        let started = Instant::now();
        let lines = Lines::new(left, right, options);
        let left_refs: Vec<&str> = lines.left_normalized.iter().map(String::as_str).collect();
        let right_refs: Vec<&str> = lines.right_normalized.iter().map(String::as_str).collect();

        let diff = TextDiff::from_slices(&left_refs, &right_refs);
        let mut results = Vec::new();
        let (mut left_index, mut right_index) = (0, 0);
        let (mut deleted, mut inserted) = (Vec::new(), Vec::new());

        for change in diff.iter_all_changes() {
            match change.tag() {
                ChangeTag::Equal => {
                    lines.flush_block(&mut results, &deleted, &inserted, options.fuzzy_threshold);
                    deleted.clear();
                    inserted.clear();
                    results.push(lines.unchanged(left_index, right_index));
                    left_index += 1;
                    right_index += 1;
                }
                ChangeTag::Delete => {
                    deleted.push(left_index);
                    left_index += 1;
                }
                ChangeTag::Insert => {
                    inserted.push(right_index);
                    right_index += 1;
                }
            }
        }
        lines.flush_block(&mut results, &deleted, &inserted, options.fuzzy_threshold);

        telemetry::info("TextCompareEngine::compare_buffers", "text comparison complete");
        telemetry::record(
            "file_compare",
            started,
            Sample {
                sizes: Some((left.len(), right.len())),
                lines: Some((lines.left_raw.len(), lines.right_raw.len())),
                detail: Some("CPU diff".to_owned()),
                ..Sample::default()
            },
        );
        results
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn normalizes_timestamps_and_pairs_fuzzy_changes() {
        let options = CompareOptions {
            ignore_timestamps: true,
            fuzzy_threshold: 0.9,
            ..CompareOptions::default()
        };
        let result = TextCompareEngine::compare_buffers(
            "2026-09-21T10:00:00Z started task",
            "2026-09-21T10:01:00Z started tasks",
            &options,
        );
        assert!(matches!(result[0].kind, ChangeKind::Modified { .. }));
    }

    #[test]
    fn masks_date_sample_with_different_digits() {
        let options = CompareOptions {
            ignore_samples: vec!["2026-09-21T10:00:00Z".to_owned()],
            ..CompareOptions::default()
        };
        let result = TextCompareEngine::compare_buffers(
            "2026-09-21T10:00:00Z started task",
            "2027-12-31T23:59:59Z started task",
            &options,
        );

        assert_eq!(result.len(), 1);
        assert_eq!(result[0].kind, ChangeKind::Unchanged);
    }

    #[test]
    fn pairs_config_value_replacement_for_inline_diff() {
        let result = TextCompareEngine::compare_buffers(
            "{\n  \"server\": \"staging\",\n  \"port\": 8080\n}",
            "{\n  \"server\": \"production\",\n  \"port\": 8085\n}",
            &CompareOptions::default(),
        );

        assert!(matches!(result[1].kind, ChangeKind::Modified { .. }));
        assert_eq!(result[1].left_text, "  \"server\": \"staging\",");
        assert_eq!(result[1].right_text, "  \"server\": \"production\",");
    }

    #[test]
    fn does_not_reorder_rows_when_changed_lines_are_reordered() {
        let options = CompareOptions {
            fuzzy_threshold: 0.9,
            ..CompareOptions::default()
        };
        let result = TextCompareEngine::compare_buffers("alpha\nbeta", "beta\nalpha", &options);

        assert_eq!(
            result
                .iter()
                .filter_map(|row| row.left_line_num.map(|_| row.left_text.as_str()))
                .collect::<Vec<_>>(),
            ["alpha", "beta"]
        );
        assert_eq!(
            result
                .iter()
                .filter_map(|row| row.right_line_num.map(|_| row.right_text.as_str()))
                .collect::<Vec<_>>(),
            ["beta", "alpha"]
        );
    }

    #[test]
    fn inline_diff_uses_utf8_byte_ranges() {
        let result = compute_inline_diff("préfix old", "préfix new");
        assert_eq!(result.left[0].kind, CharChangeKind::Equal);
        assert_eq!(result.left[1].kind, CharChangeKind::Mismatch);
        assert_eq!(result.left[1].start, "préfix ".len());
    }

    #[test]
    fn inline_diff_matches_unchanged_words_between_multiple_insertions() {
        let left = "# 2. Integrate Rust Cargo via Corrosion";
        let right = "# 2. Integrate in Rust Cargo via Corrosion rust";
        let result = compute_inline_diff(left, right);

        let mismatches = |text: &str, ranges: &[CharDiff]| {
            ranges
                .iter()
                .filter(|range| range.kind == CharChangeKind::Mismatch)
                .map(|range| text[range.start..range.start + range.length].to_owned())
                .collect::<Vec<_>>()
        };
        assert!(mismatches(left, &result.left).is_empty());
        assert_eq!(mismatches(right, &result.right), ["in", "rust"]);
        for word in ["Rust", "Cargo", "via", "Corrosion"] {
            assert!(result.left.iter().any(|range| {
                range.kind == CharChangeKind::Equal
                    && left[range.start..range.start + range.length].contains(word)
            }));
        }
    }
}
