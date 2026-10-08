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

/// Total UTF-8 byte length of a character slice.
fn utf8_len(chars: &[char]) -> usize {
    chars.iter().map(|c| c.len_utf8()).sum()
}

/// Compute character ranges for two already-aligned lines.
///
/// Ranges are UTF-8 byte offsets: an equal prefix, one changed middle, an equal suffix.
#[must_use]
pub fn compute_inline_diff(left: &str, right: &str) -> InlineDiff {
    let left_chars: Vec<char> = left.chars().collect();
    let right_chars: Vec<char> = right.chars().collect();
    let prefix = left_chars.iter().zip(&right_chars).take_while(|(l, r)| l == r).count();
    let suffix = left_chars[prefix..]
        .iter()
        .rev()
        .zip(right_chars[prefix..].iter().rev())
        .take_while(|(l, r)| l == r)
        .count();
    let left_middle = &left_chars[prefix..left_chars.len() - suffix];
    let right_middle = &right_chars[prefix..right_chars.len() - suffix];
    let whitespace_only = left_middle.iter().chain(right_middle).all(|c| *c == ' ' || *c == '\t');
    let kind = if whitespace_only {
        CharChangeKind::WhitespaceMismatch
    } else {
        CharChangeKind::Mismatch
    };
    let side = |chars: &[char], middle: &[char]| {
        build_inline_ranges(
            utf8_len(&chars[..prefix]),
            utf8_len(middle),
            utf8_len(&chars[chars.len() - suffix..]),
            kind,
        )
    };
    InlineDiff {
        left: side(&left_chars, left_middle),
        right: side(&right_chars, right_middle),
    }
}

fn build_inline_ranges(
    start: usize,
    middle_len: usize,
    suffix_len: usize,
    kind: CharChangeKind,
) -> Vec<CharDiff> {
    let mut ranges = Vec::new();
    if start > 0 {
        ranges.push(CharDiff { start: 0, length: start, kind: CharChangeKind::Equal });
    }
    if middle_len > 0 {
        ranges.push(CharDiff { start, length: middle_len, kind });
    }
    if suffix_len > 0 {
        ranges.push(CharDiff {
            start: start + middle_len,
            length: suffix_len,
            kind: CharChangeKind::Equal,
        });
    }
    ranges
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
}
