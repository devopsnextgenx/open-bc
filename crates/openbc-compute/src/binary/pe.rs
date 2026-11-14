//! PE (Windows .exe / .dll / .sys) extraction.

use lief::generic::{Binary as _, Section as _, Symbol as _};
use lief::pe;
use openbc_core::binary_compare::{BinaryModel, SectionKind, SymbolInfo, SymbolKind};

use super::common::{build_functions, build_strings, hex, iso8601, meta, section_info, Blob, Candidate};
use super::ParseOptions;

const IMAGE_SCN_CNT_CODE: u32 = 0x0000_0020;
const IMAGE_SCN_MEM_EXECUTE: u32 = 0x2000_0000;
const IMAGE_SCN_MEM_READ: u32 = 0x4000_0000;
const IMAGE_SCN_MEM_WRITE: u32 = 0x8000_0000;

fn section_flags(raw: u32) -> String {
    format!(
        "{}{}{}",
        if raw & IMAGE_SCN_MEM_READ != 0 { 'R' } else { '-' },
        if raw & IMAGE_SCN_MEM_WRITE != 0 { 'W' } else { '-' },
        if raw & IMAGE_SCN_MEM_EXECUTE != 0 { 'X' } else { '-' }
    )
}

/// Fill `model` from a PE binary.
pub(super) fn extract(bin: &pe::Binary, options: &ParseOptions, model: &mut BinaryModel) {
    model.format = "PE".to_owned();
    let header = bin.header();
    let optional = bin.optional_header();
    meta(model, "format", "PE");
    meta(model, "architecture", format!("{:?}", header.machine()));
    meta(model, "characteristics", format!("{:?}", header.characteristics()));
    meta(model, "subsystem", format!("{:?}", optional.subsystem()));
    meta(
        model,
        "linker version",
        format!("{}.{}", optional.major_linker_version(), optional.minor_linker_version()),
    );
    meta(model, "entry point", hex(bin.entrypoint()));
    meta(model, "image base", hex(bin.imagebase()));
    meta(model, "position independent", bin.is_pie().to_string());
    meta(model, "nx compatible", bin.has_nx().to_string());
    let stamp = u64::from(header.time_date_stamp());
    meta(
        model,
        "compile timestamp",
        if stamp == 0 {
            "0 (zeroed: reproducible build)".to_owned()
        } else {
            format!("{} ({stamp})", iso8601(stamp))
        },
    );
    if let Some(rich) = bin.rich_header() {
        let tools: Vec<String> = rich
            .entries()
            .map(|e| format!("{}.{}x{}", e.id(), e.build_id(), e.count()))
            .collect();
        meta(model, "rich header (id.build x count)", tools.join(" "));
    }
    for entry in bin.debug() {
        if let pe::debug::Entries::CodeViewPDB(pdb) = entry {
            meta(model, "pdb path", pdb.filename());
            meta(model, "pdb guid", pdb.guid());
        }
    }

    let sections: Vec<pe::Section> = bin.sections().collect();
    for sec in &sections {
        let raw = sec.characteristics().bits();
        model.sections.push(section_info(
            sec.name(),
            SectionKind::Section,
            sec.virtual_address(),
            sec.size(),
            section_flags(raw),
            sec.content(),
        ));
    }
    let blobs: Vec<Blob<'_>> = sections
        .iter()
        .map(|sec| {
            let raw = sec.characteristics().bits();
            Blob {
                name: sec.name(),
                address: sec.virtual_address(),
                executable: raw & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE) != 0,
                data: sec.content(),
            }
        })
        .collect();

    let mut candidates: Vec<Candidate> = Vec::new();

    if let Some(export) = bin.export() {
        for entry in export.entries() {
            let name = entry.name();
            if name.is_empty() {
                continue;
            }
            model.symbols.push(SymbolInfo {
                name: name.clone(),
                kind: SymbolKind::Exported,
                address: u64::from(entry.function_rva()),
                library: None,
            });
            if !entry.is_forwarded() {
                candidates.push(Candidate { name, address: u64::from(entry.function_rva()), size: 0 });
            }
        }
    }

    for import in bin.imports() {
        let library = import.name();
        for entry in import.entries() {
            let name = entry.name();
            let name = if entry.is_ordinal() { format!("#ordinal {}", entry.ordinal()) } else { name };
            model.symbols.push(SymbolInfo {
                name,
                kind: SymbolKind::Imported,
                address: 0,
                library: Some(library.clone()),
            });
        }
    }

    for sym in bin.symbols() {
        let name = sym.name();
        if !name.is_empty() {
            model.symbols.push(SymbolInfo {
                name: name.clone(),
                kind: SymbolKind::Local,
                address: u64::from(sym.value()),
                library: None,
            });
        }
    }
    if model.symbols.iter().all(|s| s.kind != SymbolKind::Local) {
        model.notes.push("no COFF/PDB symbols: local debug symbols unavailable (PDB is a separate file)".to_owned());
    }

    build_functions(candidates, &blobs, bin.imagebase(), options, model);
    build_strings(&blobs, options, model);
}
