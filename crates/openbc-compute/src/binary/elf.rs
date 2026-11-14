//! ELF extraction.

use lief::elf;
use lief::elf::dynamic::Entries;
use lief::generic::{Binary as _, Section as _, Symbol as _};
use openbc_core::binary_compare::{BinaryModel, SectionKind, SymbolInfo, SymbolKind};

use super::common::{build_functions, build_strings, hex, meta, section_info, Blob, Candidate};
use super::ParseOptions;

const SHF_EXECINSTR: u64 = 0x4;
const SHF_ALLOC: u64 = 0x2;
const SHF_WRITE: u64 = 0x1;

fn section_flags(raw: u64) -> String {
    let mut parts = Vec::new();
    if raw & SHF_ALLOC != 0 {
        parts.push("ALLOC");
    }
    if raw & SHF_WRITE != 0 {
        parts.push("WRITE");
    }
    if raw & SHF_EXECINSTR != 0 {
        parts.push("EXEC");
    }
    parts.join("|")
}

fn segment_flags(raw: u32) -> String {
    format!(
        "{}{}{}",
        if raw & 4 != 0 { 'R' } else { '-' },
        if raw & 2 != 0 { 'W' } else { '-' },
        if raw & 1 != 0 { 'X' } else { '-' }
    )
}

/// Fill `model` from an ELF binary.
pub(super) fn extract(bin: &elf::Binary, options: &ParseOptions, model: &mut BinaryModel) {
    model.format = "ELF".to_owned();
    let header = bin.header();
    meta(model, "format", "ELF");
    meta(model, "class", format!("{:?}", header.identity_class()));
    meta(model, "endianness", format!("{:?}", header.identity_data()));
    meta(model, "architecture", format!("{:?}", header.machine_type()));
    meta(model, "file type", format!("{:?}", header.file_type()));
    meta(model, "os abi", format!("{:?}", header.identity_os_abi()));
    meta(model, "entry point", hex(bin.entrypoint()));
    meta(model, "position independent", bin.is_pie().to_string());
    meta(model, "nx (non-executable stack)", bin.has_nx().to_string());
    meta(model, "interpreter", bin.interpreter());
    meta(model, "compile timestamp", "not recorded in ELF");

    for entry in bin.dynamic_entries() {
        match entry {
            Entries::Library(lib) => meta(model, &format!("needed library: {}", lib.name()), "required"),
            Entries::SharedObject(so) => meta(model, "soname", so.name()),
            Entries::RunPath(r) => meta(model, "runpath", r.runpath()),
            Entries::Rpath(r) => meta(model, "rpath", r.rpath()),
            _ => {}
        }
    }

    // --- sections / blobs ---------------------------------------------------------------
    let sections: Vec<elf::Section> = bin.sections().collect();
    for sec in &sections {
        let flags = sec.flags().bits();
        model.sections.push(section_info(
            sec.name(),
            SectionKind::Section,
            sec.virtual_address(),
            sec.size(),
            section_flags(flags),
            sec.content(),
        ));
        match sec.name().as_str() {
            ".comment" => {
                let text = String::from_utf8_lossy(sec.content());
                let mut seen: Vec<&str> = Vec::new();
                for line in text.split('\0').map(str::trim).filter(|l| !l.is_empty()) {
                    if !seen.contains(&line) {
                        seen.push(line);
                    }
                }
                meta(model, "compiler (.comment)", seen.join(" | "));
            }
            ".note.gnu.build-id" => {
                let c = sec.content();
                // note header is 16 bytes (namesz, descsz, type, "GNU\0"); the id follows.
                if c.len() > 16 {
                    meta(model, "build id", crate::digest::to_hex(&c[16..]));
                }
            }
            _ => {}
        }
    }
    let blobs: Vec<Blob<'_>> = sections
        .iter()
        .map(|sec| Blob {
            name: sec.name(),
            address: sec.virtual_address(),
            executable: sec.flags().bits() & SHF_EXECINSTR != 0,
            data: sec.content(),
        })
        .collect();

    for seg in bin.segments() {
        model.sections.push(section_info(
            format!("{:?}", seg.p_type()),
            SectionKind::Segment,
            seg.virtual_address(),
            seg.physical_size(),
            segment_flags(seg.flags()),
            seg.content(),
        ));
    }

    // --- DWARF producer = the real compiler flags --------------------------------------
    match bin.debug_info() {
        Some(lief::DebugInfo::Dwarf(dwarf)) => {
            let mut producers: Vec<String> = Vec::new();
            for cu in dwarf.compilation_units() {
                let p = cu.producer();
                if !p.is_empty() && !producers.contains(&p) && producers.len() < 8 {
                    producers.push(p);
                }
            }
            meta(model, "compiler flags (DWARF producer)", producers.join(" | "));
        }
        Some(_) => {}
        None => model.notes.push("DWARF info unavailable (absent, or LIEF built without extended support): compiler flags and local symbols come from .comment/.symtab only".to_owned()),
    }

    // --- symbols -------------------------------------------------------------------------
    let mut candidates: Vec<Candidate> = Vec::new();
    let mut exported_names = std::collections::HashSet::new();
    for sym in bin.exported_symbols() {
        let name = sym.name();
        if name.is_empty() {
            continue;
        }
        exported_names.insert(name.clone());
        model.symbols.push(SymbolInfo { name: name.clone(), kind: SymbolKind::Exported, address: sym.value(), library: None });
        if is_function(&sym) {
            candidates.push(Candidate { name, address: sym.value(), size: sym.size() });
        }
    }
    for sym in bin.imported_symbols() {
        let name = sym.name();
        if !name.is_empty() {
            model.symbols.push(SymbolInfo { name, kind: SymbolKind::Imported, address: 0, library: None });
        }
    }
    for sym in bin.symtab_symbols() {
        let name = sym.name();
        if name.is_empty() || sym.value() == 0 {
            continue;
        }
        if is_function(&sym) {
            candidates.push(Candidate { name: name.clone(), address: sym.value(), size: sym.size() });
        }
        if !exported_names.contains(&name) {
            model.symbols.push(SymbolInfo { name, kind: SymbolKind::Local, address: sym.value(), library: None });
        }
    }

    build_functions(candidates, &blobs, 0, options, model);
    build_strings(&blobs, options, model);
}

fn is_function(sym: &elf::Symbol) -> bool {
    matches!(sym.get_type(), elf::symbol::Type::FUNC | elf::symbol::Type::GNU_IFUNC)
}
