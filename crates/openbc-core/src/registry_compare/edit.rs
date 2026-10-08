//! Copy edits: make one side match the other for a range of diff rows.

use super::diff::{RegDiffRow, RegRowKind};
use super::model::{RegFile, RegKey, RegValue};

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
