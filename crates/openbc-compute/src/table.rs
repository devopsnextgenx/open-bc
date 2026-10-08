//! Table decoding (CSV/TSV and spreadsheets) and Rayon-parallel row diffing.

use std::io::Cursor;
use std::sync::Arc;

use calamine::{open_workbook_auto_from_rs, Data, Reader};
use openbc_core::table_compare::csv::{decode_text, parse_csv, sniff_delimiter};
use openbc_core::table_compare::{diff_with_signatures, row_signature, SourceFormat, Table, TableCompareOptions, TableDiffRow, Workbook};
use rayon::prelude::*;

/// Failure to decode a table file.
#[derive(Debug, thiserror::Error)]
pub enum TableError {
    #[error("unsupported table format '.{0}'")]
    Unsupported(String),
    #[error("spreadsheet decode failed: {0}")]
    Spreadsheet(String),
    #[error("sheet '{sheet}' exceeds the {limit} cell limit")]
    TooLarge { sheet: String, limit: usize },
}

/// Extensions decoded as delimited text.
pub fn is_delimited_extension(ext: &str) -> bool {
    matches!(ext, "csv" | "tsv" | "tab" | "txt")
}

/// Extensions decoded by the spreadsheet reader.
pub fn is_spreadsheet_extension(ext: &str) -> bool {
    matches!(ext, "xlsx" | "xlsm" | "xlsb" | "xls" | "ods")
}

/// Decodes `bytes` according to the lower-case file extension. Spreadsheet cells are
/// their cached *values* (formulas are not compared).
pub fn decode_workbook(bytes: Vec<u8>, ext: &str, max_cells: usize) -> Result<Workbook, TableError> {
    if is_delimited_extension(ext) {
        let text = decode_text(&bytes);
        let delimiter = if ext == "tsv" || ext == "tab" { b'\t' } else { sniff_delimiter(&text) };
        let rows = parse_csv(&text, delimiter);
        let cells: usize = rows.iter().map(Vec::len).sum();
        if cells > max_cells {
            return Err(TableError::TooLarge { sheet: "(csv)".into(), limit: max_cells });
        }
        return Ok(Workbook { format: SourceFormat::Csv { delimiter }, sheets: vec![Arc::new(Table { name: "Sheet1".into(), rows })] });
    }
    if !is_spreadsheet_extension(ext) {
        return Err(TableError::Unsupported(ext.to_string()));
    }
    let mut book = open_workbook_auto_from_rs(Cursor::new(bytes)).map_err(|e| TableError::Spreadsheet(e.to_string()))?;
    let mut sheets = Vec::new();
    let mut total = 0usize;
    for name in book.sheet_names().to_vec() {
        let range = book.worksheet_range(&name).map_err(|e| TableError::Spreadsheet(e.to_string()))?;
        // Pad so row/column positions match what Excel shows.
        let (r0, c0) = range.start().map_or((0, 0), |(r, c)| (r as usize, c as usize));
        let mut rows: Vec<Vec<String>> = vec![Vec::new(); r0];
        for row in range.rows() {
            let mut cells = vec![String::new(); c0];
            cells.extend(row.iter().map(cell_text));
            total += cells.len();
            rows.push(cells);
        }
        if total > max_cells {
            return Err(TableError::TooLarge { sheet: name, limit: max_cells });
        }
        sheets.push(Arc::new(Table { name, rows }));
    }
    Ok(Workbook { format: SourceFormat::Spreadsheet, sheets })
}

fn cell_text(d: &Data) -> String {
    match d {
        Data::Empty => String::new(),
        Data::String(s) | Data::DateTimeIso(s) | Data::DurationIso(s) => s.clone(),
        Data::Int(i) => i.to_string(),
        Data::Float(f) if f.fract() == 0.0 && f.abs() < 1e15 => format!("{}", *f as i64),
        Data::Float(f) => f.to_string(),
        Data::Bool(b) => b.to_string().to_uppercase(),
        Data::DateTime(dt) => dt
            .as_datetime()
            .map(|t| t.format("%Y-%m-%d %H:%M:%S").to_string())
            .unwrap_or_else(|| dt.as_f64().to_string()),
        Data::Error(e) => format!("{e}"),
    }
}

/// Row signatures on Rayon, then the pure aligner from `openbc-core`.
pub fn diff_tables_parallel(left: &Table, right: &Table, o: &TableCompareOptions) -> Vec<TableDiffRow> {
    let sig = |t: &Table| -> Vec<u64> { t.rows.par_iter().map(|r| row_signature(r, o)).collect() };
    let (ls, rs) = rayon::join(|| sig(left), || sig(right));
    diff_with_signatures(left, right, &ls, &rs, o)
}
