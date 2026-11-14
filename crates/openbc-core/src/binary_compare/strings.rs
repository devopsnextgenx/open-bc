//! Format-independent string extraction from raw section bytes.
//!
//! LIEF exposes strings for some formats only, so the same scanner runs over the content of every
//! non-executable section. Both printable ASCII and UTF-16LE (common in PE resources and
//! `wchar_t` literals) are found.

/// Default minimum length, matching `strings(1)`.
pub const DEFAULT_MIN_LEN: usize = 4;

/// Upper bound on collected strings, so a pathological file cannot exhaust memory.
pub const MAX_STRINGS: usize = 200_000;

/// Longest string kept; longer runs are truncated at a char boundary.
pub const MAX_STRING_LEN: usize = 512;

fn printable(b: u8) -> bool {
    b == b'\t' || (0x20..0x7F).contains(&b)
}

fn push(out: &mut Vec<String>, s: String) {
    let mut s = s;
    if s.len() > MAX_STRING_LEN {
        let mut cut = MAX_STRING_LEN;
        while !s.is_char_boundary(cut) {
            cut -= 1;
        }
        s.truncate(cut);
    }
    out.push(s);
}

/// Extract ASCII and UTF-16LE strings of at least `min_len` characters, in file order
/// (ASCII first, then UTF-16LE), without deduplication.
#[must_use]
pub fn extract_strings(data: &[u8], min_len: usize) -> Vec<String> {
    let min_len = min_len.max(1);
    let mut out = Vec::new();

    // ASCII runs.
    let mut start = None;
    for (i, &b) in data.iter().enumerate() {
        match (printable(b), start) {
            (true, None) => start = Some(i),
            (false, Some(s)) => {
                if i - s >= min_len {
                    push(&mut out, String::from_utf8_lossy(&data[s..i]).into_owned());
                }
                start = None;
            }
            _ => {}
        }
        if out.len() >= MAX_STRINGS {
            return out;
        }
    }
    if let Some(s) = start {
        if data.len() - s >= min_len {
            push(&mut out, String::from_utf8_lossy(&data[s..]).into_owned());
        }
    }

    // UTF-16LE runs: printable byte followed by 0x00.
    let mut run = String::new();
    let mut i = 0;
    while i + 1 < data.len() && out.len() < MAX_STRINGS {
        // A run may not *start* right after a printable byte: that byte belongs to the ASCII
        // string before it (`ab\0` + `w\0i\0` must not read as `bwi`).
        let may_start = !run.is_empty() || i == 0 || !printable(data[i - 1]);
        if printable(data[i]) && data[i + 1] == 0 && may_start {
            run.push(char::from(data[i]));
            i += 2;
        } else {
            if run.chars().count() >= min_len {
                push(&mut out, std::mem::take(&mut run));
            }
            run.clear();
            i += 1;
        }
    }
    if run.chars().count() >= min_len && out.len() < MAX_STRINGS {
        push(&mut out, run);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_ascii_and_utf16() {
        let mut data = b"\0\0hello world\0ab\0".to_vec();
        for c in "wide str".chars() {
            data.push(c as u8);
            data.push(0);
        }
        data.extend_from_slice(&[0xFF, 0xFF]);
        let found = extract_strings(&data, 4);
        assert!(found.contains(&"hello world".to_owned()));
        assert!(found.contains(&"wide str".to_owned()));
        assert!(!found.iter().any(|s| s == "ab"));
    }

    #[test]
    fn respects_min_len_and_caps_length() {
        assert!(extract_strings(b"abc\0", 4).is_empty());
        let long = vec![b'x'; 2000];
        assert_eq!(extract_strings(&long, 4)[0].len(), MAX_STRING_LEN);
    }
}
