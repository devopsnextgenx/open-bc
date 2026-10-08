//! Data model of table comparison: workbooks, options and aligned rows.

use std::sync::Arc;

/// Where a workbook came from; decides whether it can be written back.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SourceFormat {
    /// Delimited text; writable.
    Csv {
        /// Field delimiter byte.
        delimiter: u8,
    },
    /// xlsx / xls / xlsb / ods; read-only (export via Save As CSV).
    Spreadsheet,
}

/// One sheet (a CSV file is a single sheet).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Table {
    /// Sheet name.
    pub name: String,
    /// Rows of cells; rows may have different lengths.
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
    /// Origin of the data.
    pub format: SourceFormat,
    /// Sheets in file order.
    pub sheets: Vec<Arc<Table>>,
}

impl Workbook {
    /// Sheet names in file order.
    pub fn sheet_names(&self) -> Vec<String> {
        self.sheets.iter().map(|s| s.name.clone()).collect()
    }

    /// Index of the sheet called `name`.
    pub fn find(&self, name: &str) -> Option<usize> {
        self.sheets.iter().position(|s| s.name == name)
    }
}

/// Cell/row comparison policy.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct TableCompareOptions {
    /// Compare cells case-insensitively.
    pub ignore_case: bool,
    /// Ignore leading/trailing whitespace in cells.
    pub trim_cells: bool,
    /// Numeric cells within this absolute distance are equal (0 disables).
    pub numeric_tolerance: f64,
    /// Zero-based key columns; empty aligns rows by order instead.
    pub key_columns: Vec<usize>,
}

/// How an aligned row relates the two tables.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RowKind {
    /// Same cells on both sides.
    Equal,
    /// Paired rows with at least one differing cell.
    Changed,
    /// Row exists only in the left table.
    LeftOnly,
    /// Row exists only in the right table.
    RightOnly,
}

/// One aligned row. Row numbers are 1-based; 0 means "no row on that side".
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TableDiffRow {
    /// Relationship between the two sides.
    pub kind: RowKind,
    /// 1-based row number on the left, or 0.
    pub left_row: u32,
    /// 1-based row number on the right, or 0.
    pub right_row: u32,
    /// Sorted zero-based columns whose cells differ (only for `Changed`).
    pub changed_cols: Vec<u32>,
}

/// A maximal run of consecutive non-equal rows (one copy arrow in the UI).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DiffGroup {
    /// Index of the first row of the run.
    pub start: usize,
    /// Index of the last row of the run (inclusive).
    pub end: usize,
}
