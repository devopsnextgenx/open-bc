//! `.reg` text to [`RegFile`].

use super::model::{RegError, RegFile, RegKey, RegValue, DELETE_KEY_NAME};
use super::syntax::{type_name, unescape, KIND_DELETE, KIND_DELETE_KEY, KIND_DWORD, KIND_SZ};
use crate::encoding::decode_text;

/// Splits `"quoted name"=rest` / `@=rest`; returns `(name, rest)`.
fn split_name(line: &str) -> Option<(String, &str)> {
    if let Some(rest) = line.strip_prefix("@=") {
        return Some(("@".into(), rest));
    }
    let body = line.strip_prefix('"')?;
    let (mut end, mut escaped) = (None, false);
    for (i, c) in body.char_indices() {
        match (escaped, c) {
            (true, _) => escaped = false,
            (false, '\\') => escaped = true,
            (false, '"') => {
                end = Some(i);
                break;
            }
            _ => {}
        }
    }
    let end = end?;
    Some((unescape(&body[..end]), body[end + 1..].trim_start().strip_prefix('=')?))
}

/// Parses the right-hand side of a `name=...` line into a canonical value.
fn parse_value(name: String, rest: &str) -> RegValue {
    let rest = rest.trim();
    let (kind, data) = if rest == "-" {
        (KIND_DELETE.to_string(), String::new())
    } else if let Some(s) = rest.strip_prefix('"') {
        (KIND_SZ.into(), unescape(s.strip_suffix('"').unwrap_or(s)))
    } else if let Some(d) = rest.strip_prefix("dword:") {
        (KIND_DWORD.into(), d.trim().to_ascii_lowercase())
    } else if let Some(h) = rest.strip_prefix("hex") {
        let (code, bytes) = match h.strip_prefix('(').and_then(|t| t.split_once("):")) {
            Some((code, bytes)) => (code, bytes),
            None => ("3", h.trim_start_matches(':')),
        };
        let canon: Vec<String> = bytes
            .split(',')
            .map(|b| b.trim().to_ascii_lowercase())
            .filter(|b| !b.is_empty())
            .collect();
        (type_name(code), canon.join(","))
    } else {
        (KIND_SZ.into(), rest.to_string())
    };
    RegValue { name, kind, data }
}

/// Builds the key for a `[path]` / `[-path]` header (`inner` is the text between brackets).
fn parse_key_header(inner: &str) -> RegKey {
    match inner.strip_prefix('-') {
        Some(path) => RegKey {
            path: path.to_string(),
            values: vec![RegValue {
                name: DELETE_KEY_NAME.into(),
                kind: KIND_DELETE_KEY.into(),
                data: String::new(),
            }],
        },
        None => RegKey { path: inner.to_string(), values: Vec::new() },
    }
}

/// Joins a `hex:` value continued over several lines with trailing backslashes.
fn join_continuations<'a>(first: &str, rest: &mut impl Iterator<Item = &'a str>) -> String {
    let mut full = first.to_string();
    while full.ends_with('\\') && full.contains("=hex") {
        full.pop();
        match rest.next() {
            Some(next) => full.push_str(next),
            None => break,
        }
    }
    full
}

/// Parses a `.reg` export (UTF-16LE v5.00 or ANSI/UTF-8 REGEDIT4).
pub fn parse_reg(bytes: &[u8]) -> Result<RegFile, RegError> {
    let text = decode_text(bytes);
    let mut lines = text.lines().map(str::trim).filter(|l| !l.is_empty());
    match lines.next() {
        Some(h) if h.starts_with("Windows Registry Editor") || h.starts_with("REGEDIT4") => {}
        _ => return Err(RegError::NotRegistryFile),
    }
    let mut file = RegFile::default();
    while let Some(line) = lines.next() {
        if line.starts_with(';') {
            continue;
        }
        if let Some(inner) = line.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
            file.keys.push(parse_key_header(inner));
            continue;
        }
        let full = join_continuations(line, &mut lines);
        if let (Some(key), Some((name, rest))) = (file.keys.last_mut(), split_name(&full)) {
            key.values.push(parse_value(name, rest));
        }
    }
    Ok(file)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_non_registry_text() {
        assert!(matches!(parse_reg(b"hello"), Err(RegError::NotRegistryFile)));
    }

    #[test]
    fn parses_value_kinds() {
        let v = parse_value("n".into(), "hex(7):41,00,00,00");
        assert_eq!((v.kind.as_str(), v.data.as_str()), ("REG_MULTI_SZ", "41,00,00,00"));
        assert_eq!(parse_value("n".into(), "-").kind, KIND_DELETE);
        assert_eq!(parse_value("n".into(), "\"a\\\\b\"").data, "a\\b");
        assert_eq!(parse_value("n".into(), "hex:AA,BB").data, "aa,bb");
    }
}
