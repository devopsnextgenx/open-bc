//! Format-neutral description of one parsed binary.

use serde::{Deserialize, Serialize};

/// A named metadata value (`architecture`, `entry point`, `compiler`, ...).
///
/// Metadata is a flat key/value list so every executable format can contribute its own keys and
/// the diff stays generic.
#[derive(Clone, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
pub struct MetaEntry {
    /// Property name, unique within one model.
    pub key: String,
    /// Display value.
    pub value: String,
}

impl MetaEntry {
    /// Convenience constructor.
    #[must_use]
    pub fn new(key: impl Into<String>, value: impl Into<String>) -> Self {
        Self { key: key.into(), value: value.into() }
    }
}

/// Whether a [`SectionInfo`] is a linker section or a loader segment.
#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, PartialEq, Serialize)]
pub enum SectionKind {
    /// `.text`, `.data`, `__TEXT,__text`, ...
    Section,
    /// ELF program header / Mach-O load segment.
    Segment,
}

/// A section or segment with its content hash.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct SectionInfo {
    /// Section or segment name (`.text`, `LOAD`, `__TEXT`).
    pub name: String,
    /// Section vs. segment.
    pub kind: SectionKind,
    /// Virtual address (informational; never used to decide `Modified`).
    pub address: u64,
    /// Size in bytes of the content that was hashed.
    pub size: u64,
    /// Permission / type flags as text (`R-X`, `ALLOC|EXEC`).
    pub flags: String,
    /// Lower-case hex SHA-256 of the raw content.
    pub sha256: String,
}

/// How a symbol relates to the binary.
#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, PartialEq, PartialOrd, Ord, Serialize)]
pub enum SymbolKind {
    /// Public API defined here.
    Exported,
    /// External dependency resolved at load time.
    Imported,
    /// Local / debug symbol (symtab, DWARF or PDB derived).
    Local,
}

/// An exported, imported or local symbol.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct SymbolInfo {
    /// Linkage name (mangled when the format stores it that way).
    pub name: String,
    /// Exported / imported / local.
    pub kind: SymbolKind,
    /// Address, `0` for imports.
    pub address: u64,
    /// Library an import comes from, when the format records it (PE, Mach-O).
    pub library: Option<String>,
}

/// One function with hashes over its raw bytes.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct FunctionInfo {
    /// Symbol name of the function.
    pub name: String,
    /// Start address (informational).
    pub address: u64,
    /// Byte length used for hashing. When the format does not record sizes this is the distance
    /// to the next function in the same section.
    pub size: u64,
    /// `true` when `size` was inferred rather than read from the symbol.
    pub size_estimated: bool,
    /// Lower-case hex SHA-256 over the function bytes.
    pub sha256: String,
    /// 64-bit SimHash of the bytes, for similarity between *modified* versions.
    pub simhash: u64,
}

/// Everything the viewer needs to know about one binary.
#[derive(Clone, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
pub struct BinaryModel {
    /// Display label (file name).
    pub label: String,
    /// File size in bytes.
    pub file_size: u64,
    /// Lower-case hex SHA-256 of the whole file.
    pub file_sha256: String,
    /// `ELF`, `PE`, `Mach-O`, `COFF`.
    pub format: String,
    /// Header / metadata properties (architecture, entry point, timestamp, compiler, ...).
    pub metadata: Vec<MetaEntry>,
    /// Sections and segments.
    pub sections: Vec<SectionInfo>,
    /// Exported, imported and local symbols.
    pub symbols: Vec<SymbolInfo>,
    /// Functions with hashes.
    pub functions: Vec<FunctionInfo>,
    /// Embedded strings from data sections.
    pub strings: Vec<String>,
    /// Non-fatal problems found while parsing (missing debug info, truncated section, ...).
    pub notes: Vec<String>,
}

impl BinaryModel {
    /// Value of a metadata key.
    #[must_use]
    pub fn meta(&self, key: &str) -> Option<&str> {
        self.metadata.iter().find(|m| m.key == key).map(|m| m.value.as_str())
    }
}
