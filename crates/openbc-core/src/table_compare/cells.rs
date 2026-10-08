//! Cell-level comparison: normalisation, equality, signatures and keys.

use std::borrow::Cow;
use std::collections::hash_map::DefaultHasher;
use std::hash::{Hash, Hasher};

use super::model::TableCompareOptions;

/// Applies the trim / case policy of `o` to one cell.
fn norm<'a>(s: &'a str, o: &TableCompareOptions) -> Cow<'a, str> {
    let t = if o.trim_cells { s.trim() } else { s };
    if o.ignore_case {
        Cow::Owned(t.to_lowercase())
    } else {
        Cow::Borrowed(t)
    }
}

/// Cell at `i`, or `""` when the row is shorter.
fn cell(row: &[String], i: usize) -> &str {
    row.get(i).map_or("", String::as_str)
}

/// Cell equality under `o` (case, trimming, numeric tolerance).
pub fn cells_equal(a: &str, b: &str, o: &TableCompareOptions) -> bool {
    let (x, y) = (norm(a, o), norm(b, o));
    if x == y {
        return true;
    }
    if o.numeric_tolerance > 0.0 {
        if let (Ok(p), Ok(q)) = (x.trim().parse::<f64>(), y.trim().parse::<f64>()) {
            return (p - q).abs() <= o.numeric_tolerance;
        }
    }
    false
}

/// Order-alignment signature of a row; trailing empty cells do not count.
pub fn row_signature(row: &[String], o: &TableCompareOptions) -> u64 {
    let end = row.iter().rposition(|c| !norm(c, o).is_empty()).map_or(0, |i| i + 1);
    let mut h = DefaultHasher::new();
    end.hash(&mut h);
    for c in &row[..end] {
        norm(c, o).hash(&mut h);
    }
    h.finish()
}

/// Join key of a row over `o.key_columns` (unit-separator delimited).
pub(super) fn key_of(row: &[String], o: &TableCompareOptions) -> String {
    let mut key = String::new();
    for &c in &o.key_columns {
        key.push_str(&norm(cell(row, c), o));
        key.push('\u{1f}');
    }
    key
}

/// Zero-based columns whose cells differ between two rows.
pub(super) fn changed_cols(l: &[String], r: &[String], o: &TableCompareOptions) -> Vec<u32> {
    (0..l.len().max(r.len()))
        .filter(|&i| !cells_equal(cell(l, i), cell(r, i), o))
        .map(|i| i as u32)
        .collect()
}
