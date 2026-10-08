//! Key/value alignment between two [`RegFile`]s.

use std::collections::{BTreeMap, BTreeSet};

use super::model::{RegFile, RegValue};
use crate::diff_util::changed_runs;

/// How a row relates the two sides.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RegRowKind {
    /// Present and equal on both sides.
    Equal,
    /// Present on both sides with different kind or data.
    Changed,
    /// Present only on the left.
    LeftOnly,
    /// Present only on the right.
    RightOnly,
}

/// One aligned row: a key itself (`name == None`) or one of its values.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RegDiffRow {
    /// Relationship between the two sides.
    pub kind: RegRowKind,
    /// Key path (display casing of the first side that has it).
    pub key: String,
    /// Value name; `None` for the row of the key itself.
    pub name: Option<String>,
    /// Left value, when present.
    pub left: Option<RegValue>,
    /// Right value, when present.
    pub right: Option<RegValue>,
}

/// Comparison policy.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RegCompareOptions {
    /// Compare string/hex data case-insensitively.
    pub ignore_data_case: bool,
}

type ValueMap = BTreeMap<String, RegValue>;
/// Lower-cased key path to `(display path, values by lower-cased name)`.
type KeyMap = BTreeMap<String, (String, ValueMap)>;

fn index(file: &RegFile) -> KeyMap {
    let mut map = KeyMap::new();
    for key in &file.keys {
        let entry = map
            .entry(key.path.to_lowercase())
            .or_insert_with(|| (key.path.clone(), ValueMap::new()));
        for v in &key.values {
            let name = if v.name == "@" { String::new() } else { v.name.to_lowercase() };
            entry.1.insert(name, v.clone());
        }
    }
    map
}

/// Row kind for an item present on `has_left` / `has_right`; `both` applies when on both.
fn row_kind(has_left: bool, has_right: bool, both: RegRowKind) -> RegRowKind {
    match (has_left, has_right) {
        (true, true) => both,
        (true, false) => RegRowKind::LeftOnly,
        _ => RegRowKind::RightOnly,
    }
}

/// Aligns keys and values (case-insensitive, sorted; default value first).
pub fn diff_reg(left: &RegFile, right: &RegFile, o: &RegCompareOptions) -> Vec<RegDiffRow> {
    let (lm, rm) = (index(left), index(right));
    let same = |a: &RegValue, b: &RegValue| {
        a.kind == b.kind
            && if o.ignore_data_case { a.data.eq_ignore_ascii_case(&b.data) } else { a.data == b.data }
    };
    let keys: BTreeSet<&String> = lm.keys().chain(rm.keys()).collect();
    let empty = ValueMap::new();
    let mut out = Vec::new();
    for key in keys {
        let (l, r) = (lm.get(key), rm.get(key));
        let path = l.or(r).map(|e| e.0.clone()).unwrap_or_default();
        out.push(RegDiffRow {
            kind: row_kind(l.is_some(), r.is_some(), RegRowKind::Equal),
            key: path.clone(),
            name: None,
            left: None,
            right: None,
        });
        let (lv, rv) = (l.map_or(&empty, |e| &e.1), r.map_or(&empty, |e| &e.1));
        let names: BTreeSet<&String> = lv.keys().chain(rv.keys()).collect();
        for n in names {
            let (a, b) = (lv.get(n), rv.get(n));
            let both = match (a, b) {
                (Some(x), Some(y)) if !same(x, y) => RegRowKind::Changed,
                _ => RegRowKind::Equal,
            };
            out.push(RegDiffRow {
                kind: row_kind(a.is_some(), b.is_some(), both),
                key: path.clone(),
                name: a.or(b).map(|v| v.name.clone()),
                left: a.cloned(),
                right: b.cloned(),
            });
        }
    }
    out
}

/// Maximal runs of consecutive non-equal rows as `(start, end)` inclusive.
pub fn group_reg_rows(rows: &[RegDiffRow]) -> Vec<(usize, usize)> {
    changed_runs(rows, |row| row.kind == RegRowKind::Equal)
}
