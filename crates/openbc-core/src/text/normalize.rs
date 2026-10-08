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
    ///
    /// Compiles the `ignore_samples` patterns on every call; when normalizing many
    /// lines with the same options, use the crate-internal `PreparedNormalizer` instead.
    #[must_use]
    pub fn normalize(line: &str, options: &CompareOptions) -> String {
        PreparedNormalizer::new(options).apply(line)
    }
}

/// [`CompareOptions`] with the per-sample regexes compiled once, for normalizing many lines.
pub(crate) struct PreparedNormalizer<'a> {
    options: &'a CompareOptions,
    samples: Vec<Regex>,
}

impl<'a> PreparedNormalizer<'a> {
    /// Compiles the sample patterns; samples that yield no valid pattern are skipped.
    pub(crate) fn new(options: &'a CompareOptions) -> Self {
        let samples = options.ignore_samples.iter().filter_map(|s| sample_pattern(s)).collect();
        Self { options, samples }
    }

    /// Normalize one line (timestamps, container ids, samples, then whitespace).
    pub(crate) fn apply(&self, line: &str) -> String {
        let mut result = line.to_owned();
        if self.options.ignore_timestamps {
            result = TIMESTAMP_REGEX.replace_all(&result, "<TS>").into_owned();
        }
        if self.options.ignore_container_ids {
            result = CONTAINER_ID_REGEX.replace_all(&result, "<CONTAINER>").into_owned();
        }
        for pattern in &self.samples {
            result = pattern.replace_all(&result, "<SAMPLE>").into_owned();
        }
        if self.options.ignore_whitespace {
            result = result.split_whitespace().collect::<Vec<_>>().join(" ");
        }
        result
    }
}

/// Turns a sample value into a regex matching the same *shape*: digit runs become `\d+`,
/// every other character matches literally.
fn sample_pattern(sample: &str) -> Option<Regex> {
    if sample.is_empty() {
        return None;
    }
    let mut pattern = String::new();
    let mut digits = false;
    for character in sample.chars() {
        if character.is_ascii_digit() {
            if !digits {
                pattern.push_str(r"\d+");
                digits = true;
            }
        } else {
            pattern.push_str(&regex::escape(&character.to_string()));
            digits = false;
        }
    }
    Regex::new(&pattern).ok()
}

#[cfg(test)]
mod tests {
    use super::{LogNormalizer, PreparedNormalizer};
    use crate::text::models::CompareOptions;

    #[test]
    fn sample_uses_the_current_string_shape() {
        let options = CompareOptions {
            ignore_samples: vec!["35,964".to_owned()],
            ..CompareOptions::default()
        };

        assert_eq!(
            LogNormalizer::normalize("value=35,964", &options),
            "value=<SAMPLE>"
        );
        assert_eq!(
            LogNormalizer::normalize("value=00:29:35,964", &options),
            "value=00:29:<SAMPLE>"
        );
    }

    #[test]
    fn prepared_normalizer_matches_one_shot_and_is_reusable() {
        let options = CompareOptions {
            ignore_timestamps: true,
            ignore_container_ids: true,
            ignore_whitespace: true,
            ignore_samples: vec![String::new(), "7,5".to_owned()],
            ..CompareOptions::default()
        };
        let prepared = PreparedNormalizer::new(&options);
        for line in ["2026-09-21T10:00:00Z  pod-abc12 took 12,34", "plain   text"] {
            assert_eq!(prepared.apply(line), LogNormalizer::normalize(line, &options));
        }
        assert_eq!(prepared.apply("2026-09-21T10:00:00Z pod-abc12 took 12,34"), "<TS> <CONTAINER> took <SAMPLE>");
    }
}
