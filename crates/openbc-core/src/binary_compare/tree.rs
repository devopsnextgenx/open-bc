//! The hierarchical, collapsible result tree built from a [`BinaryDiff`].
//!
//! ```text
//! 📁 app.exe vs app_v2.exe
//!  ├── 📊 Header & Metadata Comparison
//!  ├── 📂 Sections & Segments
//!  ├── 📂 Symbol Table / Functions
//!  │    ├── Exported Symbols      (state of each symbol)
//!  │    ├── Imported Symbols
//!  │    ├── Removed Symbols       (all removed symbols, any kind)
//!  │    ├── Added Symbols         (all added symbols, any kind)
//!  │    ├── Local / Debug Symbols
//!  │    └── Functions
//!  └── 📂 Strings Table Comparison
//! ```
//!
//! The tree is plain data. [`flatten`] linearizes it (pre-order) for the C ABI so the Qt model can
//! stay virtualized without the Rust side exposing pointers into the tree.
//!
//! Every leaf also carries the *per-side values* (`left` / `right`) the split viewer shows next to
//! each other, the raw identity `key` (the mangled name for symbols) and whether the hex /
//! disassembly inspector can open it. [`ViewOptions`] decides which unchanged elements are listed,
//! whether C++ names are demangled and which routine changes the granular filters hide.

use super::demangle::display_name;
use super::diff::{BinaryDiff, DiffCounts, Entry};
use super::filter::ViewOptions;
use super::model::{FunctionInfo, SectionInfo, SymbolInfo};
use super::ChangeState;

/// What a node represents; drives the icon in the viewer.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum NodeKind {
    /// Top node: the file pair.
    Root,
    /// "Header & Metadata Comparison" group.
    HeaderGroup,
    /// "Sections & Segments" group.
    SectionsGroup,
    /// "Symbol Table / Functions" group.
    SymbolsGroup,
    /// A sub-group of symbols or functions (Exported, Imported, Removed, Added, Local, Functions).
    SubGroup,
    /// "Strings Table Comparison" group.
    StringsGroup,
    /// One metadata property.
    Metadata,
    /// A section.
    Section,
    /// A segment.
    Segment,
    /// A symbol.
    Symbol,
    /// A function.
    Function,
    /// A string.
    Text,
    /// A parser note.
    Note,
}

impl NodeKind {
    /// Numeric code for the C ABI, in declaration order.
    #[must_use]
    pub const fn code(self) -> u8 {
        self as u8
    }

    /// Emoji marker for leaf/group kinds that carry no change state.
    #[must_use]
    pub const fn glyph(self) -> &'static str {
        match self {
            Self::Root => "\u{1F4C1}",
            Self::HeaderGroup => "\u{1F4CA}",
            Self::SectionsGroup | Self::SymbolsGroup | Self::StringsGroup | Self::SubGroup => {
                "\u{1F4C2}"
            }
            Self::Note => "\u{2139}\u{FE0F}",
            _ => "",
        }
    }
}

/// One node of the result tree.
#[derive(Clone, Debug, PartialEq)]
pub struct TreeNode {
    /// Text shown for the node.
    pub label: String,
    /// Node role.
    pub kind: NodeKind,
    /// Change state; groups carry the aggregate (`Modified` if anything below changed).
    pub state: Option<ChangeState>,
    /// Secondary text (what changed, hashes, counts).
    pub detail: String,
    /// Child nodes.
    pub children: Vec<TreeNode>,
    /// Identity used for matching (raw linkage name for symbols); empty for groups.
    pub key: String,
    /// Version 1 value shown by the split viewer (empty: absent or a group).
    pub left: String,
    /// Version 2 value shown by the split viewer (empty: absent or a group).
    pub right: String,
    /// `true` when the hex / disassembly inspector can open this node.
    pub inspect: bool,
}

impl TreeNode {
    fn leaf(label: String, kind: NodeKind, state: ChangeState, detail: String) -> Self {
        Self {
            label,
            kind,
            state: Some(state),
            detail,
            children: Vec::new(),
            key: String::new(),
            left: String::new(),
            right: String::new(),
            inspect: false,
        }
    }

    fn sided(mut self, key: String, left: String, right: String, inspect: bool) -> Self {
        self.key = key;
        self.left = left;
        self.right = right;
        self.inspect = inspect;
        self
    }

    fn group(label: &str, kind: NodeKind, children: Vec<TreeNode>, counts: DiffCounts) -> Self {
        let state = if counts.changed() == 0 { ChangeState::Unchanged } else { ChangeState::Modified };
        let detail = if counts.total() == 0 {
            "empty".to_owned()
        } else if counts.changed() == 0 {
            format!("{} unchanged", counts.unchanged)
        } else {
            format!(
                "{} items: {} modified, {} removed, {} added",
                counts.total(),
                counts.modified,
                counts.removed,
                counts.added
            )
        };
        Self {
            label: label.to_owned(),
            kind,
            state: Some(state),
            detail,
            children,
            key: String::new(),
            left: String::new(),
            right: String::new(),
            inspect: false,
        }
    }

    /// Total node count including `self`.
    #[must_use]
    pub fn size(&self) -> usize {
        1 + self.children.iter().map(Self::size).sum::<usize>()
    }
}

// ----------------------------------------------------------------------------- per-side text

fn describe_section(s: &SectionInfo) -> String {
    let mut text = format!("{} bytes @ 0x{:x}", s.size, s.address);
    if !s.flags.is_empty() {
        text.push_str("  ");
        text.push_str(&s.flags);
    }
    text
}

fn describe_symbol(s: &SymbolInfo) -> String {
    match (&s.library, s.address) {
        (Some(lib), 0) => lib.clone(),
        (Some(lib), addr) => format!("0x{addr:x}  {lib}"),
        (None, 0) => String::new(),
        (None, addr) => format!("0x{addr:x}"),
    }
}

fn describe_function(f: &FunctionInfo) -> String {
    format!("0x{:x}  {} bytes{}", f.address, f.size, if f.size_estimated { " (est.)" } else { "" })
}

fn sides<T>(e: &Entry<T>, describe: impl Fn(&T) -> String) -> (String, String) {
    (
        e.left.as_ref().map(&describe).unwrap_or_default(),
        e.right.as_ref().map(&describe).unwrap_or_default(),
    )
}

// ----------------------------------------------------------------------------- filtered view

/// The entries of a diff that survive the active filters.
struct Visible<'a> {
    metadata: Vec<&'a Entry<super::MetaEntry>>,
    sections: Vec<&'a Entry<SectionInfo>>,
    segments: Vec<&'a Entry<SectionInfo>>,
    exported: Vec<&'a Entry<SymbolInfo>>,
    imported: Vec<&'a Entry<SymbolInfo>>,
    local: Vec<&'a Entry<SymbolInfo>>,
    functions: Vec<&'a Entry<FunctionInfo>>,
    strings: Vec<&'a Entry<String>>,
}

/// Drops changed entries for which `hide` is true; unchanged entries always stay.
fn visible<'a, T>(entries: &'a [Entry<T>], hide: impl Fn(&Entry<T>) -> bool) -> Vec<&'a Entry<T>> {
    entries.iter().filter(|e| !(e.state.is_change() && hide(e))).collect()
}

impl<'a> Visible<'a> {
    fn new(d: &'a BinaryDiff, o: &ViewOptions) -> Self {
        Self {
            metadata: visible(&d.metadata, |e| o.hides_metadata(e)),
            sections: visible(&d.sections, |e| o.hides_section(e)),
            segments: visible(&d.segments, |e| o.hides_section(e)),
            exported: visible(&d.exported, |_| false),
            imported: visible(&d.imported, |_| false),
            local: visible(&d.local, |_| o.hides_local_symbols()),
            functions: visible(&d.functions, |e| o.hides_function(e)),
            strings: visible(&d.strings, |_| false),
        }
    }

    fn counts(&self) -> DiffCounts {
        let mut c = DiffCounts::default();
        c.merge(DiffCounts::of_refs(&self.metadata));
        c.merge(DiffCounts::of_refs(&self.sections));
        c.merge(DiffCounts::of_refs(&self.segments));
        c.merge(DiffCounts::of_refs(&self.exported));
        c.merge(DiffCounts::of_refs(&self.imported));
        c.merge(DiffCounts::of_refs(&self.local));
        c.merge(DiffCounts::of_refs(&self.functions));
        c.merge(DiffCounts::of_refs(&self.strings));
        c
    }
}

/// Totals over the elements that the filters in `o` leave visible (hidden changes are excluded).
#[must_use]
pub fn visible_counts(d: &BinaryDiff, o: &ViewOptions) -> DiffCounts {
    Visible::new(d, o).counts()
}

// ----------------------------------------------------------------------------- leaves

fn meta_leaves(v: &Visible<'_>, show_unchanged: bool) -> Vec<TreeNode> {
    v.metadata
        .iter()
        .copied()
        .filter(|e| show_unchanged || e.state.is_change())
        .map(|e| {
            let value = e.right.as_ref().or(e.left.as_ref()).map(|m| m.value.as_str()).unwrap_or("");
            let detail = if e.state == ChangeState::Unchanged || e.detail.is_empty() {
                value.to_owned()
            } else {
                e.detail.clone()
            };
            let (l, r) = sides(e, |m| m.value.clone());
            TreeNode::leaf(e.key.clone(), NodeKind::Metadata, e.state, detail).sided(e.key.clone(), l, r, false)
        })
        .collect()
}

fn section_leaves(
    entries: &[&Entry<SectionInfo>],
    kind: NodeKind,
    show_unchanged: bool,
    label: impl Fn(&Entry<SectionInfo>) -> String,
) -> Vec<TreeNode> {
    entries
        .iter()
        .copied()
        .filter(|e| show_unchanged || e.state.is_change())
        .map(|e| {
            let (l, r) = sides(e, describe_section);
            let inspect = kind == NodeKind::Section
                && (e.left.as_ref().is_some_and(|s| s.file_offset.is_some())
                    || e.right.as_ref().is_some_and(|s| s.file_offset.is_some()));
            TreeNode::leaf(label(e), kind, e.state, e.detail.clone()).sided(e.key.clone(), l, r, inspect)
        })
        .collect()
}

fn symbol_leaves(entries: &[&Entry<SymbolInfo>], show_unchanged: bool, demangle: bool) -> Vec<TreeNode> {
    entries
        .iter()
        .copied()
        .filter(|e| show_unchanged || e.state.is_change())
        .map(|e| {
            let raw = e.right.as_ref().or(e.left.as_ref()).map_or(e.key.as_str(), |s| s.name.as_str());
            let (l, r) = sides(e, describe_symbol);
            TreeNode::leaf(display_name(&e.key, raw, demangle), NodeKind::Symbol, e.state, e.detail.clone())
                .sided(e.key.clone(), l, r, false)
        })
        .collect()
}

/// Removed / added aggregates across the three symbol lists.
fn symbols_with_state(v: &Visible<'_>, want: ChangeState, demangle: bool) -> Vec<TreeNode> {
    let mut out = Vec::new();
    for (tag, list) in [("exported", &v.exported), ("imported", &v.imported), ("local", &v.local)] {
        for e in list.iter().copied().filter(|e| e.state == want) {
            let raw = e.right.as_ref().or(e.left.as_ref()).map_or(e.key.as_str(), |s| s.name.as_str());
            let (l, r) = sides(e, describe_symbol);
            out.push(
                TreeNode::leaf(display_name(&e.key, raw, demangle), NodeKind::Symbol, want, tag.to_owned())
                    .sided(e.key.clone(), l, r, false),
            );
        }
    }
    out
}

fn function_leaves(entries: &[&Entry<FunctionInfo>], show_unchanged: bool, demangle: bool) -> Vec<TreeNode> {
    entries
        .iter()
        .copied()
        .filter(|e| show_unchanged || e.state.is_change())
        .map(|e| {
            let raw = e.right.as_ref().or(e.left.as_ref()).map_or(e.key.as_str(), |f| f.name.as_str());
            let (l, r) = sides(e, describe_function);
            let inspect = e.left.as_ref().is_some_and(|f| f.file_offset.is_some())
                || e.right.as_ref().is_some_and(|f| f.file_offset.is_some());
            TreeNode::leaf(display_name(&e.key, raw, demangle), NodeKind::Function, e.state, e.detail.clone())
                .sided(e.key.clone(), l, r, inspect)
        })
        .collect()
}

fn string_leaves(entries: &[&Entry<String>], show_unchanged: bool) -> Vec<TreeNode> {
    entries
        .iter()
        .copied()
        .filter(|e| show_unchanged || e.state.is_change())
        .map(|e| {
            let (l, r) = sides(e, |s: &String| s.clone());
            TreeNode::leaf(e.key.clone(), NodeKind::Text, e.state, e.detail.clone()).sided(e.key.clone(), l, r, false)
        })
        .collect()
}

/// Build the result tree with the default view (demangling off, no filters). With
/// `show_unchanged = false` only changed elements are listed (group headers still report the
/// unchanged count).
#[must_use]
pub fn build_tree(d: &BinaryDiff, show_unchanged: bool) -> TreeNode {
    build_tree_with(d, &ViewOptions { show_unchanged, demangle: false, hide: 0 })
}

/// Build the result tree for the given presentation options.
#[must_use]
pub fn build_tree_with(d: &BinaryDiff, o: &ViewOptions) -> TreeNode {
    let v = Visible::new(d, o);
    let show = o.show_unchanged;

    let header = TreeNode::group(
        "Header & Metadata Comparison",
        NodeKind::HeaderGroup,
        meta_leaves(&v, show),
        DiffCounts::of_refs(&v.metadata),
    );

    let mut sec_children = section_leaves(&v.sections, NodeKind::Section, show, |e| e.key.clone());
    sec_children.extend(section_leaves(&v.segments, NodeKind::Segment, show, |e| format!("segment {}", e.key)));
    let mut sec_counts = DiffCounts::of_refs(&v.sections);
    sec_counts.merge(DiffCounts::of_refs(&v.segments));
    let sections = TreeNode::group("Sections & Segments", NodeKind::SectionsGroup, sec_children, sec_counts);

    let exported = TreeNode::group(
        "Exported Symbols",
        NodeKind::SubGroup,
        symbol_leaves(&v.exported, show, o.demangle),
        DiffCounts::of_refs(&v.exported),
    );
    let imported = TreeNode::group(
        "Imported Symbols",
        NodeKind::SubGroup,
        symbol_leaves(&v.imported, show, o.demangle),
        DiffCounts::of_refs(&v.imported),
    );

    let removed_nodes = symbols_with_state(&v, ChangeState::Removed, o.demangle);
    let removed_counts = DiffCounts { removed: removed_nodes.len(), ..DiffCounts::default() };
    let removed = TreeNode::group("Removed Symbols", NodeKind::SubGroup, removed_nodes, removed_counts);

    let added_nodes = symbols_with_state(&v, ChangeState::Added, o.demangle);
    let added_counts = DiffCounts { added: added_nodes.len(), ..DiffCounts::default() };
    let added = TreeNode::group("Added Symbols", NodeKind::SubGroup, added_nodes, added_counts);

    let local = TreeNode::group(
        "Local / Debug Symbols",
        NodeKind::SubGroup,
        symbol_leaves(&v.local, show, o.demangle),
        DiffCounts::of_refs(&v.local),
    );
    let functions = TreeNode::group(
        "Functions",
        NodeKind::SubGroup,
        function_leaves(&v.functions, show, o.demangle),
        DiffCounts::of_refs(&v.functions),
    );

    let mut sym_counts = DiffCounts::of_refs(&v.exported);
    sym_counts.merge(DiffCounts::of_refs(&v.imported));
    sym_counts.merge(DiffCounts::of_refs(&v.local));
    sym_counts.merge(DiffCounts::of_refs(&v.functions));
    let symbols = TreeNode::group(
        "Symbol Table / Functions",
        NodeKind::SymbolsGroup,
        vec![exported, imported, removed, added, local, functions],
        sym_counts,
    );

    let strings = TreeNode::group(
        "Strings Table Comparison",
        NodeKind::StringsGroup,
        string_leaves(&v.strings, show),
        DiffCounts::of_refs(&v.strings),
    );

    let mut children = vec![header, sections, symbols, strings];
    for note in &d.notes {
        children.push(TreeNode::leaf(note.clone(), NodeKind::Note, ChangeState::Unchanged, String::new()));
    }

    let mut root = TreeNode::group(&format!("{} vs {}", d.left_label, d.right_label), NodeKind::Root, children, v.counts());
    // The file hash decides the root even if every parsed element matched (e.g. overlay bytes).
    if d.file_state == ChangeState::Modified {
        root.state = Some(ChangeState::Modified);
        root.detail = format!(
            "sha256 {} \u{2192} {}; {}",
            &d.file_hashes.0[..d.file_hashes.0.len().min(12)],
            &d.file_hashes.1[..d.file_hashes.1.len().min(12)],
            root.detail
        );
    }
    root
}

/// One row of the pre-order linearization.
#[derive(Clone, Debug, PartialEq)]
pub struct FlatRow {
    /// Nesting depth, root = 0.
    pub depth: u32,
    /// [`NodeKind::code`].
    pub kind: u8,
    /// [`ChangeState::code`], or `255` for none.
    pub state: u8,
    /// Node text.
    pub label: String,
    /// Secondary text.
    pub detail: String,
    /// Whether the node has children (the next row at `depth + 1` is the first one).
    pub has_children: bool,
    /// Identity (raw linkage name for symbols and functions); empty for groups.
    pub key: String,
    /// Version 1 value for the split viewer.
    pub left: String,
    /// Version 2 value for the split viewer.
    pub right: String,
    /// `true` when the hex / disassembly inspector can open the row.
    pub inspect: bool,
}

/// Linearize the tree depth-first.
#[must_use]
pub fn flatten(root: &TreeNode) -> Vec<FlatRow> {
    fn walk(n: &TreeNode, depth: u32, out: &mut Vec<FlatRow>) {
        out.push(FlatRow {
            depth,
            kind: n.kind.code(),
            state: n.state.map_or(255, ChangeState::code),
            label: n.label.clone(),
            detail: n.detail.clone(),
            has_children: !n.children.is_empty(),
            key: n.key.clone(),
            left: n.left.clone(),
            right: n.right.clone(),
            inspect: n.inspect,
        });
        for c in &n.children {
            walk(c, depth + 1, out);
        }
    }
    let mut out = Vec::with_capacity(root.size());
    walk(root, 0, &mut out);
    out
}

#[cfg(test)]
mod tests {
    use super::super::diff::diff_binaries;
    use super::super::model::{BinaryModel, SectionInfo, SectionKind, SymbolInfo, SymbolKind};
    use super::*;

    fn sym(name: &str, kind: SymbolKind) -> SymbolInfo {
        SymbolInfo { name: name.into(), kind, address: 0, library: None }
    }

    fn pair() -> BinaryDiff {
        let a = BinaryModel {
            label: "v1".into(),
            file_sha256: "aaaaaaaaaaaaaaaa".into(),
            symbols: vec![sym("keep", SymbolKind::Exported), sym("old", SymbolKind::Exported)],
            strings: vec!["alpha".into()],
            ..BinaryModel::default()
        };
        let b = BinaryModel {
            label: "v2".into(),
            file_sha256: "bbbbbbbbbbbbbbbb".into(),
            symbols: vec![sym("keep", SymbolKind::Exported), sym("new", SymbolKind::Imported)],
            strings: vec!["alpha".into()],
            ..BinaryModel::default()
        };
        diff_binaries(&a, &b)
    }

    #[test]
    fn has_the_four_top_groups_and_symbol_subgroups() {
        let t = build_tree(&pair(), true);
        let top: Vec<_> = t.children.iter().map(|c| c.label.as_str()).collect();
        assert_eq!(
            top,
            ["Header & Metadata Comparison", "Sections & Segments", "Symbol Table / Functions", "Strings Table Comparison"]
        );
        let subs: Vec<_> = t.children[2].children.iter().map(|c| c.label.as_str()).collect();
        assert_eq!(
            subs,
            ["Exported Symbols", "Imported Symbols", "Removed Symbols", "Added Symbols", "Local / Debug Symbols", "Functions"]
        );
        assert_eq!(t.state, Some(ChangeState::Modified));
    }

    #[test]
    fn removed_and_added_groups_collect_across_kinds() {
        let t = build_tree(&pair(), true);
        let sym = &t.children[2];
        assert_eq!(sym.children[2].children.len(), 1);
        assert_eq!(sym.children[2].children[0].label, "old");
        assert_eq!(sym.children[3].children[0].label, "new");
        assert_eq!(sym.children[3].children[0].detail, "imported");
    }

    #[test]
    fn hiding_unchanged_drops_leaves_only() {
        let full = build_tree(&pair(), true).size();
        let lean = build_tree(&pair(), false).size();
        assert!(lean < full);
        let t = build_tree(&pair(), false);
        assert!(t.children[3].children.is_empty());
        assert_eq!(t.children[3].state, Some(ChangeState::Unchanged));
    }

    #[test]
    fn flatten_is_preorder_with_depths() {
        let rows = flatten(&build_tree(&pair(), true));
        assert_eq!(rows[0].depth, 0);
        assert!(rows[0].has_children);
        assert!(rows.iter().skip(1).all(|r| r.depth >= 1));
        assert_eq!(rows.len(), build_tree(&pair(), true).size());
    }

    fn section(name: &str, kind: SectionKind, sha: &str, offset: Option<u64>) -> SectionInfo {
        SectionInfo {
            name: name.into(),
            kind,
            address: 0x1000,
            size: 4,
            flags: "ALLOC".into(),
            sha256: sha.into(),
            sha256_unpadded: sha.into(),
            file_offset: offset,
        }
    }

    fn filter_pair() -> BinaryDiff {
        let a = BinaryModel {
            label: "v1".into(),
            file_sha256: "aaaaaaaaaaaaaaaa".into(),
            sections: vec![
                section(".text", SectionKind::Section, "t1", Some(0x400)),
                section(".symtab", SectionKind::Section, "s1", None),
                section(".strtab", SectionKind::Section, "r1", None),
            ],
            symbols: vec![sym("_Z3addii", SymbolKind::Exported)],
            ..BinaryModel::default()
        };
        let b = BinaryModel {
            label: "v2".into(),
            file_sha256: "bbbbbbbbbbbbbbbb".into(),
            sections: vec![
                section(".text", SectionKind::Section, "t2", Some(0x400)),
                section(".symtab", SectionKind::Section, "s2", None),
                section(".strtab", SectionKind::Section, "r2", None),
            ],
            symbols: vec![sym("_Z3addii", SymbolKind::Exported)],
            ..BinaryModel::default()
        };
        diff_binaries(&a, &b)
    }

    fn labels(node: &TreeNode, out: &mut Vec<String>) {
        out.push(node.label.clone());
        for c in &node.children {
            labels(c, out);
        }
    }

    #[test]
    fn symtab_strtab_filter_hides_routine_shifts_and_adjusts_totals() {
        let d = filter_pair();
        let plain = ViewOptions { show_unchanged: false, demangle: false, hide: 0 };
        let filtered = ViewOptions { hide: super::super::filter::HIDE_SYMTAB_STRTAB, ..plain };
        let mut all = Vec::new();
        labels(&build_tree_with(&d, &plain), &mut all);
        assert!(all.iter().any(|l| l == ".symtab") && all.iter().any(|l| l == ".strtab"));
        let mut some = Vec::new();
        labels(&build_tree_with(&d, &filtered), &mut some);
        assert!(!some.iter().any(|l| l == ".symtab" || l == ".strtab"));
        assert!(some.iter().any(|l| l == ".text"), "functional code stays visible");
        assert_eq!(visible_counts(&d, &plain).modified, 3);
        assert_eq!(visible_counts(&d, &filtered).modified, 1);
    }

    #[test]
    fn demangling_changes_the_label_but_not_the_identity() {
        let d = filter_pair();
        let on = flatten(&build_tree_with(&d, &ViewOptions { demangle: true, ..ViewOptions::default() }));
        let off = flatten(&build_tree_with(&d, &ViewOptions { demangle: false, ..ViewOptions::default() }));
        let row_on = on.iter().find(|r| r.key == "_Z3addii").expect("symbol row");
        let row_off = off.iter().find(|r| r.key == "_Z3addii").expect("symbol row");
        assert_eq!(row_on.label, "add(int, int)");
        assert_eq!(row_off.label, "_Z3addii");
        assert_eq!(on.len(), off.len(), "demangling never changes which rows exist");
    }

    #[test]
    fn only_sections_with_a_file_offset_are_inspectable() {
        let rows = flatten(&build_tree_with(&filter_pair(), &ViewOptions::default()));
        let by = |label: &str| rows.iter().find(|r| r.label == label).expect("row");
        assert!(by(".text").inspect);
        assert!(!by(".symtab").inspect);
    }
}
