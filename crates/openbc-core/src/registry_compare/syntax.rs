//! Vocabulary of the `.reg` format: value-kind names, type codes and string escaping.

/// Canonical kind names used in [`RegValue::kind`](super::RegValue::kind).
pub(super) const KIND_SZ: &str = "REG_SZ";
pub(super) const KIND_EXPAND_SZ: &str = "REG_EXPAND_SZ";
pub(super) const KIND_BINARY: &str = "REG_BINARY";
pub(super) const KIND_DWORD: &str = "REG_DWORD";
pub(super) const KIND_LINK: &str = "REG_LINK";
pub(super) const KIND_MULTI_SZ: &str = "REG_MULTI_SZ";
/// A `name=-` line.
pub(super) const KIND_DELETE: &str = "DELETE";
/// A `[-HKEY...]` key deletion.
pub(super) const KIND_DELETE_KEY: &str = "DELETE_KEY";

/// `hex(t):` type code (lowercase hex digit) to canonical kind name.
const TYPES: [(&str, &str); 12] = [
    ("0", "REG_NONE"),
    ("1", KIND_SZ),
    ("2", KIND_EXPAND_SZ),
    ("3", KIND_BINARY),
    ("4", KIND_DWORD),
    ("5", "REG_DWORD_BIG_ENDIAN"),
    ("6", KIND_LINK),
    ("7", KIND_MULTI_SZ),
    ("8", "REG_RESOURCE_LIST"),
    ("9", "REG_FULL_RESOURCE_DESCRIPTOR"),
    ("a", "REG_RESOURCE_REQUIREMENTS_LIST"),
    ("b", "REG_QWORD"),
];

/// Kind name for a `hex(code):` type code; unknown codes keep the `hex(code)` spelling.
pub(super) fn type_name(code: &str) -> String {
    let code = code.to_ascii_lowercase();
    TYPES
        .iter()
        .find(|(c, _)| *c == code)
        .map_or_else(|| format!("hex({code})"), |(_, name)| name.to_string())
}

/// Inverse of [`type_name`].
pub(super) fn type_code(kind: &str) -> String {
    TYPES.iter().find(|(_, name)| *name == kind).map_or_else(
        || kind.trim_start_matches("hex(").trim_end_matches(')').to_string(),
        |(code, _)| code.to_string(),
    )
}

/// Resolves `\\` and `\"` escapes (any backslash escapes the following character).
pub(super) fn unescape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut chars = s.chars();
    while let Some(c) = chars.next() {
        if c == '\\' {
            out.push(chars.next().unwrap_or('\\'));
        } else {
            out.push(c);
        }
    }
    out
}

/// Escapes backslashes and double quotes for a quoted `.reg` string.
pub(super) fn escape(s: &str) -> String {
    s.replace('\\', "\\\\").replace('"', "\\\"")
}
