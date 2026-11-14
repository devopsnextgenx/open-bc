//! Version diff: classify every element of two [`BinaryModel`]s.
//!
//! Matching is by *identity*, never by address, so a function that only moved is `Unchanged`:
//!
//! | Element   | Identity                          | `Modified` when                          |
//! |-----------|-----------------------------------|------------------------------------------|
//! | metadata  | key                               | value differs                            |
//! | section   | kind + name (`#2` for duplicates) | SHA-256 differs                          |
//! | symbol    | kind + name                       | imported from a different library        |
//! | function  | name                              | SHA-256 of the bytes differs             |
//! | string    | text                              | removed + added pair at >= 80 % similar  |

use std::collections::{HashMap, HashSet};

use super::fuzzy;
use super::model::{
    BinaryModel, FunctionInfo, MetaEntry, SectionInfo, SectionKind, SymbolInfo, SymbolKind,
};
use super::ChangeState;

/// One compared element.
#[derive(Clone, Debug, PartialEq)]
pub struct Entry<T> {
    /// Identity used for matching, also the tree label.
    pub key: String,
    /// Classification.
    pub state: ChangeState,
    /// Short explanation of *what* changed (empty for unchanged).
    pub detail: String,
    /// Version 1 value, if present.
    pub left: Option<T>,
    /// Version 2 value, if present.
    pub right: Option<T>,
}

/// Counts per state for one list.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct DiffCounts {
    /// Matching elements.
    pub unchanged: usize,
    /// Elements with changed content.
    pub modified: usize,
    /// Version-1-only elements.
    pub removed: usize,
    /// Version-2-only elements.
    pub added: usize,
}

impl DiffCounts {
    /// Count the states of `entries`.
    #[must_use]
    pub fn of<T>(entries: &[Entry<T>]) -> Self {
        let mut c = Self::default();
        for e in entries {
            match e.state {
                ChangeState::Unchanged => c.unchanged += 1,
                ChangeState::Modified => c.modified += 1,
                ChangeState::Removed => c.removed += 1,
                ChangeState::Added => c.added += 1,
            }
        }
        c
    }

    /// Sum of all four.
    #[must_use]
    pub const fn total(&self) -> usize {
        self.unchanged + self.modified + self.removed + self.added
    }

    /// Elements that are not `Unchanged`.
    #[must_use]
    pub const fn changed(&self) -> usize {
        self.modified + self.removed + self.added
    }

    /// Add another count into this one.
    pub fn merge(&mut self, o: Self) {
        self.unchanged += o.unchanged;
        self.modified += o.modified;
        self.removed += o.removed;
        self.added += o.added;
    }
}

/// Complete result of comparing two binaries.
#[derive(Clone, Debug, PartialEq)]
pub struct BinaryDiff {
    /// Baseline label.
    pub left_label: String,
    /// Target label.
    pub right_label: String,
    /// `Unchanged` when the whole-file SHA-256 matches, else `Modified`.
    pub file_state: ChangeState,
    /// Whole-file hashes `(v1, v2)`.
    pub file_hashes: (String, String),
    /// Header and metadata properties.
    pub metadata: Vec<Entry<MetaEntry>>,
    /// Sections.
    pub sections: Vec<Entry<SectionInfo>>,
    /// Segments.
    pub segments: Vec<Entry<SectionInfo>>,
    /// Exported symbols.
    pub exported: Vec<Entry<SymbolInfo>>,
    /// Imported symbols.
    pub imported: Vec<Entry<SymbolInfo>>,
    /// Local / debug symbols.
    pub local: Vec<Entry<SymbolInfo>>,
    /// Functions.
    pub functions: Vec<Entry<FunctionInfo>>,
    /// Strings.
    pub strings: Vec<Entry<String>>,
    /// Parser notes of both sides, prefixed with `v1:` / `v2:`.
    pub notes: Vec<String>,
}

impl BinaryDiff {
    /// Totals over every list.
    #[must_use]
    pub fn totals(&self) -> DiffCounts {
        let mut c = DiffCounts::default();
        c.merge(DiffCounts::of(&self.metadata));
        c.merge(DiffCounts::of(&self.sections));
        c.merge(DiffCounts::of(&self.segments));
        c.merge(DiffCounts::of(&self.exported));
        c.merge(DiffCounts::of(&self.imported));
        c.merge(DiffCounts::of(&self.local));
        c.merge(DiffCounts::of(&self.functions));
        c.merge(DiffCounts::of(&self.strings));
        c
    }
}

/// Make keys unique within one side: second `.text` becomes `.text#2`.
fn unique_keys(keys: impl Iterator<Item = String>) -> Vec<String> {
    let mut seen: HashMap<String, usize> = HashMap::new();
    keys.map(|k| {
        let n = seen.entry(k.clone()).or_insert(0);
        *n += 1;
        if *n == 1 { k } else { format!("{k}#{n}") }
    })
    .collect()
}

/// Generic keyed alignment: left order first, then right-only items in right order.
fn align<T: Clone>(
    left: &[T],
    right: &[T],
    key: impl Fn(&T) -> String,
    judge: impl Fn(&T, &T) -> (ChangeState, String),
) -> Vec<Entry<T>> {
    let lk = unique_keys(left.iter().map(&key));
    let rk = unique_keys(right.iter().map(&key));
    let right_index: HashMap<&str, usize> =
        rk.iter().enumerate().map(|(i, k)| (k.as_str(), i)).collect();
    let mut matched = vec![false; right.len()];
    let mut out = Vec::with_capacity(left.len().max(right.len()));
    for (i, l) in left.iter().enumerate() {
        match right_index.get(lk[i].as_str()) {
            Some(&j) => {
                matched[j] = true;
                let (state, detail) = judge(l, &right[j]);
                out.push(Entry {
                    key: lk[i].clone(),
                    state,
                    detail,
                    left: Some(l.clone()),
                    right: Some(right[j].clone()),
                });
            }
            None => out.push(Entry {
                key: lk[i].clone(),
                state: ChangeState::Removed,
                detail: String::new(),
                left: Some(l.clone()),
                right: None,
            }),
        }
    }
    for (j, r) in right.iter().enumerate() {
        if !matched[j] {
            out.push(Entry {
                key: rk[j].clone(),
                state: ChangeState::Added,
                detail: String::new(),
                left: None,
                right: Some(r.clone()),
            });
        }
    }
    out
}

fn short(hash: &str) -> &str {
    &hash[..hash.len().min(12)]
}

fn diff_sections(left: &[SectionInfo], right: &[SectionInfo]) -> Vec<Entry<SectionInfo>> {
    align(
        left,
        right,
        |s| s.name.clone(),
        |a, b| {
            if a.sha256 == b.sha256 {
                return (ChangeState::Unchanged, String::new());
            }
            let size = if a.size == b.size {
                format!("{} bytes", a.size)
            } else {
                format!("{} \u{2192} {} bytes", a.size, b.size)
            };
            (
                ChangeState::Modified,
                format!("{size}; sha256 {} \u{2192} {}", short(&a.sha256), short(&b.sha256)),
            )
        },
    )
}

fn diff_symbols(left: &[&SymbolInfo], right: &[&SymbolInfo]) -> Vec<Entry<SymbolInfo>> {
    let l: Vec<SymbolInfo> = left.iter().map(|s| (*s).clone()).collect();
    let r: Vec<SymbolInfo> = right.iter().map(|s| (*s).clone()).collect();
    align(
        &l,
        &r,
        |s| s.name.clone(),
        |a, b| {
            if a.library == b.library {
                (ChangeState::Unchanged, String::new())
            } else {
                (
                    ChangeState::Modified,
                    format!(
                        "library {} \u{2192} {}",
                        a.library.as_deref().unwrap_or("-"),
                        b.library.as_deref().unwrap_or("-")
                    ),
                )
            }
        },
    )
}

fn diff_functions(left: &[FunctionInfo], right: &[FunctionInfo]) -> Vec<Entry<FunctionInfo>> {
    align(
        left,
        right,
        |f| f.name.clone(),
        |a, b| {
            if a.sha256 == b.sha256 {
                return (ChangeState::Unchanged, String::new());
            }
            let sim = fuzzy::similarity(a.simhash, b.simhash) * 100.0;
            (
                ChangeState::Modified,
                format!("{sim:.0}% similar; {} \u{2192} {} bytes", a.size, b.size),
            )
        },
    )
}

/// Pairs removed/added strings that are near-identical so a tweaked literal reads as `Modified`.
const STRING_PAIR_THRESHOLD: f64 = 0.8;
/// Hard bound on removed x added candidate pairs examined.
const STRING_PAIR_BUDGET: usize = 50_000;

fn diff_strings(left: &[String], right: &[String]) -> Vec<Entry<String>> {
    fn dedupe(v: &[String]) -> Vec<String> {
        let mut seen = HashSet::new();
        v.iter().filter(|s| seen.insert((*s).clone())).cloned().collect()
    }
    let l = dedupe(left);
    let r = dedupe(right);
    let rset: HashSet<&str> = r.iter().map(String::as_str).collect();
    let lset: HashSet<&str> = l.iter().map(String::as_str).collect();

    let removed: Vec<&String> = l.iter().filter(|s| !rset.contains(s.as_str())).collect();
    let added: Vec<&String> = r.iter().filter(|s| !lset.contains(s.as_str())).collect();

    // Pair near-identical removed/added strings, within the work budget.
    let mut paired: HashMap<&str, &String> = HashMap::new();
    let mut used_added: HashSet<&str> = HashSet::new();
    if removed.len().saturating_mul(added.len()) <= STRING_PAIR_BUDGET {
        for rem in &removed {
            let mut best: Option<(&String, f64)> = None;
            for add in &added {
                if used_added.contains(add.as_str()) {
                    continue;
                }
                let (a, b) = (rem.len() as f64, add.len() as f64);
                if (a - b).abs() > 0.25 * a.max(b) {
                    continue;
                }
                let score = strsim::normalized_levenshtein(rem, add);
                if score >= STRING_PAIR_THRESHOLD && best.map_or(true, |(_, s)| score > s) {
                    best = Some((add, score));
                }
            }
            if let Some((add, _)) = best {
                used_added.insert(add.as_str());
                paired.insert(rem.as_str(), add);
            }
        }
    }

    let mut out = Vec::with_capacity(l.len() + added.len());
    for s in &l {
        if rset.contains(s.as_str()) {
            out.push(Entry {
                key: s.clone(),
                state: ChangeState::Unchanged,
                detail: String::new(),
                left: Some(s.clone()),
                right: Some(s.clone()),
            });
        } else if let Some(new) = paired.get(s.as_str()) {
            out.push(Entry {
                key: s.clone(),
                state: ChangeState::Modified,
                detail: format!("\u{2192} {new}"),
                left: Some(s.clone()),
                right: Some((*new).clone()),
            });
        } else {
            out.push(Entry {
                key: s.clone(),
                state: ChangeState::Removed,
                detail: String::new(),
                left: Some(s.clone()),
                right: None,
            });
        }
    }
    for s in added.iter().filter(|s| !used_added.contains(s.as_str())) {
        out.push(Entry {
            key: (*s).clone(),
            state: ChangeState::Added,
            detail: String::new(),
            left: None,
            right: Some((*s).clone()),
        });
    }
    out
}

/// Compare version 1 (baseline) with version 2 (target).
#[must_use]
pub fn diff_binaries(v1: &BinaryModel, v2: &BinaryModel) -> BinaryDiff {
    let metadata = align(
        &v1.metadata,
        &v2.metadata,
        |m| m.key.clone(),
        |a, b| {
            if a.value == b.value {
                (ChangeState::Unchanged, String::new())
            } else {
                (ChangeState::Modified, format!("{} \u{2192} {}", a.value, b.value))
            }
        },
    );

    let split = |m: &BinaryModel, kind: SectionKind| -> Vec<SectionInfo> {
        m.sections.iter().filter(|s| s.kind == kind).cloned().collect()
    };
    let sections = diff_sections(&split(v1, SectionKind::Section), &split(v2, SectionKind::Section));
    let segments = diff_sections(&split(v1, SectionKind::Segment), &split(v2, SectionKind::Segment));

    fn pick(m: &BinaryModel, kind: SymbolKind) -> Vec<&SymbolInfo> {
        m.symbols.iter().filter(|s| s.kind == kind).collect()
    }

    let mut notes: Vec<String> = v1.notes.iter().map(|n| format!("v1: {n}")).collect();
    notes.extend(v2.notes.iter().map(|n| format!("v2: {n}")));
    if v1.format != v2.format && !v1.format.is_empty() && !v2.format.is_empty() {
        notes.push(format!("formats differ: {} vs {}", v1.format, v2.format));
    }

    BinaryDiff {
        left_label: v1.label.clone(),
        right_label: v2.label.clone(),
        file_state: if v1.file_sha256 == v2.file_sha256 {
            ChangeState::Unchanged
        } else {
            ChangeState::Modified
        },
        file_hashes: (v1.file_sha256.clone(), v2.file_sha256.clone()),
        metadata,
        sections,
        segments,
        exported: diff_symbols(&pick(v1, SymbolKind::Exported), &pick(v2, SymbolKind::Exported)),
        imported: diff_symbols(&pick(v1, SymbolKind::Imported), &pick(v2, SymbolKind::Imported)),
        local: diff_symbols(&pick(v1, SymbolKind::Local), &pick(v2, SymbolKind::Local)),
        functions: diff_functions(&v1.functions, &v2.functions),
        strings: diff_strings(&v1.strings, &v2.strings),
        notes,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sec(name: &str, hash: &str, addr: u64) -> SectionInfo {
        SectionInfo {
            name: name.into(),
            kind: SectionKind::Section,
            address: addr,
            size: 16,
            flags: "R-X".into(),
            sha256: hash.into(),
        }
    }
    fn func(name: &str, hash: &str, addr: u64) -> FunctionInfo {
        FunctionInfo {
            name: name.into(),
            address: addr,
            size: 8,
            size_estimated: false,
            sha256: hash.into(),
            simhash: fuzzy::simhash(hash.as_bytes()),
        }
    }
    fn sym(name: &str, kind: SymbolKind, lib: Option<&str>) -> SymbolInfo {
        SymbolInfo { name: name.into(), kind, address: 0, library: lib.map(Into::into) }
    }

    fn base() -> BinaryModel {
        BinaryModel {
            label: "a".into(),
            file_sha256: "aa".into(),
            format: "ELF".into(),
            metadata: vec![MetaEntry::new("arch", "x86_64"), MetaEntry::new("entry", "0x1000")],
            sections: vec![sec(".text", "h1", 0x1000), sec(".data", "d1", 0x2000)],
            symbols: vec![
                sym("api_a", SymbolKind::Exported, None),
                sym("api_b", SymbolKind::Exported, None),
                sym("printf", SymbolKind::Imported, Some("libc")),
            ],
            functions: vec![func("f", "x", 0x1000), func("g", "y", 0x1100)],
            strings: vec!["hello world".into(), "version 1.0.0".into(), "gone string".into()],
            ..BinaryModel::default()
        }
    }

    #[test]
    fn identical_models_are_all_unchanged() {
        let d = diff_binaries(&base(), &base());
        assert_eq!(d.file_state, ChangeState::Unchanged);
        assert_eq!(d.totals().changed(), 0);
        assert!(d.totals().unchanged > 0);
    }

    #[test]
    fn moved_function_is_unchanged_but_edited_one_is_modified() {
        let mut b = base();
        b.label = "b".into();
        b.file_sha256 = "bb".into();
        b.functions = vec![func("f", "x", 0x9000), func("g", "y2", 0x9100)];
        b.sections[0].sha256 = "h2".into();
        b.sections[0].size = 24;
        let d = diff_binaries(&base(), &b);
        assert_eq!(d.file_state, ChangeState::Modified);
        assert_eq!(d.functions[0].state, ChangeState::Unchanged);
        assert_eq!(d.functions[1].state, ChangeState::Modified);
        assert!(d.functions[1].detail.contains("similar"));
        assert_eq!(d.sections[0].state, ChangeState::Modified);
        assert!(d.sections[0].detail.contains("16 \u{2192} 24"));
        assert_eq!(d.sections[1].state, ChangeState::Unchanged);
    }

    #[test]
    fn symbols_added_removed_and_library_change() {
        let mut b = base();
        b.symbols = vec![
            sym("api_a", SymbolKind::Exported, None),
            sym("api_new", SymbolKind::Exported, None),
            sym("printf", SymbolKind::Imported, Some("msvcrt")),
        ];
        let d = diff_binaries(&base(), &b);
        let states: Vec<_> = d.exported.iter().map(|e| (e.key.as_str(), e.state)).collect();
        assert_eq!(
            states,
            [
                ("api_a", ChangeState::Unchanged),
                ("api_b", ChangeState::Removed),
                ("api_new", ChangeState::Added)
            ]
        );
        assert_eq!(d.imported[0].state, ChangeState::Modified);
    }

    #[test]
    fn strings_pair_modified_and_keep_rest() {
        let mut b = base();
        b.strings = vec!["hello world".into(), "version 1.0.1".into(), "brand new text".into()];
        let d = diff_binaries(&base(), &b);
        let find = |k: &str| d.strings.iter().find(|e| e.key == k).unwrap().state;
        assert_eq!(find("hello world"), ChangeState::Unchanged);
        assert_eq!(find("version 1.0.0"), ChangeState::Modified);
        assert_eq!(find("gone string"), ChangeState::Removed);
        assert_eq!(find("brand new text"), ChangeState::Added);
    }

    #[test]
    fn duplicate_section_names_stay_distinct() {
        let mut a = base();
        a.sections.push(sec(".text", "dup", 0x3000));
        let d = diff_binaries(&a, &a);
        assert!(d.sections.iter().any(|e| e.key == ".text#2"));
        assert_eq!(d.totals().changed(), 0);
    }
}
