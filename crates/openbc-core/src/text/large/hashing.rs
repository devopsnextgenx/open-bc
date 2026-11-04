use std::collections::hash_map::DefaultHasher;
use std::hash::{Hash, Hasher};

/// Hash a single line in a stable, deterministic way.
#[must_use]
pub fn hash_line(input: &[u8]) -> u64 {
    let mut hasher = DefaultHasher::new();
    input.hash(&mut hasher);
    hasher.finish()
}

/// Apply the same normalization strategy used by the line diff pipeline: strip trailing carriage
/// returns, trim surrounding whitespace, and preserve only the significant bytes.
#[must_use]
pub fn normalize_line_bytes(input: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(input.len());
    let mut saw_content = false;
    for &byte in input {
        if byte == b'\r' {
            continue;
        }
        if byte.is_ascii_whitespace() {
            if saw_content {
                out.push(b' ');
            }
            continue;
        }
        out.push(byte);
        saw_content = true;
    }
    while out.last().is_some_and(|&byte| byte == b' ') {
        out.pop();
    }
    out
}

#[cfg(test)]
mod tests {
    use super::{hash_line, normalize_line_bytes};

    #[test]
    fn hash_line_is_stable() {
        let first = hash_line(b"alpha\n");
        let second = hash_line(b"alpha\n");
        assert_eq!(first, second);
    }

    #[test]
    fn normalize_line_bytes_removes_carriage_return_and_trims_space() {
        let normalized = normalize_line_bytes(b"  a\r\n");
        assert_eq!(normalized, b"a");
    }
}
