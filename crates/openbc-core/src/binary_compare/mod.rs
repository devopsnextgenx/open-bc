//! Binary comparison: format-neutral model, version diff and the hierarchical result tree.
//!
//! # Layering
//!
//! * [`model`]  - the format-neutral description of one binary ([`BinaryModel`]). Producing it
//!   from a PE / ELF / Mach-O file is the job of `openbc-compute::binary` (LIEF); this crate never
//!   parses an executable.
//! * [`diff`]   - [`diff_binaries`] compares two models and classifies every element as
//!   [`ChangeState::Unchanged`], `Modified`, `Removed` or `Added`.
//! * [`tree`]   - [`build_tree`] turns the diff into the collapsible tree shown by the viewer, and
//!   [`flatten`] linearizes it for the C ABI.
//! * [`fuzzy`]  - the 64-bit SimHash used to report *how much* a function changed.
//! * [`strings`] - format-independent ASCII / UTF-16LE string extraction.
//!
//! Everything is pure and read-only: nothing here writes or patches a binary.

pub mod diff;
pub mod fuzzy;
pub mod model;
pub mod strings;
pub mod tree;

pub use diff::{diff_binaries, BinaryDiff, DiffCounts, Entry};
pub use model::{
    BinaryModel, FunctionInfo, MetaEntry, SectionInfo, SectionKind, SymbolInfo, SymbolKind,
};
pub use tree::{build_tree, flatten, FlatRow, NodeKind, TreeNode};

/// Classification of one element between version 1 (baseline) and version 2 (target).
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq, PartialOrd, Ord)]
pub enum ChangeState {
    /// Present in both versions with matching hash / data.
    Unchanged,
    /// Present in both versions with a different hash or content.
    Modified,
    /// Present in version 1 only.
    Removed,
    /// Present in version 2 only.
    Added,
}

impl ChangeState {
    /// Marker shown in the tree: green circle, orange diamond, cross, check.
    #[must_use]
    pub const fn glyph(self) -> &'static str {
        match self {
            Self::Unchanged => "\u{1F7E2}",
            Self::Modified => "\u{1F538}",
            Self::Removed => "\u{274C}",
            Self::Added => "\u{2705}",
        }
    }

    /// Stable lower-case id (used by the C ABI and tests).
    #[must_use]
    pub const fn id(self) -> &'static str {
        match self {
            Self::Unchanged => "unchanged",
            Self::Modified => "modified",
            Self::Removed => "removed",
            Self::Added => "added",
        }
    }

    /// Numeric code for the C ABI: 0 unchanged, 1 modified, 2 removed, 3 added.
    #[must_use]
    pub const fn code(self) -> u8 {
        match self {
            Self::Unchanged => 0,
            Self::Modified => 1,
            Self::Removed => 2,
            Self::Added => 3,
        }
    }

    /// `true` for everything except [`ChangeState::Unchanged`].
    #[must_use]
    pub const fn is_change(self) -> bool {
        !matches!(self, Self::Unchanged)
    }
}
