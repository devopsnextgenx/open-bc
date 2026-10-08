//! Copy edits: make one table match the other for a range of diff rows.

use super::model::{RowKind, Table, TableDiffRow};

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
