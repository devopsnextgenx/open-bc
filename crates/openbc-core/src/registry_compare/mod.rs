//! Windows registry export (`.reg`) parsing, comparison, copy edits and writing.
//!
//! Values are kept in a canonical text form so comparison is a plain string compare:
//! `REG_SZ` holds the unescaped string, `REG_DWORD` eight lowercase hex digits, every
//! other type the lowercase comma-separated bytes of its `hex(t):` encoding.

use std::collections::BTreeMap;

use crate::table_compare::csv::decode_text;

/// Failure to read a `.reg` export.
#[derive(Debug, thiserror::Error)]
pub enum RegError {
    #[error("not a registry export (missing 'Windows Registry Editor' or 'REGEDIT4' header)")]
    NotRegistryFile,
}

/// Pseudo value name recording a `[-HKEY...]` key deletion.
pub const DELETE_KEY_NAME: &str = "<delete key>";

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RegValue {
    /// `@` is the key's default value.
    pub name: String,
    /// `REG_SZ`, `REG_DWORD`, ..., `DELETE` (a `name=-` line) or `DELETE_KEY`.
    pub kind: String,
    pub data: String,
}

impl RegValue {
    /// Human-readable data (decodes UTF-16 types, shows DWORD as hex and decimal).
    pub fn display(&self) -> String {
        let bytes = || -> Vec<u8> { self.data.split(',').filter_map(|b| u8::from_str_radix(b.trim(), 16).ok()).collect() };
        let utf16 = |b: &[u8]| {
            let units: Vec<u16> = b.chunks_exact(2).map(|c| u16::from_le_bytes([c[0], c[1]])).collect();
            String::from_utf16_lossy(&units)
        };
        match self.kind.as_str() {
            "REG_DWORD" => u32::from_str_radix(&self.data, 16).map_or(self.data.clone(), |v| format!("0x{:08x} ({v})", v)),
            "REG_EXPAND_SZ" | "REG_LINK" => utf16(&bytes()).trim_end_matches('\0').to_string(),
            "REG_MULTI_SZ" => utf16(&bytes()).trim_end_matches('\0').split('\0').collect::<Vec<_>>().join(" | "),
            _ => self.data.clone(),
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RegKey {
    pub path: String,
    pub values: Vec<RegValue>,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RegFile {
    pub keys: Vec<RegKey>,
}

const TYPES: [(&str, &str); 12] = [
    ("0", "REG_NONE"), ("1", "REG_SZ"), ("2", "REG_EXPAND_SZ"), ("3", "REG_BINARY"), ("4", "REG_DWORD"),
    ("5", "REG_DWORD_BIG_ENDIAN"), ("6", "REG_LINK"), ("7", "REG_MULTI_SZ"), ("8", "REG_RESOURCE_LIST"),
    ("9", "REG_FULL_RESOURCE_DESCRIPTOR"), ("a", "REG_RESOURCE_REQUIREMENTS_LIST"), ("b", "REG_QWORD"),
];

fn type_name(code: &str) -> String {
    let code = code.to_ascii_lowercase();
    TYPES.iter().find(|(c, _)| *c == code).map_or_else(|| format!("hex({code})"), |(_, n)| n.to_string())
}

fn type_code(kind: &str) -> String {
    TYPES.iter().find(|(_, n)| *n == kind).map_or_else(|| kind.trim_start_matches("hex(").trim_end_matches(')').to_string(), |(c, _)| c.to_string())
}

fn unescape(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut chars = s.chars();
    while let Some(c) = chars.next() {
        match (c, c == '\\') {
            (_, true) => out.push(chars.next().unwrap_or('\\')),
            _ => out.push(c),
        }
    }
    out
}

fn escape(s: &str) -> String {
    s.replace('\\', "\\\\").replace('"', "\\\"")
}

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

fn parse_value(name: String, rest: &str) -> RegValue {
    let rest = rest.trim();
    let (kind, data) = if rest == "-" {
        ("DELETE".to_string(), String::new())
    } else if let Some(s) = rest.strip_prefix('"') {
        ("REG_SZ".into(), unescape(s.strip_suffix('"').unwrap_or(s)))
    } else if let Some(d) = rest.strip_prefix("dword:") {
        ("REG_DWORD".into(), d.trim().to_ascii_lowercase())
    } else if let Some(h) = rest.strip_prefix("hex") {
        let (code, bytes) = match h.strip_prefix('(').and_then(|t| t.split_once("):")) {
            Some((code, bytes)) => (code, bytes),
            None => ("3", h.trim_start_matches(':')),
        };
        let canon: Vec<String> = bytes.split(',').map(|b| b.trim().to_ascii_lowercase()).filter(|b| !b.is_empty()).collect();
        (type_name(code), canon.join(","))
    } else {
        ("REG_SZ".into(), rest.to_string())
    };
    RegValue { name, kind, data }
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
    let mut lines = lines.peekable();
    while let Some(line) = lines.next() {
        if line.starts_with(';') {
            continue;
        }
        if let Some(inner) = line.strip_prefix('[').and_then(|l| l.strip_suffix(']')) {
            let (path, deleted) = inner.strip_prefix('-').map_or((inner, false), |p| (p, true));
            let mut key = RegKey { path: path.to_string(), values: Vec::new() };
            if deleted {
                key.values.push(RegValue { name: DELETE_KEY_NAME.into(), kind: "DELETE_KEY".into(), data: String::new() });
            }
            file.keys.push(key);
            continue;
        }
        let mut full = line.to_string();
        while full.ends_with('\\') && full.contains("=hex") {
            full.pop();
            match lines.next() {
                Some(next) => full.push_str(next),
                None => break,
            }
        }
        if let (Some(key), Some((name, rest))) = (file.keys.last_mut(), split_name(&full)) {
            key.values.push(parse_value(name, rest));
        }
    }
    Ok(file)
}

/// Serialises as a UTF-16LE (BOM) `Windows Registry Editor Version 5.00` file.
pub fn write_reg(file: &RegFile) -> Vec<u8> {
    let mut text = String::from("Windows Registry Editor Version 5.00\r\n\r\n");
    for key in &file.keys {
        let deleted = key.values.iter().any(|v| v.kind == "DELETE_KEY");
        text.push_str(&format!("[{}{}]\r\n", if deleted { "-" } else { "" }, key.path));
        for v in key.values.iter().filter(|v| v.kind != "DELETE_KEY") {
            let name = if v.name == "@" { "@".to_string() } else { format!("\"{}\"", escape(&v.name)) };
            let data = match v.kind.as_str() {
                "DELETE" => "-".to_string(),
                "REG_SZ" => format!("\"{}\"", escape(&v.data)),
                "REG_DWORD" => format!("dword:{}", v.data),
                "REG_BINARY" => format!("hex:{}", v.data),
                other => format!("hex({}):{}", type_code(other), v.data),
            };
            text.push_str(&format!("{name}={data}\r\n"));
        }
        text.push_str("\r\n");
    }
    let mut out = vec![0xFF, 0xFE];
    out.extend(text.encode_utf16().flat_map(u16::to_le_bytes));
    out
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RegRowKind {
    Equal,
    Changed,
    LeftOnly,
    RightOnly,
}

/// One aligned row: a key itself (`name == None`) or one of its values.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RegDiffRow {
    pub kind: RegRowKind,
    pub key: String,
    pub name: Option<String>,
    pub left: Option<RegValue>,
    pub right: Option<RegValue>,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RegCompareOptions {
    /// Compare string/hex data case-insensitively.
    pub ignore_data_case: bool,
}

type ValueMap = BTreeMap<String, RegValue>;
type KeyMap = BTreeMap<String, (String, ValueMap)>;

fn index(file: &RegFile) -> KeyMap {
    let mut map = KeyMap::new();
    for key in &file.keys {
        let entry = map.entry(key.path.to_lowercase()).or_insert_with(|| (key.path.clone(), ValueMap::new()));
        for v in &key.values {
            entry.1.insert(if v.name == "@" { String::new() } else { v.name.to_lowercase() }, v.clone());
        }
    }
    map
}

/// Aligns keys and values (case-insensitive, sorted; default value first).
pub fn diff_reg(left: &RegFile, right: &RegFile, o: &RegCompareOptions) -> Vec<RegDiffRow> {
    let (lm, rm) = (index(left), index(right));
    let mut all: Vec<&String> = lm.keys().chain(rm.keys()).collect();
    all.sort();
    all.dedup();
    let same = |a: &RegValue, b: &RegValue| {
        a.kind == b.kind && if o.ignore_data_case { a.data.eq_ignore_ascii_case(&b.data) } else { a.data == b.data }
    };
    let mut out = Vec::new();
    for key in all {
        let (l, r) = (lm.get(key), rm.get(key));
        let path = l.or(r).map(|e| e.0.clone()).unwrap_or_default();
        let kind = match (l, r) {
            (Some(_), Some(_)) => RegRowKind::Equal,
            (Some(_), None) => RegRowKind::LeftOnly,
            _ => RegRowKind::RightOnly,
        };
        out.push(RegDiffRow { kind, key: path.clone(), name: None, left: None, right: None });
        let empty = ValueMap::new();
        let (lv, rv) = (l.map_or(&empty, |e| &e.1), r.map_or(&empty, |e| &e.1));
        let mut names: Vec<&String> = lv.keys().chain(rv.keys()).collect();
        names.sort();
        names.dedup();
        for n in names {
            let (a, b) = (lv.get(n), rv.get(n));
            let kind = match (a, b) {
                (Some(x), Some(y)) if same(x, y) => RegRowKind::Equal,
                (Some(_), Some(_)) => RegRowKind::Changed,
                (Some(_), None) => RegRowKind::LeftOnly,
                _ => RegRowKind::RightOnly,
            };
            let shown = a.or(b).map(|v| v.name.clone());
            out.push(RegDiffRow { kind, key: path.clone(), name: shown, left: a.cloned(), right: b.cloned() });
        }
    }
    out
}

/// Maximal runs of consecutive non-equal rows as `(start, end)` inclusive.
pub fn group_reg_rows(rows: &[RegDiffRow]) -> Vec<(usize, usize)> {
    let mut groups = Vec::new();
    let mut i = 0;
    while i < rows.len() {
        if rows[i].kind == RegRowKind::Equal {
            i += 1;
            continue;
        }
        let start = i;
        while i < rows.len() && rows[i].kind != RegRowKind::Equal {
            i += 1;
        }
        groups.push((start, i - 1));
    }
    groups
}

impl RegFile {
    fn key_index(&self, path: &str) -> Option<usize> {
        self.keys.iter().position(|k| k.path.eq_ignore_ascii_case(path))
    }
    fn ensure_key(&mut self, path: &str) -> usize {
        self.key_index(path).unwrap_or_else(|| {
            self.keys.push(RegKey { path: path.to_string(), values: Vec::new() });
            self.keys.len() - 1
        })
    }
    fn set_value(&mut self, path: &str, value: RegValue) {
        let k = self.ensure_key(path);
        let values = &mut self.keys[k].values;
        match values.iter().position(|v| v.name.eq_ignore_ascii_case(&value.name)) {
            Some(i) => values[i] = value,
            None => values.push(value),
        }
    }
    fn remove_value(&mut self, path: &str, name: &str) {
        if let Some(k) = self.key_index(path) {
            self.keys[k].values.retain(|v| !v.name.eq_ignore_ascii_case(name));
        }
    }
}

/// Makes the destination match the source for diff rows `start..=end`.
pub fn apply_reg_copy(left: &mut RegFile, right: &mut RegFile, rows: &[RegDiffRow], start: usize, end: usize, to_right: bool) {
    let dst = if to_right { right } else { left };
    for row in rows.iter().skip(start).take(end.saturating_sub(start) + 1) {
        if row.kind == RegRowKind::Equal {
            continue;
        }
        let src_has = |side_is_left: bool| if side_is_left { row.kind != RegRowKind::RightOnly } else { row.kind != RegRowKind::LeftOnly };
        let source_present = src_has(to_right);
        match &row.name {
            None if source_present => {
                dst.ensure_key(&row.key);
            }
            None => dst.keys.retain(|k| !k.path.eq_ignore_ascii_case(&row.key)),
            Some(name) => {
                let source = if to_right { &row.left } else { &row.right };
                match source {
                    Some(v) => dst.set_value(&row.key, v.clone()),
                    None => dst.remove_value(&row.key, name),
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const A: &str = "Windows Registry Editor Version 5.00\r\n\r\n[HKEY_CURRENT_USER\\Soft]\r\n@=\"x\"\r\n\"Count\"=dword:0000000A\r\n\"Blob\"=hex:01,02,\\\r\n  03\r\n";

    #[test]
    fn parses_and_diffs() {
        let l = parse_reg(A.as_bytes()).unwrap();
        let r = parse_reg(A.replace("0000000A", "0000000b").as_bytes()).unwrap();
        assert_eq!(l.keys[0].values[2].data, "01,02,03");
        let rows = diff_reg(&l, &r, &RegCompareOptions::default());
        assert_eq!(rows.iter().filter(|r| r.kind == RegRowKind::Changed).count(), 1);
    }

    #[test]
    fn write_then_parse_round_trips() {
        let l = parse_reg(A.as_bytes()).unwrap();
        assert_eq!(parse_reg(&write_reg(&l)).unwrap(), l);
    }

    #[test]
    fn copy_adds_missing_value() {
        let mut l = parse_reg(A.as_bytes()).unwrap();
        let mut r = parse_reg(A.replace("\"Count\"=dword:0000000A\r\n", "").as_bytes()).unwrap();
        let rows = diff_reg(&l, &r, &RegCompareOptions::default());
        let (s, e) = group_reg_rows(&rows)[0];
        apply_reg_copy(&mut l, &mut r, &rows, s, e, true);
        assert!(diff_reg(&l, &r, &RegCompareOptions::default()).iter().all(|r| r.kind == RegRowKind::Equal));
    }
}
