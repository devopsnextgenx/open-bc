//! Line-oriented text comparison.

use crate::text::models::{ChangeKind, CompareOptions, LineDiff};
use crate::text::normalize::LogNormalizer;
use similar::{ChangeTag, TextDiff};
use strsim::jaro_winkler;

/// Synchronous text comparison engine with optional log normalization.
pub struct TextCompareEngine;

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
}
