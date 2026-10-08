//! [`RegFile`] to `.reg` bytes.

use super::model::{RegFile, RegValue};
use super::syntax::{
    escape, type_code, KIND_BINARY, KIND_DELETE, KIND_DELETE_KEY, KIND_DWORD, KIND_SZ,
};
use crate::encoding::utf16le_with_bom;

/// The `name=data` line (without line ending) for one value.
fn value_line(v: &RegValue) -> String {
    let name = if v.name == "@" { "@".to_string() } else { format!("\"{}\"", escape(&v.name)) };
    let data = match v.kind.as_str() {
        KIND_DELETE => "-".to_string(),
        KIND_SZ => format!("\"{}\"", escape(&v.data)),
        KIND_DWORD => format!("dword:{}", v.data),
        KIND_BINARY => format!("hex:{}", v.data),
        other => format!("hex({}):{}", type_code(other), v.data),
    };
    format!("{name}={data}")
}

/// Serialises as a UTF-16LE (BOM) `Windows Registry Editor Version 5.00` file.
pub fn write_reg(file: &RegFile) -> Vec<u8> {
    let mut text = String::from("Windows Registry Editor Version 5.00\r\n\r\n");
    for key in &file.keys {
        let deleted = key.values.iter().any(|v| v.kind == KIND_DELETE_KEY);
        text.push_str(&format!("[{}{}]\r\n", if deleted { "-" } else { "" }, key.path));
        for v in key.values.iter().filter(|v| v.kind != KIND_DELETE_KEY) {
            text.push_str(&value_line(v));
            text.push_str("\r\n");
        }
        text.push_str("\r\n");
    }
    utf16le_with_bom(&text)
}
