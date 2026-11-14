//! Mach-O (macOS / iOS .dylib, executables, bundles) extraction.

use lief::generic::{Binary as _, Section as _, Symbol as _};
use lief::macho;
use lief::macho::section::MachOSection as _;
use openbc_core::binary_compare::{BinaryModel, SectionKind, SymbolInfo, SymbolKind};

use super::common::{build_functions, build_strings, hex, meta, section_info, Blob, Candidate};
use super::{BinaryError, ParseOptions};

/// Sections that hold code even though their segment (`__TEXT`) also holds read-only data.
fn is_code_section(name: &str) -> bool {
    matches!(name, "__text" | "__stubs" | "__stub_helper" | "__auth_stubs")
}

/// Fill `model` from a (possibly fat) Mach-O.
pub(super) fn extract(
    fat: &macho::FatBinary,
    options: &ParseOptions,
    model: &mut BinaryModel,
) -> Result<(), BinaryError> {
    let slices: Vec<macho::Binary> = fat.iter().collect();
    if slices.is_empty() {
        return Err(BinaryError::Other("Mach-O container has no slices".to_owned()));
    }
    let arch_of = |b: &macho::Binary| format!("{:?}", b.header().cpu_type());
    let wanted = options.prefer_arch.as_ref().map(|a| a.to_lowercase());
    let index = wanted
        .as_ref()
        .and_then(|w| slices.iter().position(|b| arch_of(b).to_lowercase().contains(w.as_str())))
        .unwrap_or(0);
    if slices.len() > 1 {
        model.notes.push(format!(
            "universal binary with {} slices; compared slice {} ({})",
            slices.len(),
            index,
            arch_of(&slices[index])
        ));
    }
    let bin = &slices[index];

    model.format = "Mach-O".to_owned();
    let header = bin.header();
    meta(model, "format", "Mach-O");
    meta(model, "architecture", arch_of(bin));
    meta(model, "file type", format!("{:?}", header.file_type()));
    meta(model, "flags", format!("{:?}", header.flags()));
    meta(model, "bits", if header.is_64bit() { "64" } else { "32" });
    meta(model, "entry point", hex(bin.entrypoint()));
    meta(model, "position independent", bin.is_pie().to_string());
    meta(model, "nx", bin.has_nx().to_string());
    meta(model, "compile timestamp", "not recorded in Mach-O");
    for lib in bin.libraries() {
        meta(model, &format!("linked library: {}", lib.name()), "required");
    }

    let sections: Vec<macho::Section> = bin.sections().collect();
    for sec in &sections {
        let name = format!("{},{}", sec.segment_name(), sec.name());
        model.sections.push(section_info(
            name,
            SectionKind::Section,
            sec.virtual_address(),
            sec.size(),
            String::new(),
            sec.content(),
        ));
    }
    let blobs: Vec<Blob<'_>> = sections
        .iter()
        .map(|sec| Blob {
            name: sec.name(),
            address: sec.virtual_address(),
            executable: is_code_section(&sec.name()),
            data: sec.content(),
        })
        .collect();

    for seg in bin.segments() {
        model.sections.push(section_info(
            seg.name(),
            SectionKind::Segment,
            seg.virtual_address(),
            seg.file_size(),
            String::new(),
            seg.content(),
        ));
    }

    let mut candidates: Vec<Candidate> = Vec::new();
    let mut exported = std::collections::HashSet::new();
    for sym in bin.exported_symbols() {
        let name = sym.name();
        if name.is_empty() {
            continue;
        }
        exported.insert(name.clone());
        model.symbols.push(SymbolInfo { name: name.clone(), kind: SymbolKind::Exported, address: sym.value(), library: None });
        candidates.push(Candidate { name, address: sym.value(), size: 0 });
    }
    for sym in bin.symbols() {
        let name = sym.name();
        if name.is_empty() || exported.contains(&name) {
            continue;
        }
        let undefined = matches!(sym.category(), macho::symbol::Category::UNDEFINED)
            || matches!(sym.origin(), macho::symbol::Origin::DYLD_BIND);
        if undefined {
            let library = sym.library().map(|l| l.name());
            model.symbols.push(SymbolInfo { name, kind: SymbolKind::Imported, address: 0, library });
        } else if sym.value() != 0 {
            candidates.push(Candidate { name: name.clone(), address: sym.value(), size: 0 });
            model.symbols.push(SymbolInfo { name, kind: SymbolKind::Local, address: sym.value(), library: None });
        }
    }

    build_functions(candidates, &blobs, 0, options, model);
    build_strings(&blobs, options, model);
    Ok(())
}
