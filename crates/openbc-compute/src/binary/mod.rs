//! LIEF-backed extraction of PE / ELF / Mach-O files into `openbc_core::binary_compare::BinaryModel`.
//!
//! This module is **read-only**: it parses with LIEF, hashes with SHA-256 and never calls any LIEF
//! mutator or builder.
//!
//! # What is extracted
//!
//! | Part      | ELF                                  | PE                                   | Mach-O                         |
//! |-----------|--------------------------------------|--------------------------------------|--------------------------------|
//! | metadata  | class, ABI, type, machine, entry, interpreter, `DT_NEEDED`, `.comment`, build-id, DWARF producers | machine, subsystem, linker version, **timestamp**, PDB path, Rich-header tools | cpu, file type, flags, dylibs |
//! | sections  | sections + program headers           | sections                             | sections + segments            |
//! | symbols   | dynamic exports / imports, symtab    | export table, import table, COFF     | exports, binds, symtab         |
//! | functions | symbols of type FUNC with a size     | exports + COFF symbols               | exports + symtab               |
//!
//! Function bytes are cut out of the containing section. When a format records no size the
//! function extends to the next function start (`size_estimated`).

mod common;
mod elf;
mod macho;
mod pe;

use std::fs::File;
use std::io::Read;
use std::path::Path;

use openbc_core::binary_compare::BinaryModel;
use openbc_core::HashAlgorithm;
use thiserror::Error;

use crate::digest;

/// Extraction failure for one file.
#[derive(Debug, Error)]
pub enum BinaryError {
    /// The file could not be read.
    #[error("cannot read {path}: {source}")]
    Io {
        /// Offending path.
        path: String,
        /// OS error.
        #[source]
        source: std::io::Error,
    },
    /// The file is larger than [`ParseOptions::max_file_bytes`].
    #[error("{path} is {size} bytes, above the {limit} byte limit")]
    TooLarge {
        /// Offending path.
        path: String,
        /// Actual size.
        size: u64,
        /// Configured limit.
        limit: u64,
    },
    /// LIEF does not recognise the file as PE, ELF, Mach-O or COFF.
    #[error("{0} is not a supported executable (PE, ELF, Mach-O or COFF)")]
    Unsupported(String),
    /// A COFF object was recognised but this tool only reports basic information for it.
    #[error("{0}")]
    Other(String),
}

/// Knobs for [`parse_binary`].
#[derive(Clone, Debug)]
pub struct ParseOptions {
    /// Minimum characters for an extracted string.
    pub min_string_len: usize,
    /// Refuse files above this size (LIEF holds the parsed file in memory).
    pub max_file_bytes: u64,
    /// For fat Mach-O files: prefer the slice whose architecture contains this text
    /// (case-insensitive, e.g. `arm64`). Falls back to the first slice.
    pub prefer_arch: Option<String>,
    /// Largest single function hashed; bigger ones are skipped with a note.
    pub max_function_bytes: u64,
}

impl Default for ParseOptions {
    fn default() -> Self {
        Self {
            min_string_len: openbc_core::binary_compare::strings::DEFAULT_MIN_LEN,
            max_file_bytes: 1024 * 1024 * 1024,
            prefer_arch: None,
            max_function_bytes: 16 * 1024 * 1024,
        }
    }
}

/// SHA-256 of a whole file, streamed in 1 MiB blocks so it never needs the file in memory.
pub fn file_sha256(path: &Path) -> Result<(String, u64), BinaryError> {
    use sha2::{Digest, Sha256};
    let io = |source| BinaryError::Io { path: path.display().to_string(), source };
    let mut file = File::open(path).map_err(io)?;
    let mut hasher = Sha256::new();
    let mut buf = vec![0u8; 1024 * 1024];
    let mut total = 0u64;
    loop {
        let n = file.read(&mut buf).map_err(io)?;
        if n == 0 {
            break;
        }
        hasher.update(&buf[..n]);
        total += n as u64;
    }
    Ok((digest::to_hex(&hasher.finalize()), total))
}

/// Parse one binary into the format-neutral model.
pub fn parse_binary(path: &Path, options: &ParseOptions) -> Result<BinaryModel, BinaryError> {
    let display = path.display().to_string();
    let size = std::fs::metadata(path)
        .map_err(|source| BinaryError::Io { path: display.clone(), source })?
        .len();
    if size > options.max_file_bytes {
        return Err(BinaryError::TooLarge { path: display, size, limit: options.max_file_bytes });
    }
    let (file_sha256, file_size) = file_sha256(path)?;

    let parsed = lief::Binary::parse(path).ok_or_else(|| BinaryError::Unsupported(display.clone()))?;
    let mut model = BinaryModel {
        label: path.file_name().map_or_else(|| display.clone(), |n| n.to_string_lossy().into_owned()),
        file_size,
        file_sha256,
        ..BinaryModel::default()
    };
    match parsed {
        lief::Binary::ELF(bin) => elf::extract(&bin, options, &mut model),
        lief::Binary::PE(bin) => pe::extract(&bin, options, &mut model),
        lief::Binary::MachO(fat) => macho::extract(&fat, options, &mut model)?,
        lief::Binary::COFF(_) => {
            return Err(BinaryError::Other(format!(
                "{display}: COFF object files are recognised but not compared in detail"
            )))
        }
    }
    Ok(model)
}

/// Architecture text of a model (metadata key `architecture`), used to pick matching Mach-O slices.
#[must_use]
pub fn architecture_of(model: &BinaryModel) -> Option<String> {
    model.meta("architecture").map(str::to_owned)
}

/// Parse both versions. For fat Mach-O the second file is parsed for the first file's architecture
/// so a universal binary is compared slice-to-slice.
pub fn parse_pair(
    v1: &Path,
    v2: &Path,
    options: &ParseOptions,
) -> Result<(BinaryModel, BinaryModel), BinaryError> {
    let left = parse_binary(v1, options)?;
    let mut opts2 = options.clone();
    if opts2.prefer_arch.is_none() {
        opts2.prefer_arch = architecture_of(&left);
    }
    let right = parse_binary(v2, &opts2)?;
    Ok((left, right))
}

/// Hashing algorithm for section/function hashes (fixed by the requirements).
pub(crate) const CONTENT_HASH: HashAlgorithm = HashAlgorithm::Sha256;

#[cfg(test)]
mod tests;
