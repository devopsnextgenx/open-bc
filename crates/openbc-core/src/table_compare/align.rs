//! Row alignment: by order (Myers LCS over signatures) or by key columns (hash join).

use std::collections::{HashMap, VecDeque};
use std::ops::Range;

use super::cells::{changed_cols, key_of, row_signature};
use super::model::{DiffGroup, RowKind, Table, TableCompareOptions, TableDiffRow};
use super::myers::myers_matches;
use crate::diff_util::changed_runs;

/// Largest edit distance the LCS aligner explores before falling back to positional pairing.
const MAX_EDIT_DISTANCE: usize = 1500;

/// Groups consecutive non-equal rows.
pub fn group_changed(rows: &[TableDiffRow]) -> Vec<DiffGroup> {
    changed_runs(rows, |row| row.kind == RowKind::Equal)
        .into_iter()
        .map(|(start, end)| DiffGroup { start, end })
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
    if o.key_columns.is_empty() {
        align_by_order(l, r, ls, rs, o)
    } else {
        align_by_key(l, r, o)
    }
}

/// Positionally pairs the rows of a changed gap; leftovers become one-sided rows.
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
