//! Windows registry export (`.reg`) parsing, comparison, copy edits and writing.
//!
//! Values are kept in a canonical text form so comparison is a plain string compare:
//! `REG_SZ` holds the unescaped string, `REG_DWORD` eight lowercase hex digits, every
//! other type the lowercase comma-separated bytes of its `hex(t):` encoding.
//!
//! | Submodule | Role                                                   |
//! |-----------|--------------------------------------------------------|
//! | `model`   | [`RegFile`] / [`RegKey`] / [`RegValue`], [`RegError`]  |
//! | `syntax`  | kind names, type codes, string escaping                |
//! | `parse`   | `.reg` text to model                                   |
//! | `write`   | model to UTF-16LE `.reg` bytes                         |
//! | `diff`    | key/value alignment and change grouping                |
//! | `edit`    | copy-left/right edits driven by diff rows              |

mod diff;
mod edit;
mod model;
mod parse;
mod syntax;
mod write;

pub use diff::{diff_reg, group_reg_rows, RegCompareOptions, RegDiffRow, RegRowKind};
pub use edit::{apply_reg_copy, set_reg_value_text};
pub use model::{RegError, RegFile, RegKey, RegValue, DELETE_KEY_NAME};
pub use parse::parse_reg;
pub use write::write_reg;

#[cfg(test)]
mod tests {
    use super::*;

    const A: &str = "Windows Registry Editor Version 5.00\r\n\r\n[HKEY_CURRENT_USER\\Soft]\r\n@=\"x\"\r\n\"Count\"=dword:0000000A\r\n\"Blob\"=hex:01,02,\\\r\n  03\r\n";

    #[test]
    fn parses_and_diffs() {
        let l = parse_reg(A.as_bytes()).unwrap();
        let r = parse_reg(A.replace("0000000A", "0000000b").as_bytes()).unwrap();
        assert_eq!(l.keys[0].values[2].data, "01,02,03");
        let rows = diff_reg(&l, &r, &RegCompareOptions::default());
        assert_eq!(rows.iter().filter(|r| r.kind == RegRowKind::Changed).count(), 1);
    }

    #[test]
    fn write_then_parse_round_trips() {
        let l = parse_reg(A.as_bytes()).unwrap();
        assert_eq!(parse_reg(&write_reg(&l)).unwrap(), l);
    }

    #[test]
    fn copy_adds_missing_value() {
        let mut l = parse_reg(A.as_bytes()).unwrap();
        let mut r = parse_reg(A.replace("\"Count\"=dword:0000000A\r\n", "").as_bytes()).unwrap();
        let rows = diff_reg(&l, &r, &RegCompareOptions::default());
        let (s, e) = group_reg_rows(&rows)[0];
        apply_reg_copy(&mut l, &mut r, &rows, s, e, true);
        assert!(diff_reg(&l, &r, &RegCompareOptions::default()).iter().all(|r| r.kind == RegRowKind::Equal));
    }
}
