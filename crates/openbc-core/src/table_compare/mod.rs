//! Table (CSV / spreadsheet) comparison: pure row alignment, cell diff and copy edits.
//!
//! Rows are aligned either by order (Myers LCS over row signatures, with positional
//! pairing inside changed gaps) or by key columns (hash join). Cells of paired rows are
//! compared individually so the UI can highlight exactly the changed cells.
//!
//! | Submodule | Role                                                        |
//! |-----------|-------------------------------------------------------------|
//! | `model`   | tables, workbooks, options and aligned-row types            |
//! | `cells`   | cell equality, row signatures, join keys                    |
//! | `align`   | order / key alignment and change grouping                   |
//! | `myers`   | the LCS kernel used by order alignment                      |
//! | `edit`    | copy-left/right edits driven by diff rows                   |
//! | [`csv`]   | CSV/TSV text decoding, parsing and writing                  |

mod align;
mod cells;
pub mod csv;
mod edit;
mod model;
mod myers;

pub use align::{diff_tables, diff_with_signatures, group_changed};
pub use cells::{cells_equal, row_signature};
pub use edit::apply_copy;
pub use model::{
    DiffGroup, RowKind, SourceFormat, Table, TableCompareOptions, TableDiffRow, Workbook,
};

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
