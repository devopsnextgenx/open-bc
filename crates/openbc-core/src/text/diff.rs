//! Line-oriented text comparison.

use crate::text::models::{
    ChangeKind, CharChangeKind, CharDiff, CompareOptions, InlineDiff, LineDiff,
};
use crate::text::normalize::LogNormalizer;
use similar::{ChangeTag, TextDiff};
use strsim::jaro_winkler;

/// Synchronous text comparison engine with optional log normalization.
pub struct TextCompareEngine;

/// Compute character ranges for two already-aligned lines.
#[must_use]
pub fn compute_inline_diff(left: &str, right: &str) -> InlineDiff {
    let left_chars: Vec<char> = left.chars().collect();
    let right_chars: Vec<char> = right.chars().collect();
    let mut prefix = 0;
    while prefix < left_chars.len()
        && prefix < right_chars.len()
        && left_chars[prefix] == right_chars[prefix]
    {
        prefix += 1;
    }
    let mut suffix = 0;
    while suffix < left_chars.len().saturating_sub(prefix)
        && suffix < right_chars.len().saturating_sub(prefix)
        && left_chars[left_chars.len() - 1 - suffix] == right_chars[right_chars.len() - 1 - suffix]
    {
        suffix += 1;
    }
    let left_middle = &left_chars[prefix..left_chars.len().saturating_sub(suffix)];
    let right_middle = &right_chars[prefix..right_chars.len().saturating_sub(suffix)];
    let whitespace_only = left_middle
        .iter()
        .chain(right_middle)
        .all(|c| *c == ' ' || *c == '\t');
    let kind = if whitespace_only {
        CharChangeKind::WhitespaceMismatch
    } else {
        CharChangeKind::Mismatch
    };
    let left_start: usize = left_chars[..prefix].iter().map(|c| c.len_utf8()).sum();
    let right_start: usize = right_chars[..prefix].iter().map(|c| c.len_utf8()).sum();
    let left_middle_len: usize = left_middle.iter().map(|c| c.len_utf8()).sum();
    let right_middle_len: usize = right_middle.iter().map(|c| c.len_utf8()).sum();
    let left_suffix_len: usize = left_chars[left_chars.len().saturating_sub(suffix)..]
        .iter()
        .map(|c| c.len_utf8())
        .sum();
    let right_suffix_len: usize = right_chars[right_chars.len().saturating_sub(suffix)..]
        .iter()
        .map(|c| c.len_utf8())
        .sum();
    InlineDiff {
        left: build_inline_ranges(left_start, left_middle_len, left_suffix_len, kind),
        right: build_inline_ranges(right_start, right_middle_len, right_suffix_len, kind),
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
        ranges.push(CharDiff {
            start: 0,
            length: start,
            kind: CharChangeKind::Equal,
        });
    }
    if middle_len > 0 {
        ranges.push(CharDiff {
            start,
            length: middle_len,
            kind,
        });
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

impl TextCompareEngine {
    /// Compare two UTF-8 buffers and return aligned rows containing original text.
    #[must_use]
    pub fn compare_buffers(left: &str, right: &str, options: &CompareOptions) -> Vec<LineDiff> {
        let left_raw: Vec<&str> = left.lines().collect();
        let right_raw: Vec<&str> = right.lines().collect();
        let left_normalized: Vec<String> = left_raw
            .iter()
            .map(|line| LogNormalizer::normalize(line, options))
            .collect();
        let right_normalized: Vec<String> = right_raw
            .iter()
            .map(|line| LogNormalizer::normalize(line, options))
            .collect();
        let left_normalized_refs: Vec<&str> = left_normalized.iter().map(String::as_str).collect();
        let right_normalized_refs: Vec<&str> =
            right_normalized.iter().map(String::as_str).collect();

        let diff = TextDiff::from_slices(&left_normalized_refs, &right_normalized_refs);
        let mut results = Vec::new();
        let mut left_index = 0;
        let mut right_index = 0;

        for change in diff.iter_all_changes() {
            match change.tag() {
                ChangeTag::Equal => {
                    results.push(LineDiff {
                        left_line_num: Some(left_index + 1),
                        right_line_num: Some(right_index + 1),
                        left_text: left_raw[left_index].to_owned(),
                        right_text: right_raw[right_index].to_owned(),
                        kind: ChangeKind::Unchanged,
                    });
                    left_index += 1;
                    right_index += 1;
                }
                ChangeTag::Delete => {
                    results.push(LineDiff {
                        left_line_num: Some(left_index + 1),
                        right_line_num: None,
                        left_text: left_raw[left_index].to_owned(),
                        right_text: String::new(),
                        kind: ChangeKind::Deleted,
                    });
                    left_index += 1;
                }
                ChangeTag::Insert => {
                    let inserted = right_raw[right_index];
                    let inserted_normalized = &right_normalized[right_index];
                    let match_index = results.iter().rposition(|row| {
                        row.kind == ChangeKind::Deleted
                            && row.right_line_num.is_none()
                            && row.left_line_num.is_some_and(|line| {
                                jaro_winkler(&left_normalized[line - 1], inserted_normalized)
                                    >= options.fuzzy_threshold
                            })
                    });

                    if let Some(result_index) = match_index {
                        let left_line = results[result_index]
                            .left_line_num
                            .expect("deleted rows have a left line number");
                        let score =
                            (jaro_winkler(&left_normalized[left_line - 1], inserted_normalized)
                                * 100.0)
                                .round()
                                .clamp(0.0, 100.0) as u8;
                        let result = &mut results[result_index];
                        result.right_line_num = Some(right_index + 1);
                        result.right_text = inserted.to_owned();
                        result.kind = ChangeKind::Modified {
                            similarity_score: score,
                        };
                    } else {
                        results.push(LineDiff {
                            left_line_num: None,
                            right_line_num: Some(right_index + 1),
                            left_text: String::new(),
                            right_text: inserted.to_owned(),
                            kind: ChangeKind::Added,
                        });
                    }
                    right_index += 1;
                }
            }
        }
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
    fn inline_diff_uses_utf8_byte_ranges() {
        let result = compute_inline_diff("préfix old", "préfix new");
        assert_eq!(result.left[0].kind, CharChangeKind::Equal);
        assert_eq!(result.left[1].kind, CharChangeKind::Mismatch);
        assert_eq!(result.left[1].start, "préfix ".len());
    }
}
