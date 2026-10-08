//! Table (CSV / spreadsheet) comparison: pure row alignment, cell diff and copy edits.
//!
//! Rows are aligned either by order (Myers LCS over row signatures, with positional
//! pairing inside changed gaps) or by key columns (hash join). Cells of paired rows are
//! compared individually so the UI can highlight exactly the changed cells.

pub mod csv;

use std::borrow::Cow;
use std::collections::{hash_map::DefaultHasher, HashMap, VecDeque};
use std::hash::{Hash, Hasher};
use std::ops::Range;
use std::sync::Arc;

/// Largest edit distance the LCS aligner explores before falling back to positional pairing.
const MAX_EDIT_DISTANCE: usize = 1500;

/// Where a workbook came from; decides whether it can be written back.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SourceFormat {
    /// Delimited text; writable.
    Csv { delimiter: u8 },
    /// xlsx / xls / xlsb / ods; read-only (export via Save As CSV).
    Spreadsheet,
}

/// One sheet (a CSV file is a single sheet).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Table {
    pub name: String,
    pub rows: Vec<Vec<String>>,
}

impl Table {
    /// Widest row.
    pub fn columns(&self) -> usize {
        self.rows.iter().map(Vec::len).max().unwrap_or(0)
    }
}

/// A set of sheets; sheets are `Arc`ed so result snapshots share them cheaply.
#[derive(Clone, Debug)]
pub struct Workbook {
    pub format: SourceFormat,
    pub sheets: Vec<Arc<Table>>,
}

impl Workbook {
    pub fn sheet_names(&self) -> Vec<String> {
        self.sheets.iter().map(|s| s.name.clone()).collect()
    }
    pub fn find(&self, name: &str) -> Option<usize> {
        self.sheets.iter().position(|s| s.name == name)
    }
}

/// Cell/row comparison policy.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct TableCompareOptions {
    pub ignore_case: bool,
    pub trim_cells: bool,
    /// Numeric cells within this absolute distance are equal (0 disables).
    pub numeric_tolerance: f64,
    /// Zero-based key columns; empty aligns rows by order instead.
    pub key_columns: Vec<usize>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RowKind {
    Equal,
    Changed,
    LeftOnly,
    RightOnly,
}

/// One aligned row. Row numbers are 1-based; 0 means "no row on that side".
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TableDiffRow {
    pub kind: RowKind,
    pub left_row: u32,
    pub right_row: u32,
    /// Sorted zero-based columns whose cells differ (only for `Changed`).
    pub changed_cols: Vec<u32>,
}

/// A maximal run of consecutive non-equal rows (one copy arrow in the UI).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DiffGroup {
    pub start: usize,
    pub end: usize,
}

/// Groups consecutive non-equal rows.
pub fn group_changed(rows: &[TableDiffRow]) -> Vec<DiffGroup> {
    let mut groups = Vec::new();
    let mut i = 0;
    while i < rows.len() {
        if rows[i].kind == RowKind::Equal {
            i += 1;
            continue;
        }
        let start = i;
        while i < rows.len() && rows[i].kind != RowKind::Equal {
            i += 1;
        }
        groups.push(DiffGroup { start, end: i - 1 });
    }
    groups
}

fn norm<'a>(s: &'a str, o: &TableCompareOptions) -> Cow<'a, str> {
    let t = if o.trim_cells { s.trim() } else { s };
    if o.ignore_case { Cow::Owned(t.to_lowercase()) } else { Cow::Borrowed(t) }
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

fn key_of(row: &[String], o: &TableCompareOptions) -> String {
    let mut key = String::new();
    for &c in &o.key_columns {
        key.push_str(&norm(row.get(c).map_or("", String::as_str), o));
        key.push('\u{1f}');
    }
    key
}

fn changed_cols(l: &[String], r: &[String], o: &TableCompareOptions) -> Vec<u32> {
    (0..l.len().max(r.len()))
        .filter(|&i| !cells_equal(l.get(i).map_or("", String::as_str), r.get(i).map_or("", String::as_str), o))
        .map(|i| i as u32)
        .collect()
}

fn equal_row(i: usize, j: usize) -> TableDiffRow {
    TableDiffRow { kind: RowKind::Equal, left_row: i as u32 + 1, right_row: j as u32 + 1, changed_cols: Vec::new() }
}

fn one_sided(kind: RowKind, i: usize) -> TableDiffRow {
    let n = i as u32 + 1;
    let (left_row, right_row) = if kind == RowKind::LeftOnly { (n, 0) } else { (0, n) };
    TableDiffRow { kind, left_row, right_row, changed_cols: Vec::new() }
}

fn paired_row(l: &Table, r: &Table, i: usize, j: usize, o: &TableCompareOptions) -> TableDiffRow {
    let cols = changed_cols(&l.rows[i], &r.rows[j], o);
    let kind = if cols.is_empty() { RowKind::Equal } else { RowKind::Changed };
    TableDiffRow { kind, left_row: i as u32 + 1, right_row: j as u32 + 1, changed_cols: cols }
}

/// Serial convenience wrapper; `openbc-compute` computes the signatures on Rayon instead.
pub fn diff_tables(left: &Table, right: &Table, o: &TableCompareOptions) -> Vec<TableDiffRow> {
    let ls: Vec<u64> = left.rows.iter().map(|r| row_signature(r, o)).collect();
    let rs: Vec<u64> = right.rows.iter().map(|r| row_signature(r, o)).collect();
    diff_with_signatures(left, right, &ls, &rs, o)
}

/// Aligns the tables given precomputed order signatures (ignored in key mode).
pub fn diff_with_signatures(l: &Table, r: &Table, ls: &[u64], rs: &[u64], o: &TableCompareOptions) -> Vec<TableDiffRow> {
    if o.key_columns.is_empty() { align_by_order(l, r, ls, rs, o) } else { align_by_key(l, r, o) }
}

fn pair_gap(out: &mut Vec<TableDiffRow>, l: &Table, r: &Table, lr: Range<usize>, rr: Range<usize>, o: &TableCompareOptions) {
    let k = lr.len().min(rr.len());
    for t in 0..k {
        out.push(paired_row(l, r, lr.start + t, rr.start + t, o));
    }
    out.extend((lr.start + k..lr.end).map(|i| one_sided(RowKind::LeftOnly, i)));
    out.extend((rr.start + k..rr.end).map(|j| one_sided(RowKind::RightOnly, j)));
}

fn align_by_order(l: &Table, r: &Table, ls: &[u64], rs: &[u64], o: &TableCompareOptions) -> Vec<TableDiffRow> {
    let (n, m) = (ls.len(), rs.len());
    let pre = ls.iter().zip(rs).take_while(|(a, b)| a == b).count();
    let suf = ls[pre..].iter().rev().zip(rs[pre..].iter().rev()).take_while(|(a, b)| a == b).count();
    let mut out = Vec::with_capacity(n.max(m));
    out.extend((0..pre).map(|i| equal_row(i, i)));
    let (mut li, mut ri) = (pre, pre);
    if let Some(pairs) = myers_matches(&ls[pre..n - suf], &rs[pre..m - suf], MAX_EDIT_DISTANCE) {
        for (a, b) in pairs {
            let (a, b) = (a + pre, b + pre);
            pair_gap(&mut out, l, r, li..a, ri..b, o);
            out.push(equal_row(a, b));
            (li, ri) = (a + 1, b + 1);
        }
    }
    pair_gap(&mut out, l, r, li..n - suf, ri..m - suf, o);
    out.extend((0..suf).map(|t| equal_row(n - suf + t, m - suf + t)));
    out
}

fn align_by_key(l: &Table, r: &Table, o: &TableCompareOptions) -> Vec<TableDiffRow> {
    let mut pending: HashMap<String, VecDeque<usize>> = HashMap::new();
    for (j, row) in r.rows.iter().enumerate() {
        pending.entry(key_of(row, o)).or_default().push_back(j);
    }
    let mut right_of: Vec<Option<usize>> = Vec::with_capacity(l.rows.len());
    let mut right_matched = vec![false; r.rows.len()];
    for row in &l.rows {
        let hit = pending.get_mut(&key_of(row, o)).and_then(VecDeque::pop_front);
        if let Some(j) = hit {
            right_matched[j] = true;
        }
        right_of.push(hit);
    }
    let mut out = Vec::with_capacity(l.rows.len().max(r.rows.len()));
    let mut next_r = 0;
    for (i, hit) in right_of.iter().copied().enumerate() {
        match hit {
            Some(j) => {
                while next_r < j {
                    if !right_matched[next_r] {
                        out.push(one_sided(RowKind::RightOnly, next_r));
                    }
                    next_r += 1;
                }
                next_r = next_r.max(j + 1);
                out.push(paired_row(l, r, i, j, o));
            }
            None => out.push(one_sided(RowKind::LeftOnly, i)),
        }
    }
    out.extend((next_r..r.rows.len()).filter(|&j| !right_matched[j]).map(|j| one_sided(RowKind::RightOnly, j)));
    out
}

/// Myers O(ND) LCS; returns ascending `(a_index, b_index)` matches, or `None` past `max_d`.
fn myers_matches(a: &[u64], b: &[u64], max_d: usize) -> Option<Vec<(usize, usize)>> {
    let (n, m) = (a.len() as i32, b.len() as i32);
    if n == 0 || m == 0 {
        return Some(Vec::new());
    }
    let max_d = max_d.min((n + m) as usize) as i32;
    let off = max_d + 1;
    let at = |k: i32| (k + off) as usize;
    let mut v = vec![0i32; (2 * max_d + 3) as usize];
    let mut trace: Vec<Vec<i32>> = Vec::new();
    'search: for d in 0..=max_d {
        trace.push(v.clone());
        for k in (-d..=d).step_by(2) {
            let mut x = if k == -d || (k != d && v[at(k - 1)] < v[at(k + 1)]) { v[at(k + 1)] } else { v[at(k - 1)] + 1 };
            let mut y = x - k;
            while x < n && y < m && a[x as usize] == b[y as usize] {
                x += 1;
                y += 1;
            }
            v[at(k)] = x;
            if x >= n && y >= m {
                break 'search;
            }
        }
        if d == max_d {
            return None;
        }
    }
    let (mut x, mut y) = (n, m);
    let mut matches = Vec::new();
    for d in (0..trace.len() as i32).rev() {
        let vd = &trace[d as usize];
        let k = x - y;
        let prev_k = if k == -d || (k != d && vd[at(k - 1)] < vd[at(k + 1)]) { k + 1 } else { k - 1 };
        let (prev_x, prev_y) = (vd[at(prev_k)], vd[at(prev_k)] - prev_k);
        while x > prev_x && y > prev_y {
            x -= 1;
            y -= 1;
            matches.push((x as usize, y as usize));
        }
        if d > 0 {
            (x, y) = (prev_x, prev_y);
        }
    }
    matches.reverse();
    Some(matches)
}

enum Op {
    Replace(usize, Vec<String>),
    Insert(usize, Vec<String>),
    Delete(usize),
}

/// Copies diff rows `start..=end` from one table onto the other (making the destination
/// match the source for those rows: replace, insert or delete). `rows` must be the diff
/// computed for the current contents of both tables.
pub fn apply_copy(left: &mut Table, right: &mut Table, rows: &[TableDiffRow], start: usize, end: usize, to_right: bool) {
    if rows.is_empty() || start >= rows.len() {
        return;
    }
    let end = end.min(rows.len() - 1);
    let number = |r: &TableDiffRow, dst: bool| if to_right == dst { r.right_row } else { r.left_row };
    let mut ops = Vec::new();
    for idx in start..=end {
        let row = &rows[idx];
        if row.kind == RowKind::Equal {
            continue;
        }
        let (src_n, dst_n) = (number(row, false), number(row, true));
        let src_row = |n: u32| {
            let src = if to_right { &*left } else { &*right };
            src.rows[n as usize - 1].clone()
        };
        match (src_n, dst_n) {
            (0, d) => ops.push(Op::Delete(d as usize - 1)),
            (s, 0) => {
                let after = rows[..idx].iter().rev().map(|p| number(p, true)).find(|&n| n != 0).unwrap_or(0);
                ops.push(Op::Insert(after as usize, src_row(s)));
            }
            (s, d) => ops.push(Op::Replace(d as usize - 1, src_row(s))),
        }
    }
    let dst = if to_right { right } else { left };
    for op in ops.into_iter().rev() {
        match op {
            Op::Replace(i, row) => dst.rows[i] = row,
            Op::Insert(i, row) => dst.rows.insert(i.min(dst.rows.len()), row),
            Op::Delete(i) => {
                dst.rows.remove(i);
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn table(rows: &[&[&str]]) -> Table {
        Table { name: "t".into(), rows: rows.iter().map(|r| r.iter().map(|c| c.to_string()).collect()).collect() }
    }

    #[test]
    fn aligns_insertions_and_marks_changed_cells() {
        let l = table(&[&["a", "1"], &["b", "2"], &["d", "4"]]);
        let r = table(&[&["a", "1"], &["b", "9"], &["c", "3"], &["d", "4"]]);
        let rows = diff_tables(&l, &r, &TableCompareOptions::default());
        let kinds: Vec<_> = rows.iter().map(|r| r.kind).collect();
        assert_eq!(kinds, [RowKind::Equal, RowKind::Changed, RowKind::RightOnly, RowKind::Equal]);
        assert_eq!(rows[1].changed_cols, vec![1]);
    }

    #[test]
    fn key_alignment_ignores_order() {
        let l = table(&[&["x", "1"], &["y", "2"]]);
        let r = table(&[&["y", "2"], &["x", "1"]]);
        let o = TableCompareOptions { key_columns: vec![0], ..Default::default() };
        assert!(diff_tables(&l, &r, &o).iter().all(|row| row.kind == RowKind::Equal));
    }

    #[test]
    fn copy_makes_destination_match() {
        let (mut l, mut r) = (table(&[&["a"], &["b"], &["d"]]), table(&[&["a"], &["c"], &["d"]]));
        let rows = diff_tables(&l, &r, &TableCompareOptions::default());
        let groups = group_changed(&rows);
        apply_copy(&mut l, &mut r, &rows, groups[0].start, groups[0].end, true);
        assert_eq!(l.rows, r.rows);
    }

    #[test]
    fn tolerance_treats_close_numbers_as_equal() {
        let o = TableCompareOptions { numeric_tolerance: 0.01, ..Default::default() };
        assert!(cells_equal("1.000", "1.004", &o));
        assert!(!cells_equal("1.000", "1.02", &o));
    }
}
