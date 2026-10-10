//! Copy edits: make one side match the other for a range of diff rows.

use super::diff::{RegDiffRow, RegRowKind};
use super::model::{RegFile, RegKey, RegValue};
use super::syntax::{KIND_DELETE, KIND_DELETE_KEY, KIND_DWORD, KIND_EXPAND_SZ, KIND_LINK, KIND_MULTI_SZ, KIND_SZ};

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
pub fn apply_reg_copy(
    left: &mut RegFile,
    right: &mut RegFile,
    rows: &[RegDiffRow],
    start: usize,
    end: usize,
    to_right: bool,
) {
    let dst = if to_right { right } else { left };
    for row in rows.iter().skip(start).take(end.saturating_sub(start) + 1) {
        if row.kind == RegRowKind::Equal {
            continue;
        }
        // The source side is left when copying to the right, and vice versa.
        let source_present = if to_right {
            row.kind != RegRowKind::RightOnly
        } else {
            row.kind != RegRowKind::LeftOnly
        };
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

/// UTF-16LE code units as canonical comma-separated lowercase hex bytes.
fn utf16_hex(units: impl Iterator<Item = u16>) -> String {
    units.flat_map(u16::to_le_bytes).map(|b| format!("{b:02x}")).collect::<Vec<_>>().join(",")
}

/// `0x1F`, `0x0000001f (31)` or a decimal number to eight lowercase hex digits.
fn parse_dword_text(text: &str) -> Result<String, String> {
    let t = text.trim();
    let t = t.split('(').next().unwrap_or(t).trim();
    let parsed = match t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        Some(hex) => u32::from_str_radix(hex, 16),
        None => t.parse::<u32>(),
    };
    parsed.map(|v| format!("{v:08x}")).map_err(|_| format!("'{t}' is not a valid DWORD (use 0x1F or a decimal number)"))
}

/// `01, 0A,ff` to `01,0a,ff`.
fn parse_hex_bytes_text(text: &str) -> Result<String, String> {
    let mut bytes = Vec::new();
    for part in text.split(',').map(str::trim).filter(|p| !p.is_empty()) {
        if part.len() > 2 || !part.chars().all(|c| c.is_ascii_hexdigit()) {
            return Err(format!("'{part}' is not a hex byte (expected comma-separated bytes such as 01,ff)"));
        }
        bytes.push(format!("{:0>2}", part.to_ascii_lowercase()));
    }
    Ok(bytes.join(","))
}

/// Replaces the data of an existing value from text typed in the form [`RegValue::display`]
/// shows it (a DWORD as `0x..` or decimal, `REG_MULTI_SZ` entries separated by ` | `, binary
/// as comma-separated hex bytes). The value keeps its kind. Returns whether the data changed.
pub fn set_reg_value_text(file: &mut RegFile, key: &str, name: &str, text: &str) -> Result<bool, String> {
    let k = file.key_index(key).ok_or_else(|| format!("key {key} not found"))?;
    let value = file.keys[k]
        .values
        .iter_mut()
        .find(|v| v.name.eq_ignore_ascii_case(name))
        .ok_or_else(|| format!("value {name} not found"))?;
    let data = match value.kind.as_str() {
        KIND_SZ => text.to_string(),
        KIND_DWORD => parse_dword_text(text)?,
        KIND_EXPAND_SZ | KIND_LINK => utf16_hex(text.encode_utf16().chain(std::iter::once(0))),
        KIND_MULTI_SZ => {
            let mut units: Vec<u16> = Vec::new();
            for entry in text.split(" | ").filter(|s| !s.is_empty()) {
                units.extend(entry.encode_utf16());
                units.push(0);
            }
            units.push(0);
            utf16_hex(units.into_iter())
        }
        KIND_DELETE | KIND_DELETE_KEY => return Err("a deletion marker has no data to edit".into()),
        _ => parse_hex_bytes_text(text)?,
    };
    if value.data == data {
        return Ok(false);
    }
    value.data = data;
    Ok(true)
}

#[cfg(test)]
mod edit_text_tests {
    use super::super::{parse_reg, set_reg_value_text};

    #[test]
    fn typed_edits_keep_the_kind() {
        let src = "Windows Registry Editor Version 5.00\r\n\r\n[HKEY_CURRENT_USER\\S]\r\n@=\"x\"\r\n\"N\"=dword:0000000a\r\n\"B\"=hex:01,02\r\n";
        let mut f = parse_reg(src.as_bytes()).unwrap();
        assert_eq!(set_reg_value_text(&mut f, "hkey_current_user\\s", "@", "y"), Ok(true));
        assert_eq!(set_reg_value_text(&mut f, "HKEY_CURRENT_USER\\S", "N", "0x1F"), Ok(true));
        assert_eq!(f.keys[0].values[1].data, "0000001f");
        assert_eq!(set_reg_value_text(&mut f, "HKEY_CURRENT_USER\\S", "N", "31"), Ok(false));
        assert!(set_reg_value_text(&mut f, "HKEY_CURRENT_USER\\S", "N", "zz").is_err());
        assert_eq!(set_reg_value_text(&mut f, "HKEY_CURRENT_USER\\S", "B", "AA, b"), Ok(true));
        assert_eq!(f.keys[0].values[2].data, "aa,0b");
    }
}
