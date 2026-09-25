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

fn append_change_block(
    results: &mut Vec<LineDiff>,
    deleted: &[usize],
    inserted: &[usize],
    left_raw: &[&str],
    right_raw: &[&str],
    left_normalized: &[String],
    right_normalized: &[String],
    fuzzy_threshold: f64,
) {
    for (&left_index, &right_index) in deleted.iter().zip(inserted) {
        let similarity = jaro_winkler(&left_normalized[left_index], &right_normalized[right_index]);
        if similarity >= fuzzy_threshold {
            results.push(LineDiff {
                left_line_num: Some(left_index + 1),
                right_line_num: Some(right_index + 1),
                left_text: left_raw[left_index].to_owned(),
                right_text: right_raw[right_index].to_owned(),
                kind: ChangeKind::Modified {
                    similarity_score: (similarity * 100.0).round().clamp(0.0, 100.0) as u8,
                },
            });
        } else {
            results.push(LineDiff {
                left_line_num: Some(left_index + 1),
                right_line_num: None,
                left_text: left_raw[left_index].to_owned(),
                right_text: String::new(),
                kind: ChangeKind::Deleted,
            });
            results.push(LineDiff {
                left_line_num: None,
                right_line_num: Some(right_index + 1),
                left_text: String::new(),
                right_text: right_raw[right_index].to_owned(),
                kind: ChangeKind::Added,
            });
        }
    }
    for &left_index in deleted.iter().skip(inserted.len()) {
        results.push(LineDiff {
            left_line_num: Some(left_index + 1),
            right_line_num: None,
            left_text: left_raw[left_index].to_owned(),
            right_text: String::new(),
            kind: ChangeKind::Deleted,
        });
    }
    for &right_index in inserted.iter().skip(deleted.len()) {
        results.push(LineDiff {
            left_line_num: None,
            right_line_num: Some(right_index + 1),
            left_text: String::new(),
            right_text: right_raw[right_index].to_owned(),
            kind: ChangeKind::Added,
        });
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
        let started = std::time::Instant::now();
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
        let mut deleted = Vec::new();
        let mut inserted = Vec::new();

        for change in diff.iter_all_changes() {
            match change.tag() {
                ChangeTag::Equal => {
                    append_change_block(
                        &mut results,
                        &deleted,
                        &inserted,
                        &left_raw,
                        &right_raw,
                        &left_normalized,
                        &right_normalized,
                        options.fuzzy_threshold,
                    );
                    deleted.clear();
                    inserted.clear();
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
                    deleted.push(left_index);
                    left_index += 1;
                }
                ChangeTag::Insert => {
                    inserted.push(right_index);
                    right_index += 1;
                }
            }
        }
        append_change_block(
            &mut results,
            &deleted,
            &inserted,
            &left_raw,
            &right_raw,
            &left_normalized,
            &right_normalized,
            options.fuzzy_threshold,
        );
        openbc_observability::log(
            "openbc-core",
            "TextCompareEngine::compare_buffers",
            "INFO",
            "text comparison complete",
        );
        openbc_observability::record(
            "openbc-core",
            "file_compare",
            started.elapsed(),
            Some((left.len(), right.len())),
            Some((left_raw.len(), right_raw.len())),
            None,
            None,
            Some("CPU diff".to_owned()),
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
