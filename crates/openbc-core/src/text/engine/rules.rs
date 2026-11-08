//! Ignore rules: parts of a line that must not influence matching.
//!
//! Same behaviour as the Rules bar of the existing text viewer, so one rule text means the same
//! thing in both editors:
//!
//! * entries are separated by commas, `,,` is a literal comma, entries are not trimmed;
//! * a plain entry is a *sample*: its shape is matched (digit runs match any digits, every other
//!   character is literal) - timestamps, UUIDs, pod ids, counters;
//! * an entry written as `/pattern/` is a regular expression (Rust `regex` syntax);
//! * every match is replaced by the token `<SAMPLE>` before hashing and comparing, so lines that
//!   differ only inside matches are *matched*, and a line that a rule covers completely matches any
//!   other line covered the same way.
//!
//! Lines longer than [`MAX_RULE_LINE_BYTES`] are matched without rules (deterministically on both
//! sides) so a pathological single-line file never forces unbounded buffering.

use std::borrow::Cow;

use regex::Regex;

use crate::text::normalize::sample_pattern;

/// Longest line (bytes) that rules are applied to.
pub const MAX_RULE_LINE_BYTES: usize = 1 << 20;

/// Replacement token written in place of every match.
pub const RULE_TOKEN: &str = "<SAMPLE>";

/// Compiled ignore rules.
#[derive(Debug)]
pub struct LineRules {
    patterns: Vec<Regex>,
    key: String,
}

impl LineRules {
    /// Parse a rule text. Returns `Ok(None)` when it contains no usable entry. Invalid regular
    /// expressions are reported (all valid entries are still not applied, so a typo never silently
    /// changes the comparison).
    pub fn parse(text: &str) -> Result<Option<Self>, String> {
        let mut patterns = Vec::new();
        for entry in split_entries(text) {
            let regex = if entry.len() >= 3 && entry.starts_with('/') && entry.ends_with('/') {
                Regex::new(&entry[1..entry.len() - 1])
                    .map_err(|e| format!("invalid regular expression {entry}: {e}"))?
            } else {
                match sample_pattern(&entry) {
                    Some(r) => r,
                    None => continue,
                }
            };
            patterns.push(regex);
        }
        if patterns.is_empty() {
            return Ok(None);
        }
        Ok(Some(Self { patterns, key: text.to_owned() }))
    }

    /// The rule text this was built from; two indexes may only be diffed with equal keys.
    #[must_use]
    pub fn key(&self) -> &str {
        &self.key
    }

    /// Apply the rules to one line (UTF-8 bytes). Borrowed when nothing matched, so invalid UTF-8 or
    /// unaffected lines are never altered.
    #[must_use]
    pub fn apply<'a>(&self, line: &'a [u8]) -> Cow<'a, [u8]> {
        if line.len() > MAX_RULE_LINE_BYTES {
            return Cow::Borrowed(line);
        }
        let text = String::from_utf8_lossy(line);
        let mut current: Cow<str> = text;
        let mut changed = false;
        for pattern in &self.patterns {
            if let Cow::Owned(replaced) = pattern.replace_all(&current, RULE_TOKEN) {
                current = Cow::Owned(replaced);
                changed = true;
            }
        }
        if changed {
            Cow::Owned(current.into_owned().into_bytes())
        } else {
            Cow::Borrowed(line)
        }
    }
}

/// Same splitting as the existing viewer (`parse_ignore_samples`): `,` separates, `,,` is a comma.
fn split_entries(input: &str) -> Vec<String> {
    let mut entries = Vec::new();
    let mut current = String::new();
    let mut chars = input.chars().peekable();
    while let Some(c) = chars.next() {
        if c == ',' {
            if chars.peek() == Some(&',') {
                current.push(',');
                chars.next();
            } else if !current.is_empty() {
                entries.push(std::mem::take(&mut current));
            }
        } else {
            current.push(c);
        }
    }
    if !current.is_empty() {
        entries.push(current);
    }
    entries
}

#[cfg(test)]
mod tests {
    use super::*;

    fn apply(rules: &str, line: &str) -> String {
        let r = LineRules::parse(rules).unwrap().unwrap();
        String::from_utf8(r.apply(line.as_bytes()).into_owned()).unwrap()
    }

    #[test]
    fn samples_match_by_shape_like_the_existing_rules_bar() {
        assert_eq!(apply("35,,964", "value=35,964"), "value=<SAMPLE>");
        assert_eq!(apply("35,964", "value=35,964"), "value=<SAMPLE>,<SAMPLE>"); // two entries
        assert_eq!(apply("2026-09-21T10:00:00Z", "at 2027-12-31T23:59:59Z ok"), "at <SAMPLE> ok");
        assert_eq!(apply("a,,b", "x a,b y"), "x <SAMPLE> y");
    }

    #[test]
    fn regex_entries_and_multiple_entries() {
        assert_eq!(apply("/^#.*$/", "# a comment"), "<SAMPLE>");
        assert_eq!(apply("/^#.*$/", "code # not a comment"), "code # not a comment");
        assert_eq!(apply("pod-1,/id=[a-f0-9]+/", "pod-7 id=9af3 up"), "<SAMPLE> <SAMPLE> up");
    }

    #[test]
    fn empty_and_invalid_rule_text() {
        assert!(LineRules::parse("").unwrap().is_none());
        assert!(LineRules::parse(",,,").unwrap().is_some()); // `,,` is a literal comma sample
        assert!(LineRules::parse("/(unclosed/").is_err());
        assert!(LineRules::parse("//").unwrap().is_some()); // "//" is a plain sample, not a regex
    }

    #[test]
    fn unaffected_and_invalid_utf8_lines_are_untouched() {
        let r = LineRules::parse("xyz").unwrap().unwrap();
        let raw = b"a\xFFb";
        assert!(matches!(r.apply(raw), Cow::Borrowed(_)));
        let huge = vec![b'x'; MAX_RULE_LINE_BYTES + 1];
        assert!(matches!(r.apply(&huge), Cow::Borrowed(_)));
    }
}
