//! Data model of a parsed `.reg` export.

use super::syntax::{KIND_DWORD, KIND_EXPAND_SZ, KIND_LINK, KIND_MULTI_SZ};
use crate::encoding::utf16_from_bytes;

/// Failure to read a `.reg` export.
#[derive(Debug, thiserror::Error)]
pub enum RegError {
    /// The first non-empty line is not a recognised `.reg` header.
    #[error("not a registry export (missing 'Windows Registry Editor' or 'REGEDIT4' header)")]
    NotRegistryFile,
}

/// Pseudo value name recording a `[-HKEY...]` key deletion.
pub const DELETE_KEY_NAME: &str = "<delete key>";

/// One registry value in canonical text form.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RegValue {
    /// `@` is the key's default value.
    pub name: String,
    /// `REG_SZ`, `REG_DWORD`, ..., `DELETE` (a `name=-` line) or `DELETE_KEY`.
    pub kind: String,
    /// Canonical data (see the module docs).
    pub data: String,
}

impl RegValue {
    /// Human-readable data (decodes UTF-16 types, shows DWORD as hex and decimal).
    pub fn display(&self) -> String {
        let bytes = || -> Vec<u8> {
            self.data.split(',').filter_map(|b| u8::from_str_radix(b.trim(), 16).ok()).collect()
        };
        let text = || utf16_from_bytes(&bytes(), false);
        match self.kind.as_str() {
            KIND_DWORD => u32::from_str_radix(&self.data, 16)
                .map_or(self.data.clone(), |v| format!("0x{v:08x} ({v})")),
            KIND_EXPAND_SZ | KIND_LINK => text().trim_end_matches('\0').to_string(),
            KIND_MULTI_SZ => text().trim_end_matches('\0').split('\0').collect::<Vec<_>>().join(" | "),
            _ => self.data.clone(),
        }
    }
}

/// A registry key and its values, in file order.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RegKey {
    /// Full key path, e.g. `HKEY_CURRENT_USER\Software\Vendor`.
    pub path: String,
    /// Values in file order.
    pub values: Vec<RegValue>,
}

/// A parsed `.reg` file.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct RegFile {
    /// Keys in file order.
    pub keys: Vec<RegKey>,
}
