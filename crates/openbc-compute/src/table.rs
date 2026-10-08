//! Table decoding (CSV/TSV and spreadsheets) and Rayon-parallel row diffing.

use std::io::Cursor;
use std::sync::Arc;

use calamine::{open_workbook_auto_from_rs, Data, Range, Reader};
use openbc_core::table_compare::csv::{decode_text, parse_csv, sniff_delimiter};
use openbc_core::table_compare::{
    diff_with_signatures, row_signature, SourceFormat, Table, TableCompareOptions, TableDiffRow,
    Workbook,
};
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
        decode_delimited(&bytes, ext, max_cells)
    } else if is_spreadsheet_extension(ext) {
        decode_spreadsheet(bytes, max_cells)
    } else {
        Err(TableError::Unsupported(ext.to_string()))
    }
}

/// Shared cell-budget check for both decoders.
fn ensure_within(cells: usize, limit: usize, sheet: &str) -> Result<(), TableError> {
    if cells > limit {
        Err(TableError::TooLarge { sheet: sheet.to_string(), limit })
    } else {
        Ok(())
    }
}

// ------------------------------------------------------------------------- delimited text

fn delimiter_for(ext: &str, text: &str) -> u8 {
    if ext == "tsv" || ext == "tab" {
        b'\t'
    } else {
        sniff_delimiter(text)
    }
}

fn decode_delimited(bytes: &[u8], ext: &str, max_cells: usize) -> Result<Workbook, TableError> {
    let text = decode_text(bytes);
    let delimiter = delimiter_for(ext, &text);
    let rows = parse_csv(&text, delimiter);
    ensure_within(rows.iter().map(Vec::len).sum(), max_cells, "(csv)")?;
    Ok(Workbook {
        format: SourceFormat::Csv { delimiter },
        sheets: vec![Arc::new(Table { name: "Sheet1".into(), rows })],
    })
}

// -------------------------------------------------------------------------- spreadsheets

fn decode_spreadsheet(bytes: Vec<u8>, max_cells: usize) -> Result<Workbook, TableError> {
    let spreadsheet_err = |e: &dyn std::fmt::Display| TableError::Spreadsheet(e.to_string());
    let mut book = open_workbook_auto_from_rs(Cursor::new(bytes)).map_err(|e| spreadsheet_err(&e))?;
    let mut sheets = Vec::new();
    let mut total = 0usize;
    for name in book.sheet_names().to_vec() {
        let range = book.worksheet_range(&name).map_err(|e| spreadsheet_err(&e))?;
        let rows = sheet_rows(&range);
        total += rows.iter().map(Vec::len).sum::<usize>();
        ensure_within(total, max_cells, &name)?;
        sheets.push(Arc::new(Table { name, rows }));
    }
    Ok(Workbook { format: SourceFormat::Spreadsheet, sheets })
}

/// Rows of one sheet, padded so row/column positions match what Excel shows.
fn sheet_rows(range: &Range<Data>) -> Vec<Vec<String>> {
    let (r0, c0) = range.start().map_or((0, 0), |(r, c)| (r as usize, c as usize));
    let mut rows: Vec<Vec<String>> = vec![Vec::new(); r0];
    for row in range.rows() {
        let mut cells = vec![String::new(); c0];
        cells.extend(row.iter().map(cell_text));
        rows.push(cells);
    }
    rows
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

// ----------------------------------------------------------------------------------- diff

/// Row signatures on Rayon, then the pure aligner from `openbc-core`.
pub fn diff_tables_parallel(left: &Table, right: &Table, o: &TableCompareOptions) -> Vec<TableDiffRow> {
    let signatures = |t: &Table| -> Vec<u64> { t.rows.par_iter().map(|r| row_signature(r, o)).collect() };
    let (ls, rs) = rayon::join(|| signatures(left), || signatures(right));
    diff_with_signatures(left, right, &ls, &rs, o)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn extension_classification_is_disjoint() {
        for ext in ["csv", "tsv", "tab", "txt"] {
            assert!(is_delimited_extension(ext) && !is_spreadsheet_extension(ext));
        }
        for ext in ["xlsx", "xlsm", "xlsb", "xls", "ods"] {
            assert!(is_spreadsheet_extension(ext) && !is_delimited_extension(ext));
        }
    }

    #[test]
    fn unknown_extension_is_unsupported() {
        assert!(matches!(
            decode_workbook(Vec::new(), "png", 10),
            Err(TableError::Unsupported(e)) if e == "png"
        ));
    }

    #[test]
    fn csv_within_budget_decodes_to_one_sheet() {
        let book = decode_workbook(b"a,b\n1,2\n".to_vec(), "csv", 100).unwrap();
        assert_eq!(book.sheets.len(), 1);
        assert_eq!(book.sheets[0].rows[0].len(), 2);
    }

    #[test]
    fn csv_over_budget_reports_the_limit() {
        let err = decode_workbook(b"a,b\n1,2\n".to_vec(), "csv", 3).err().expect("over budget");
        assert!(matches!(err, TableError::TooLarge { limit: 3, .. }));
    }

    #[test]
    fn garbage_spreadsheet_is_a_decode_error() {
        let err = decode_workbook(vec![1, 2, 3], "xlsx", 100).err().expect("not a workbook");
        assert!(matches!(err, TableError::Spreadsheet(_)));
    }
}
