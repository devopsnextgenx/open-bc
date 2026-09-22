//! Log line normalization rules.

use crate::text::models::CompareOptions;
use regex::Regex;
use std::sync::LazyLock;

static TIMESTAMP_REGEX: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(
        r"(?ix)(\d{4}-\d{2}-\d{2}[T ]\d{2}:\d{2}:\d{2}(?:\.\d+)?(?:Z|[+-]\d{2}:?\d{2})?)|(\d{2}:\d{2}:\d{2}\.\d+)|(Mon|Tue|Wed|Thu|Fri|Sat|Sun),?\s+\d{1,2}\s+(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)\s+\d{4}\s+\d{2}:\d{2}:\d{2}\s+(?:UTC|GMT)"
    )
    .expect("timestamp regex is valid")
});

static CONTAINER_ID_REGEX: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"(?i)(pod-[a-z0-9][a-z0-9-]+)|\b[a-f0-9]{12,64}\b")
        .expect("container ID regex is valid")
});

/// Applies configured masking and whitespace normalization to log lines.
pub struct LogNormalizer;

impl LogNormalizer {
    /// Normalize one line according to `options` while preserving its meaning.
    #[must_use]
    pub fn normalize(line: &str, options: &CompareOptions) -> String {
        let mut result = line.to_owned();
        if options.ignore_timestamps {
            result = TIMESTAMP_REGEX.replace_all(&result, "<TS>").into_owned();
        }
        if options.ignore_container_ids {
            result = CONTAINER_ID_REGEX
                .replace_all(&result, "<CONTAINER>")
                .into_owned();
        }
        if options.ignore_whitespace {
            result = result.split_whitespace().collect::<Vec<_>>().join(" ");
        }
        result
    }
}
