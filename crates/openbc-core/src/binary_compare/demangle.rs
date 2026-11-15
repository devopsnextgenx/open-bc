//! Display-only demangling of C++ symbol names.
//!
//! Matching between versions always uses the raw linkage name (the identity of an element); the
//! demangled text is only what the viewer shows, so turning demangling on or off never changes
//! which elements are classified as added, removed or modified.
//!
//! Supported manglings: Itanium (`_Z...`, also Mach-O's extra leading underscore `__Z...`) and
//! Microsoft Visual C++ (`?...`). Anything else (plain C names, Rust legacy names that are not
//! Itanium-shaped, ordinals) returns `None` and is shown as is.

use std::panic::{catch_unwind, AssertUnwindSafe};

/// Demangled form of `name`, or `None` when `name` is not a mangled C++ name or cannot be parsed.
///
/// An ELF symbol-version suffix (`@@GLIBCXX_3.4`) is kept after the demangled text.
#[must_use]
pub fn demangle(name: &str) -> Option<String> {
    // Parsers run on untrusted symbol tables: a bug in one must never take the worker down.
    catch_unwind(AssertUnwindSafe(|| demangle_inner(name))).ok().flatten()
}

fn demangle_inner(name: &str) -> Option<String> {
    if name.starts_with('?') {
        return msvc_demangler::demangle(name, msvc_demangler::DemangleFlags::llvm()).ok();
    }
    let itanium = if name.starts_with("__Z") { &name[1..] } else { name };
    if !itanium.starts_with("_Z") {
        return None;
    }
    let (base, version) = match itanium.find('@') {
        Some(at) => (&itanium[..at], &itanium[at..]),
        None => (itanium, ""),
    };
    let symbol = cpp_demangle::Symbol::new(base).ok()?;
    Some(format!("{symbol}{version}"))
}

/// Label for `key` (a raw linkage name, possibly with a `#2` duplicate suffix added by the
/// aligner): the demangled text plus the suffix, or `key` itself when it cannot be demangled.
#[must_use]
pub fn display_name(key: &str, raw_name: &str, enabled: bool) -> String {
    if !enabled {
        return key.to_owned();
    }
    match demangle(raw_name) {
        Some(text) => format!("{text}{}", key.strip_prefix(raw_name).unwrap_or("")),
        None => key.to_owned(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn itanium_names_are_demangled() {
        assert_eq!(demangle("_ZN3foo3barEv").as_deref(), Some("foo::bar()"));
        assert_eq!(demangle("_Z3addii").as_deref(), Some("add(int, int)"));
    }

    #[test]
    fn mach_o_double_underscore_and_versions() {
        assert_eq!(demangle("__Z3addii").as_deref(), Some("add(int, int)"));
        assert_eq!(demangle("_Z3addii@@VER_1").as_deref(), Some("add(int, int)@@VER_1"));
    }

    #[test]
    fn plain_and_broken_names_are_left_alone() {
        assert_eq!(demangle("main"), None);
        assert_eq!(demangle("_start"), None);
        assert_eq!(demangle("_Z"), None);
        assert_eq!(demangle(""), None);
        assert_eq!(demangle("#ordinal 7"), None);
    }

    #[test]
    fn display_name_keeps_duplicate_suffix_and_respects_toggle() {
        assert_eq!(display_name("_Z3addii#2", "_Z3addii", true), "add(int, int)#2");
        assert_eq!(display_name("_Z3addii", "_Z3addii", false), "_Z3addii");
        assert_eq!(display_name("main", "main", true), "main");
    }
}
