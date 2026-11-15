//! How the result tree is shown: unchanged visibility, demangling and the granular change filters.
//!
//! The filters hide *routine, low-signal* differences so the reviewer can concentrate on code and
//! data. They only affect what the tree and the totals show; the underlying diff is untouched, so
//! switching a filter never re-parses a file, and the number of hidden changes is always reported.
//!
//! | Flag                 | Hides (changed elements only)                                              |
//! |----------------------|-----------------------------------------------------------------------------|
//! | `HIDE_SYMTAB_STRTAB` | `.symtab`, `.strtab`, `.shstrtab` and Mach-O `__LINKEDIT`                  |
//! | `HIDE_DEBUG_NOTES`   | debug sections, notes, `.comment`, debug links, build-id / timestamp / PDB / compiler metadata |
//! | `HIDE_PADDING`       | sections and functions whose only difference is trailing alignment padding |
//! | `HIDE_LOCAL_SYMBOLS` | the Local / Debug Symbols list                                             |

use super::diff::Entry;
use super::model::{FunctionInfo, MetaEntry, SectionInfo, SectionKind};

/// Hide changes in the symbol and string tables of the object file.
pub const HIDE_SYMTAB_STRTAB: u32 = 1;
/// Hide debug information, notes and build-identity metadata.
pub const HIDE_DEBUG_NOTES: u32 = 2;
/// Hide sections / functions that differ only in trailing padding.
pub const HIDE_PADDING: u32 = 4;
/// Hide changes among local / debug symbols.
pub const HIDE_LOCAL_SYMBOLS: u32 = 8;
/// Every defined flag.
pub const HIDE_ALL: u32 = HIDE_SYMTAB_STRTAB | HIDE_DEBUG_NOTES | HIDE_PADDING | HIDE_LOCAL_SYMBOLS;

/// Presentation options for [`super::tree::build_tree_with`].
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ViewOptions {
    /// List unchanged elements too.
    pub show_unchanged: bool,
    /// Show C++ names demangled (identity is still the raw name).
    pub demangle: bool,
    /// Bit set of `HIDE_*` flags.
    pub hide: u32,
}

impl Default for ViewOptions {
    fn default() -> Self {
        Self { show_unchanged: true, demangle: true, hide: 0 }
    }
}

/// Name of an entry's section as stored (without the `#2` duplicate suffix of the key).
fn section_of(e: &Entry<SectionInfo>) -> Option<&SectionInfo> {
    e.right.as_ref().or(e.left.as_ref())
}

fn is_symtab_strtab(s: &SectionInfo) -> bool {
    match s.kind {
        SectionKind::Section => matches!(s.name.as_str(), ".symtab" | ".strtab" | ".shstrtab"),
        SectionKind::Segment => s.name == "__LINKEDIT",
    }
}

fn is_debug_or_note(s: &SectionInfo) -> bool {
    let n = s.name.as_str();
    match s.kind {
        SectionKind::Section => {
            n.starts_with(".debug")
                || n.starts_with(".zdebug")
                || n.starts_with(".note")
                || n.starts_with(".stab")
                || n == ".comment"
                || n == ".gnu_debuglink"
                || n == ".gnu_debugaltlink"
                || n.contains(",__debug")
        }
        SectionKind::Segment => n == "NOTE" || n == "__DWARF",
    }
}

fn is_identity_metadata(key: &str) -> bool {
    key.contains("timestamp")
        || key.contains("build id")
        || key.contains("guid")
        || key.starts_with("pdb ")
        || key.starts_with("compiler")
        || key.starts_with("rich header")
        || key.contains("checksum")
}

fn padding_only(a_full: &str, b_full: &str, a_trim: &str, b_trim: &str) -> bool {
    a_full != b_full && !a_trim.is_empty() && a_trim == b_trim
}

impl ViewOptions {
    fn on(&self, flag: u32) -> bool {
        self.hide & flag != 0
    }

    /// `true` when `e` is a changed section that the active filters hide.
    #[must_use]
    pub fn hides_section(&self, e: &Entry<SectionInfo>) -> bool {
        if !e.state.is_change() {
            return false;
        }
        let Some(s) = section_of(e) else { return false };
        if self.on(HIDE_SYMTAB_STRTAB) && is_symtab_strtab(s) {
            return true;
        }
        if self.on(HIDE_DEBUG_NOTES) && is_debug_or_note(s) {
            return true;
        }
        self.on(HIDE_PADDING) && section_padding_only(e)
    }

    /// `true` when `e` is a changed function that the active filters hide.
    #[must_use]
    pub fn hides_function(&self, e: &Entry<FunctionInfo>) -> bool {
        e.state.is_change() && self.on(HIDE_PADDING) && function_padding_only(e)
    }

    /// `true` when `e` is a changed metadata property that the active filters hide.
    #[must_use]
    pub fn hides_metadata(&self, e: &Entry<MetaEntry>) -> bool {
        e.state.is_change() && self.on(HIDE_DEBUG_NOTES) && is_identity_metadata(&e.key)
    }

    /// `true` when changed local symbols are hidden.
    #[must_use]
    pub fn hides_local_symbols(&self) -> bool {
        self.on(HIDE_LOCAL_SYMBOLS)
    }
}

/// Both sides exist, their hashes differ and only trailing padding differs.
#[must_use]
pub fn section_padding_only(e: &Entry<SectionInfo>) -> bool {
    match (&e.left, &e.right) {
        (Some(a), Some(b)) => padding_only(&a.sha256, &b.sha256, &a.sha256_unpadded, &b.sha256_unpadded),
        _ => false,
    }
}

/// Both sides exist, their hashes differ and only trailing padding differs.
#[must_use]
pub fn function_padding_only(e: &Entry<FunctionInfo>) -> bool {
    match (&e.left, &e.right) {
        (Some(a), Some(b)) => padding_only(&a.sha256, &b.sha256, &a.sha256_unpadded, &b.sha256_unpadded),
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::super::ChangeState;
    use super::*;

    fn section(name: &str, kind: SectionKind, sha: &str, unpadded: &str) -> SectionInfo {
        SectionInfo {
            name: name.into(),
            kind,
            address: 0,
            size: 8,
            flags: String::new(),
            sha256: sha.into(),
            sha256_unpadded: unpadded.into(),
            file_offset: None,
        }
    }

    fn modified(a: SectionInfo, b: SectionInfo) -> Entry<SectionInfo> {
        Entry { key: a.name.clone(), state: ChangeState::Modified, detail: String::new(), left: Some(a), right: Some(b) }
    }

    fn opts(hide: u32) -> ViewOptions {
        ViewOptions { hide, ..ViewOptions::default() }
    }

    #[test]
    fn symtab_and_strtab_are_hidden_only_when_asked() {
        let e = modified(section(".symtab", SectionKind::Section, "a", "a"), section(".symtab", SectionKind::Section, "b", "b"));
        assert!(!opts(0).hides_section(&e));
        assert!(opts(HIDE_SYMTAB_STRTAB).hides_section(&e));
        let text = modified(section(".text", SectionKind::Section, "a", "a"), section(".text", SectionKind::Section, "b", "b"));
        assert!(!opts(HIDE_ALL).hides_section(&text), "functional code is never filtered");
    }

    #[test]
    fn padding_only_requires_equal_unpadded_hashes() {
        let pad = modified(section(".data", SectionKind::Section, "a", "z"), section(".data", SectionKind::Section, "b", "z"));
        let real = modified(section(".data", SectionKind::Section, "a", "y"), section(".data", SectionKind::Section, "b", "z"));
        assert!(opts(HIDE_PADDING).hides_section(&pad));
        assert!(!opts(HIDE_PADDING).hides_section(&real));
    }

    #[test]
    fn unchanged_entries_are_never_hidden_and_metadata_filters_by_key() {
        let mut e = modified(section(".symtab", SectionKind::Section, "a", "a"), section(".symtab", SectionKind::Section, "a", "a"));
        e.state = ChangeState::Unchanged;
        assert!(!opts(HIDE_ALL).hides_section(&e));
        let meta = |k: &str| Entry {
            key: k.to_owned(),
            state: ChangeState::Modified,
            detail: String::new(),
            left: Some(MetaEntry::new(k, "1")),
            right: Some(MetaEntry::new(k, "2")),
        };
        assert!(opts(HIDE_DEBUG_NOTES).hides_metadata(&meta("build id")));
        assert!(opts(HIDE_DEBUG_NOTES).hides_metadata(&meta("compile timestamp")));
        assert!(!opts(HIDE_DEBUG_NOTES).hides_metadata(&meta("entry point")));
    }
}
