//! Pure-Rust PE / ELF / Mach-O extraction with the `object` crate.
//!
//! This is the backend used where LIEF has no prebuilt SDK (notably `x86_64-pc-windows-gnu`, the
//! MSYS2/UCRT64 build) and whenever the `binary-native` feature is on. It fills the same
//! format-neutral [`BinaryModel`] as the LIEF extractors (`pe.rs`, `elf.rs`, `macho.rs`), so
//! hashing, strings, function splitting, diffing and the viewer are shared and unchanged.
//!
//! Addresses follow the LIEF extractors: PE section / export addresses are RVAs (image-relative),
//! ELF / Mach-O addresses are virtual addresses.
//!
//! Known differences from the LIEF backend: no DWARF producer ("compiler flags") on ELF, no
//! Mach-O dyld-bind library names on imports, metadata value spellings (e.g. the architecture
//! name) come from `object`. Both sides of one comparison always use the same backend, so the
//! differences never show up as spurious changes.

use object::read::elf::{Dyn as _, ElfFile, FileHeader as ElfHeader, ProgramHeader as _};
use object::read::macho::{
    FatArch as _, LoadCommandVariant, MachHeader, MachOFatFile32, MachOFatFile64, MachOFile,
};
use object::read::pe::{ImageNtHeaders, ImageOptionalHeader as _, PeFile, PeFile32, PeFile64};
use object::{
    elf, Endianness, FileKind, LittleEndian as LE, Object, ObjectSection, ObjectSegment,
    ObjectSymbol, SectionFlags, SymbolKind as ObjSymbolKind,
};
use openbc_core::binary_compare::{BinaryModel, SectionKind, SymbolInfo, SymbolKind};

use super::common::{build_functions, build_strings, hex, iso8601, meta, section_info, Blob, Candidate};
use super::{BinaryError, ParseOptions};

/// Parse `data` (the whole file) into `model`. `display` names the file in error messages.
pub(super) fn extract(
    data: &[u8],
    display: &str,
    options: &ParseOptions,
    model: &mut BinaryModel,
) -> Result<(), BinaryError> {
    let other = |e: object::Error| BinaryError::Other(format!("{display}: {e}"));
    let kind = FileKind::parse(data).map_err(|_| BinaryError::Unsupported(display.to_owned()))?;
    match kind {
        FileKind::Pe32 => pe_extract(&PeFile32::parse(data).map_err(other)?, options, model),
        FileKind::Pe64 => pe_extract(&PeFile64::parse(data).map_err(other)?, options, model),
        FileKind::Elf32 => elf_extract(&ElfFile::<elf::FileHeader32<Endianness>>::parse(data).map_err(other)?, options, model),
        FileKind::Elf64 => elf_extract(&ElfFile::<elf::FileHeader64<Endianness>>::parse(data).map_err(other)?, options, model),
        FileKind::MachO32 => macho_extract(&MachOFile::<object::macho::MachHeader32<Endianness>, _>::parse(data).map_err(other)?, options, model),
        FileKind::MachO64 => macho_extract(&MachOFile::<object::macho::MachHeader64<Endianness>, _>::parse(data).map_err(other)?, options, model),
        FileKind::MachOFat32 => {
            let fat = MachOFatFile32::parse(data).map_err(other)?;
            let arches: Vec<(String, &[u8])> = fat
                .arches()
                .iter()
                .filter_map(|a| Some((format!("{:?}", a.architecture()), a.data(data).ok()?)))
                .collect();
            macho_fat(&arches, display, options, model)?
        }
        FileKind::MachOFat64 => {
            let fat = MachOFatFile64::parse(data).map_err(other)?;
            let arches: Vec<(String, &[u8])> = fat
                .arches()
                .iter()
                .filter_map(|a| Some((format!("{:?}", a.architecture()), a.data(data).ok()?)))
                .collect();
            macho_fat(&arches, display, options, model)?
        }
        FileKind::Coff | FileKind::CoffBig => {
            return Err(BinaryError::Other(format!(
                "{display}: COFF object files are recognised but not compared in detail"
            )))
        }
        _ => return Err(BinaryError::Unsupported(display.to_owned())),
    }
    Ok(())
}

// ------------------------------------------------------------------------------------- PE

const IMAGE_SCN_CNT_CODE: u32 = 0x0000_0020;
const IMAGE_SCN_MEM_EXECUTE: u32 = 0x2000_0000;
const IMAGE_SCN_MEM_READ: u32 = 0x4000_0000;
const IMAGE_SCN_MEM_WRITE: u32 = 0x8000_0000;

fn pe_section_flags(raw: u32) -> String {
    format!(
        "{}{}{}",
        if raw & IMAGE_SCN_MEM_READ != 0 { 'R' } else { '-' },
        if raw & IMAGE_SCN_MEM_WRITE != 0 { 'W' } else { '-' },
        if raw & IMAGE_SCN_MEM_EXECUTE != 0 { 'X' } else { '-' }
    )
}

fn coff_characteristics(raw: u16) -> String {
    const NAMES: [(u16, &str); 10] = [
        (0x0001, "RELOCS_STRIPPED"),
        (0x0002, "EXECUTABLE_IMAGE"),
        (0x0004, "LINE_NUMS_STRIPPED"),
        (0x0008, "LOCAL_SYMS_STRIPPED"),
        (0x0020, "LARGE_ADDRESS_AWARE"),
        (0x0100, "32BIT_MACHINE"),
        (0x0200, "DEBUG_STRIPPED"),
        (0x1000, "SYSTEM"),
        (0x2000, "DLL"),
        (0x4000, "UP_SYSTEM_ONLY"),
    ];
    let names: Vec<&str> = NAMES.iter().filter(|(bit, _)| raw & bit != 0).map(|(_, n)| *n).collect();
    if names.is_empty() { format!("0x{raw:x}") } else { names.join(" | ") }
}

fn pe_subsystem(raw: u16) -> String {
    match raw {
        1 => "NATIVE".to_owned(),
        2 => "WINDOWS_GUI".to_owned(),
        3 => "WINDOWS_CUI".to_owned(),
        5 => "OS2_CUI".to_owned(),
        7 => "POSIX_CUI".to_owned(),
        9 => "WINDOWS_CE_GUI".to_owned(),
        10 => "EFI_APPLICATION".to_owned(),
        11 => "EFI_BOOT_SERVICE_DRIVER".to_owned(),
        12 => "EFI_RUNTIME_DRIVER".to_owned(),
        14 => "XBOX".to_owned(),
        16 => "WINDOWS_BOOT_APPLICATION".to_owned(),
        n => format!("{n}"),
    }
}

/// Windows GUID text: the first three fields are little-endian in the CodeView record.
fn guid_text(g: [u8; 16]) -> String {
    format!(
        "{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
        u32::from_le_bytes([g[0], g[1], g[2], g[3]]),
        u16::from_le_bytes([g[4], g[5]]),
        u16::from_le_bytes([g[6], g[7]]),
        g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]
    )
}

fn pe_extract<'d, Pe: ImageNtHeaders>(
    file: &PeFile<'d, Pe, &'d [u8]>,
    options: &ParseOptions,
    model: &mut BinaryModel,
) {
    model.format = "PE".to_owned();
    let nt = file.nt_headers();
    let header = nt.file_header();
    let optional = nt.optional_header();
    let image_base = optional.image_base();
    let dll_chars = optional.dll_characteristics();

    meta(model, "format", "PE");
    meta(model, "architecture", format!("{:?}", file.architecture()));
    meta(model, "characteristics", coff_characteristics(header.characteristics.get(LE)));
    meta(model, "subsystem", pe_subsystem(optional.subsystem()));
    meta(
        model,
        "linker version",
        format!("{}.{}", optional.major_linker_version(), optional.minor_linker_version()),
    );
    meta(model, "entry point", hex(file.entry()));
    meta(model, "image base", hex(image_base));
    meta(model, "position independent", (dll_chars & 0x0040 != 0).to_string()); // DYNAMIC_BASE
    meta(model, "nx compatible", (dll_chars & 0x0100 != 0).to_string()); // NX_COMPAT
    let stamp = u64::from(header.time_date_stamp.get(LE));
    meta(
        model,
        "compile timestamp",
        if stamp == 0 {
            "0 (zeroed: reproducible build)".to_owned()
        } else {
            format!("{} ({stamp})", iso8601(stamp))
        },
    );
    if let Some(rich) = file.rich_header_info() {
        let tools: Vec<String> = rich
            .unmasked_entries()
            .map(|e| format!("{}.{}x{}", e.comp_id >> 16, e.comp_id & 0xffff, e.count))
            .collect();
        meta(model, "rich header (id.build x count)", tools.join(" "));
    }
    if let Ok(Some(pdb)) = file.pdb_info() {
        meta(model, "pdb path", String::from_utf8_lossy(pdb.path()).into_owned());
        meta(model, "pdb guid", guid_text(pdb.guid()));
    }

    // sections (RVA addresses, like LIEF)
    let sections: Vec<_> = file.sections().collect();
    let rva = |addr: u64| addr.saturating_sub(image_base);
    let mut blobs: Vec<Blob<'d>> = Vec::new();
    for sec in &sections {
        let raw = match sec.flags() {
            SectionFlags::Coff { characteristics } => characteristics,
            _ => 0,
        };
        let name = sec.name().unwrap_or("").to_owned();
        let content = sec.data().unwrap_or(&[]);
        let offset = sec.file_range().map(|(o, _)| o);
        model.sections.push(section_info(
            name.clone(),
            SectionKind::Section,
            rva(sec.address()),
            sec.size(),
            pe_section_flags(raw),
            content,
            offset,
        ));
        blobs.push(Blob {
            name,
            address: rva(sec.address()),
            executable: raw & (IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_CNT_CODE) != 0,
            data: content,
            file_offset: offset.unwrap_or(0),
        });
    }

    let mut candidates: Vec<Candidate> = Vec::new();

    if let Ok(exports) = file.exports() {
        for export in exports {
            let name = String::from_utf8_lossy(export.name()).into_owned();
            if name.is_empty() {
                continue;
            }
            let address = rva(export.address());
            model.symbols.push(SymbolInfo { name: name.clone(), kind: SymbolKind::Exported, address, library: None });
            candidates.push(Candidate { name, address, size: 0 });
        }
    }

    if let Ok(imports) = file.imports() {
        for import in imports {
            let library = String::from_utf8_lossy(import.library()).into_owned();
            let raw = import.name();
            let name = if raw.is_empty() { "#ordinal".to_owned() } else { String::from_utf8_lossy(raw).into_owned() };
            model.symbols.push(SymbolInfo { name, kind: SymbolKind::Imported, address: 0, library: Some(library) });
        }
    }

    for sym in file.symbols() {
        if let Ok(name) = sym.name() {
            if !name.is_empty() {
                model.symbols.push(SymbolInfo {
                    name: name.to_owned(),
                    kind: SymbolKind::Local,
                    address: sym.address(),
                    library: None,
                });
            }
        }
    }
    if model.symbols.iter().all(|s| s.kind != SymbolKind::Local) {
        model.notes.push("no COFF/PDB symbols: local debug symbols unavailable (PDB is a separate file)".to_owned());
    }

    build_functions(candidates, &blobs, image_base, options, model);
    build_strings(&blobs, options, model);
}

// ------------------------------------------------------------------------------------- ELF

const SHF_WRITE: u64 = 0x1;
const SHF_ALLOC: u64 = 0x2;
const SHF_EXECINSTR: u64 = 0x4;

fn elf_section_flags(raw: u64) -> String {
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

fn elf_segment_flags(raw: u32) -> String {
    format!(
        "{}{}{}",
        if raw & elf::PF_R != 0 { 'R' } else { '-' },
        if raw & elf::PF_W != 0 { 'W' } else { '-' },
        if raw & elf::PF_X != 0 { 'X' } else { '-' }
    )
}

fn elf_segment_name(p_type: u32) -> String {
    match p_type {
        elf::PT_NULL => "NULL".to_owned(),
        elf::PT_LOAD => "LOAD".to_owned(),
        elf::PT_DYNAMIC => "DYNAMIC".to_owned(),
        elf::PT_INTERP => "INTERP".to_owned(),
        elf::PT_NOTE => "NOTE".to_owned(),
        elf::PT_SHLIB => "SHLIB".to_owned(),
        elf::PT_PHDR => "PHDR".to_owned(),
        elf::PT_TLS => "TLS".to_owned(),
        elf::PT_GNU_EH_FRAME => "GNU_EH_FRAME".to_owned(),
        elf::PT_GNU_STACK => "GNU_STACK".to_owned(),
        elf::PT_GNU_RELRO => "GNU_RELRO".to_owned(),
        elf::PT_GNU_PROPERTY => "GNU_PROPERTY".to_owned(),
        n => format!("0x{n:x}"),
    }
}

fn elf_type_name(e_type: u16) -> String {
    match e_type {
        elf::ET_REL => "RELOCATABLE".to_owned(),
        elf::ET_EXEC => "EXECUTABLE".to_owned(),
        elf::ET_DYN => "DYNAMIC".to_owned(),
        elf::ET_CORE => "CORE".to_owned(),
        n => format!("0x{n:x}"),
    }
}

fn elf_os_abi(raw: u8) -> String {
    match raw {
        0 => "SYSV".to_owned(),
        2 => "NETBSD".to_owned(),
        3 => "GNU".to_owned(),
        6 => "SOLARIS".to_owned(),
        9 => "FREEBSD".to_owned(),
        12 => "OPENBSD".to_owned(),
        n => format!("{n}"),
    }
}

fn elf_extract<'d, E: ElfHeader<Endian = Endianness>>(
    file: &ElfFile<'d, E, &'d [u8]>,
    options: &ParseOptions,
    model: &mut BinaryModel,
) {
    model.format = "ELF".to_owned();
    let endian = file.endian();
    let header = file.elf_header();
    let data = file.data();
    let e_type = header.e_type(endian);

    meta(model, "format", "ELF");
    meta(model, "class", if file.is_64() { "CLASS64" } else { "CLASS32" });
    meta(model, "endianness", format!("{:?}", file.endianness()));
    meta(model, "architecture", format!("{:?}", file.architecture()));
    meta(model, "file type", elf_type_name(e_type));
    meta(model, "os abi", elf_os_abi(header.e_ident().os_abi));
    meta(model, "entry point", hex(file.entry()));
    meta(model, "position independent", (e_type == elf::ET_DYN).to_string());

    let mut nx = false;
    let mut interpreter = String::new();
    if let Ok(program_headers) = header.program_headers(endian, data) {
        for ph in program_headers {
            match ph.p_type(endian) {
                elf::PT_GNU_STACK => nx = ph.p_flags(endian) & elf::PF_X == 0,
                elf::PT_INTERP => {
                    if let Ok(bytes) = ph.data(endian, data) {
                        interpreter = String::from_utf8_lossy(bytes).trim_end_matches('\0').to_owned();
                    }
                }
                _ => {}
            }
        }
    }
    meta(model, "nx (non-executable stack)", nx.to_string());
    meta(model, "interpreter", interpreter);
    meta(model, "compile timestamp", "not recorded in ELF");

    // DT_NEEDED / DT_SONAME / DT_RUNPATH / DT_RPATH
    if let Ok(sections) = header.sections(endian, data) {
        if let Ok(Some((dynamic, link))) = sections.dynamic(endian, data) {
            if let Ok(strings) = sections.strings(endian, data, link) {
                for entry in dynamic {
                    let Some(tag) = entry.tag32(endian) else { continue };
                    if !entry.is_string(endian) {
                        continue;
                    }
                    let Ok(raw) = entry.string(endian, strings) else { continue };
                    let text = String::from_utf8_lossy(raw).into_owned();
                    match tag {
                        elf::DT_NEEDED => meta(model, &format!("needed library: {text}"), "required"),
                        elf::DT_SONAME => meta(model, "soname", text),
                        elf::DT_RUNPATH => meta(model, "runpath", text),
                        elf::DT_RPATH => meta(model, "rpath", text),
                        _ => {}
                    }
                }
            }
        }
    }

    // --- sections / blobs ---------------------------------------------------------------
    let sections: Vec<_> = file.sections().collect();
    let mut blobs: Vec<Blob<'d>> = Vec::new();
    for sec in &sections {
        let flags = match sec.flags() {
            SectionFlags::Elf { sh_flags } => sh_flags,
            _ => 0,
        };
        let name = sec.name().unwrap_or("").to_owned();
        let content = sec.data().unwrap_or(&[]);
        let offset = sec.file_range().map(|(o, _)| o);
        model.sections.push(section_info(
            name.clone(),
            SectionKind::Section,
            sec.address(),
            sec.size(),
            elf_section_flags(flags),
            content,
            offset,
        ));
        match name.as_str() {
            ".comment" => {
                let text = String::from_utf8_lossy(content);
                let mut seen: Vec<&str> = Vec::new();
                for line in text.split('\0').map(str::trim).filter(|l| !l.is_empty()) {
                    if !seen.contains(&line) {
                        seen.push(line);
                    }
                }
                meta(model, "compiler (.comment)", seen.join(" | "));
            }
            ".note.gnu.build-id" => {
                // note header is 16 bytes (namesz, descsz, type, "GNU\0"); the id follows.
                if content.len() > 16 {
                    meta(model, "build id", crate::digest::to_hex(&content[16..]));
                }
            }
            _ => {}
        }
        blobs.push(Blob {
            name,
            address: sec.address(),
            executable: flags & SHF_EXECINSTR != 0,
            data: content,
            file_offset: offset.unwrap_or(0),
        });
    }

    if let Ok(program_headers) = header.program_headers(endian, data) {
        for ph in program_headers {
            let content = ph.data(endian, data).unwrap_or(&[]);
            model.sections.push(section_info(
                elf_segment_name(ph.p_type(endian)),
                SectionKind::Segment,
                ph.p_vaddr(endian).into(),
                ph.p_filesz(endian).into(),
                elf_segment_flags(ph.p_flags(endian)),
                content,
                None,
            ));
        }
    }

    model.notes.push(
        "compiler flags (DWARF producer) are not read by the native backend: compiler info comes from .comment / .symtab only"
            .to_owned(),
    );

    // --- symbols -------------------------------------------------------------------------
    let is_function = |kind: ObjSymbolKind| matches!(kind, ObjSymbolKind::Text);
    let mut candidates: Vec<Candidate> = Vec::new();
    let mut exported_names = std::collections::HashSet::new();
    for sym in file.dynamic_symbols() {
        let Ok(name) = sym.name() else { continue };
        if name.is_empty() {
            continue;
        }
        if sym.is_undefined() {
            model.symbols.push(SymbolInfo { name: name.to_owned(), kind: SymbolKind::Imported, address: 0, library: None });
        } else if sym.is_global() {
            exported_names.insert(name.to_owned());
            model.symbols.push(SymbolInfo { name: name.to_owned(), kind: SymbolKind::Exported, address: sym.address(), library: None });
            if is_function(sym.kind()) {
                candidates.push(Candidate { name: name.to_owned(), address: sym.address(), size: sym.size() });
            }
        }
    }
    for sym in file.symbols() {
        let Ok(name) = sym.name() else { continue };
        if name.is_empty() || sym.address() == 0 || sym.is_undefined() {
            continue;
        }
        if is_function(sym.kind()) {
            candidates.push(Candidate { name: name.to_owned(), address: sym.address(), size: sym.size() });
        }
        if !exported_names.contains(name) {
            model.symbols.push(SymbolInfo { name: name.to_owned(), kind: SymbolKind::Local, address: sym.address(), library: None });
        }
    }

    build_functions(candidates, &blobs, 0, options, model);
    build_strings(&blobs, options, model);
}

// ----------------------------------------------------------------------------------- Mach-O

/// Sections that hold code even though their segment (`__TEXT`) also holds read-only data.
fn is_code_section(name: &str) -> bool {
    matches!(name, "__text" | "__stubs" | "__stub_helper" | "__auth_stubs")
}

fn macho_file_type(raw: u32) -> String {
    match raw {
        1 => "OBJECT".to_owned(),
        2 => "EXECUTE".to_owned(),
        3 => "FVMLIB".to_owned(),
        4 => "CORE".to_owned(),
        5 => "PRELOAD".to_owned(),
        6 => "DYLIB".to_owned(),
        7 => "DYLINKER".to_owned(),
        8 => "BUNDLE".to_owned(),
        9 => "DYLIB_STUB".to_owned(),
        10 => "DSYM".to_owned(),
        11 => "KEXT_BUNDLE".to_owned(),
        n => format!("0x{n:x}"),
    }
}

/// Pick the slice of a universal binary (preferred architecture, else the first) and extract it.
fn macho_fat(
    arches: &[(String, &[u8])],
    display: &str,
    options: &ParseOptions,
    model: &mut BinaryModel,
) -> Result<(), BinaryError> {
    if arches.is_empty() {
        return Err(BinaryError::Other("Mach-O container has no slices".to_owned()));
    }
    // `arm64` (Apple / LIEF spelling) and `aarch64` (`object` spelling) name the same CPU.
    let norm = |s: &str| s.to_lowercase().replace("arm64", "aarch64");
    let wanted = options.prefer_arch.as_deref().map(norm);
    let index = wanted
        .as_ref()
        .and_then(|w| arches.iter().position(|(arch, _)| norm(arch).contains(w.as_str())))
        .unwrap_or(0);
    if arches.len() > 1 {
        model.notes.push(format!(
            "universal binary with {} slices; compared slice {} ({})",
            arches.len(),
            index,
            arches[index].0
        ));
    }
    let slice = arches[index].1;
    let other = |e: object::Error| BinaryError::Other(format!("{display}: {e}"));
    match FileKind::parse(slice).map_err(|_| BinaryError::Unsupported(display.to_owned()))? {
        FileKind::MachO32 => macho_extract(&MachOFile::<object::macho::MachHeader32<Endianness>, _>::parse(slice).map_err(other)?, options, model),
        FileKind::MachO64 => macho_extract(&MachOFile::<object::macho::MachHeader64<Endianness>, _>::parse(slice).map_err(other)?, options, model),
        _ => return Err(BinaryError::Unsupported(display.to_owned())),
    }
    Ok(())
}

fn macho_extract<'d, Mach: MachHeader<Endian = Endianness>>(
    file: &MachOFile<'d, Mach, &'d [u8]>,
    options: &ParseOptions,
    model: &mut BinaryModel,
) {
    model.format = "Mach-O".to_owned();
    let endian = file.endian();
    let header = file.macho_header();
    meta(model, "format", "Mach-O");
    meta(model, "architecture", format!("{:?}", file.architecture()));
    meta(model, "file type", macho_file_type(header.filetype(endian)));
    meta(model, "flags", format!("0x{:x}", header.flags(endian)));
    meta(model, "bits", if file.is_64() { "64" } else { "32" });
    meta(model, "entry point", hex(file.entry()));
    meta(model, "position independent", (header.flags(endian) & object::macho::MH_PIE != 0).to_string());
    meta(model, "nx", (header.flags(endian) & object::macho::MH_NO_HEAP_EXECUTION != 0).to_string());
    meta(model, "compile timestamp", "not recorded in Mach-O");

    if let Ok(mut commands) = file.macho_load_commands() {
        while let Ok(Some(command)) = commands.next() {
            if let Ok(LoadCommandVariant::Dylib(dylib)) = command.variant() {
                if command.cmd() == object::macho::LC_LOAD_DYLIB
                    || command.cmd() == object::macho::LC_LOAD_WEAK_DYLIB
                    || command.cmd() == object::macho::LC_REEXPORT_DYLIB
                    || command.cmd() == object::macho::LC_LOAD_UPWARD_DYLIB
                {
                    if let Ok(name) = command.string(endian, dylib.dylib.name) {
                        meta(model, &format!("linked library: {}", String::from_utf8_lossy(name)), "required");
                    }
                }
            }
        }
    }

    let sections: Vec<_> = file.sections().collect();
    let mut blobs: Vec<Blob<'d>> = Vec::new();
    for sec in &sections {
        let name = sec.name().unwrap_or("").to_owned();
        let segment = sec.segment_name().ok().flatten().unwrap_or("").to_owned();
        let content = sec.data().unwrap_or(&[]);
        let offset = sec.file_range().map(|(o, _)| o);
        model.sections.push(section_info(
            format!("{segment},{name}"),
            SectionKind::Section,
            sec.address(),
            sec.size(),
            String::new(),
            content,
            offset,
        ));
        blobs.push(Blob {
            executable: is_code_section(&name),
            name,
            address: sec.address(),
            data: content,
            file_offset: offset.unwrap_or(0),
        });
    }

    for seg in file.segments() {
        let content = seg.data().unwrap_or(&[]);
        model.sections.push(section_info(
            seg.name().ok().flatten().unwrap_or("").to_owned(),
            SectionKind::Segment,
            seg.address(),
            content.len() as u64,
            String::new(),
            content,
            None,
        ));
    }

    let mut candidates: Vec<Candidate> = Vec::new();
    for sym in file.symbols() {
        let Ok(name) = sym.name() else { continue };
        if name.is_empty() {
            continue;
        }
        if sym.is_undefined() {
            model.symbols.push(SymbolInfo { name: name.to_owned(), kind: SymbolKind::Imported, address: 0, library: None });
        } else if sym.is_global() {
            model.symbols.push(SymbolInfo { name: name.to_owned(), kind: SymbolKind::Exported, address: sym.address(), library: None });
            candidates.push(Candidate { name: name.to_owned(), address: sym.address(), size: 0 });
        } else if sym.address() != 0 {
            candidates.push(Candidate { name: name.to_owned(), address: sym.address(), size: 0 });
            model.symbols.push(SymbolInfo { name: name.to_owned(), kind: SymbolKind::Local, address: sym.address(), library: None });
        }
    }

    build_functions(candidates, &blobs, 0, options, model);
    build_strings(&blobs, options, model);
}
