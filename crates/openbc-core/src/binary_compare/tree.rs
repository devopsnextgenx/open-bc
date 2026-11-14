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

use super::diff::{BinaryDiff, DiffCounts, Entry};
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
}

impl TreeNode {
    fn leaf(label: String, kind: NodeKind, state: ChangeState, detail: String) -> Self {
        Self { label, kind, state: Some(state), detail, children: Vec::new() }
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
        Self { label: label.to_owned(), kind, state: Some(state), detail, children }
    }

    /// Total node count including `self`.
    #[must_use]
    pub fn size(&self) -> usize {
        1 + self.children.iter().map(Self::size).sum::<usize>()
    }
}

fn leaves<T>(
    entries: &[Entry<T>],
    kind: NodeKind,
    show_unchanged: bool,
    label: impl Fn(&Entry<T>) -> String,
) -> Vec<TreeNode> {
    entries
        .iter()
        .filter(|e| show_unchanged || e.state.is_change())
        .map(|e| TreeNode::leaf(label(e), kind, e.state, e.detail.clone()))
        .collect()
}

fn metadata_label<T>(e: &Entry<T>) -> String {
    e.key.clone()
}

fn meta_leaves(d: &BinaryDiff, show_unchanged: bool) -> Vec<TreeNode> {
    d.metadata
        .iter()
        .filter(|e| show_unchanged || e.state.is_change())
        .map(|e| {
            let value = e.right.as_ref().or(e.left.as_ref()).map(|m| m.value.as_str()).unwrap_or("");
            let detail =
                if e.state == ChangeState::Unchanged { value.to_owned() } else if e.detail.is_empty() { value.to_owned() } else { e.detail.clone() };
            TreeNode::leaf(metadata_label(e), NodeKind::Metadata, e.state, detail)
        })
        .collect()
}

/// Removed/Added aggregates across the three symbol lists.
fn symbols_with_state(d: &BinaryDiff, want: ChangeState) -> Vec<TreeNode> {
    let tag = |kind: &str, e: &Entry<super::SymbolInfo>| {
        TreeNode::leaf(e.key.clone(), NodeKind::Symbol, want, kind.to_owned())
    };
    let mut out = Vec::new();
    for e in d.exported.iter().filter(|e| e.state == want) {
        out.push(tag("exported", e));
    }
    for e in d.imported.iter().filter(|e| e.state == want) {
        out.push(tag("imported", e));
    }
    for e in d.local.iter().filter(|e| e.state == want) {
        out.push(tag("local", e));
    }
    out
}

/// Build the result tree. With `show_unchanged = false` only changed elements are listed (group
/// headers still report the unchanged count).
#[must_use]
pub fn build_tree(d: &BinaryDiff, show_unchanged: bool) -> TreeNode {
    let header = TreeNode::group(
        "Header & Metadata Comparison",
        NodeKind::HeaderGroup,
        meta_leaves(d, show_unchanged),
        DiffCounts::of(&d.metadata),
    );

    let mut sec_children = leaves(&d.sections, NodeKind::Section, show_unchanged, |e| e.key.clone());
    sec_children.extend(leaves(&d.segments, NodeKind::Segment, show_unchanged, |e| format!("segment {}", e.key)));
    let mut sec_counts = DiffCounts::of(&d.sections);
    sec_counts.merge(DiffCounts::of(&d.segments));
    let sections = TreeNode::group("Sections & Segments", NodeKind::SectionsGroup, sec_children, sec_counts);

    let sym_label = |e: &Entry<super::SymbolInfo>| e.key.clone();
    let exported = TreeNode::group(
        "Exported Symbols",
        NodeKind::SubGroup,
        leaves(&d.exported, NodeKind::Symbol, show_unchanged, sym_label),
        DiffCounts::of(&d.exported),
    );
    let imported = TreeNode::group(
        "Imported Symbols",
        NodeKind::SubGroup,
        leaves(&d.imported, NodeKind::Symbol, show_unchanged, sym_label),
        DiffCounts::of(&d.imported),
    );

    let removed_nodes = symbols_with_state(d, ChangeState::Removed);
    let removed_counts = DiffCounts { removed: removed_nodes.len(), ..DiffCounts::default() };
    let removed = TreeNode::group("Removed Symbols", NodeKind::SubGroup, removed_nodes, removed_counts);

    let added_nodes = symbols_with_state(d, ChangeState::Added);
    let added_counts = DiffCounts { added: added_nodes.len(), ..DiffCounts::default() };
    let added = TreeNode::group("Added Symbols", NodeKind::SubGroup, added_nodes, added_counts);

    let local = TreeNode::group(
        "Local / Debug Symbols",
        NodeKind::SubGroup,
        leaves(&d.local, NodeKind::Symbol, show_unchanged, sym_label),
        DiffCounts::of(&d.local),
    );
    let functions = TreeNode::group(
        "Functions",
        NodeKind::SubGroup,
        leaves(&d.functions, NodeKind::Function, show_unchanged, |e| e.key.clone()),
        DiffCounts::of(&d.functions),
    );

    let mut sym_counts = DiffCounts::of(&d.exported);
    sym_counts.merge(DiffCounts::of(&d.imported));
    sym_counts.merge(DiffCounts::of(&d.local));
    sym_counts.merge(DiffCounts::of(&d.functions));
    let symbols = TreeNode::group(
        "Symbol Table / Functions",
        NodeKind::SymbolsGroup,
        vec![exported, imported, removed, added, local, functions],
        sym_counts,
    );

    let strings = TreeNode::group(
        "Strings Table Comparison",
        NodeKind::StringsGroup,
        leaves(&d.strings, NodeKind::Text, show_unchanged, |e| e.key.clone()),
        DiffCounts::of(&d.strings),
    );

    let mut children = vec![header, sections, symbols, strings];
    for note in &d.notes {
        children.push(TreeNode::leaf(note.clone(), NodeKind::Note, ChangeState::Unchanged, String::new()));
    }

    let totals = d.totals();
    let mut root = TreeNode::group(
        &format!("{} vs {}", d.left_label, d.right_label),
        NodeKind::Root,
        children,
        totals,
    );
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
    use super::super::model::{BinaryModel, SymbolInfo, SymbolKind};
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
}
