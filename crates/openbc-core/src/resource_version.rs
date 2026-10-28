//! Version / change-assessment types for loaded resources (pure: no I/O, no threads).
//!
//! A [`ResourceVersion`] is whatever cheap-but-strong identity a provider could give for a
//! file or one directory level at one moment. Watcher events and timestamps are only *hints*
//! to look again; [`assess`] decides whether the thing a comparison loaded is really outdated.

/// Whether the version describes a regular file or one directory level.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ResourceKind {
    File,
    /// One directory level; `content_hash` is a hash of the sorted (name, kind, size, mtime) rows
    /// and `size` the number of entries.
    Directory,
}

/// Identity of a resource at one moment.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ResourceVersion {
    pub kind: ResourceKind,
    /// Byte length (files) or entry count (directories).
    pub size: u64,
    /// Modification time as nanoseconds since the Unix epoch, when the provider reports one.
    pub modified_ns: Option<u128>,
    /// Device/inode style identity (local Unix). Changes on atomic "write temp + rename" saves.
    pub file_id: Option<u64>,
    /// Content hash. Always present for directories; for files only when it was computed
    /// (small files, or on demand to confirm a suspected change).
    pub content_hash: Option<u64>,
}

impl ResourceVersion {
    /// Equal by metadata alone (never reads content).
    #[must_use]
    pub fn same_stat(&self, other: &Self) -> bool {
        self.kind == other.kind
            && self.size == other.size
            && self.modified_ns == other.modified_ns
            && match (self.file_id, other.file_id) {
                (Some(a), Some(b)) => a == b,
                _ => true,
            }
            && match (self.kind, self.content_hash, other.content_hash) {
                (ResourceKind::Directory, Some(a), Some(b)) => a == b,
                _ => true,
            }
    }

    /// Equal by content: hashes decide when both sides have one (a bare `touch` is not a change).
    #[must_use]
    pub fn same_content(&self, other: &Self) -> bool {
        if self.kind != other.kind || self.size != other.size {
            return false;
        }
        match (self.content_hash, other.content_hash) {
            (Some(a), Some(b)) => a == b,
            _ => self.same_stat(other),
        }
    }
}

/// What one probe of a resource returned.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Observed {
    Present(ResourceVersion),
    /// Not found (deleted, renamed away, or mid atomic replace).
    Missing,
    /// Could not be checked right now (permissions, network down). NOT evidence of a change.
    Unavailable(String),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChangeKind {
    /// Same file, new contents (or a directory listing changed).
    Modified,
    /// A different file now sits at the path (atomic save, rename over).
    Replaced,
    Deleted,
    /// The path did not exist when loaded and does now.
    Created,
}

impl ChangeKind {
    #[must_use]
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Modified => "modified",
            Self::Replaced => "replaced",
            Self::Deleted => "deleted",
            Self::Created => "created",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Assessment {
    Unchanged,
    /// Metadata differs but size is equal and a loaded hash exists: confirm by hashing before
    /// calling it a change (catches `touch` and mtime-only rewrites).
    NeedsContentCheck,
    Changed(ChangeKind),
    /// The resource could not be checked; leave staleness as it is.
    Unknown,
}

/// Compares the version a comparison loaded (`None` = it was absent) with the latest observation.
#[must_use]
pub fn assess(loaded: Option<&ResourceVersion>, current: &Observed) -> Assessment {
    match (loaded, current) {
        (_, Observed::Unavailable(_)) => Assessment::Unknown,
        (None, Observed::Missing) => Assessment::Unchanged,
        (None, Observed::Present(_)) => Assessment::Changed(ChangeKind::Created),
        (Some(_), Observed::Missing) => Assessment::Changed(ChangeKind::Deleted),
        (Some(l), Observed::Present(c)) => {
            if l.same_stat(c) {
                Assessment::Unchanged
            } else if l.kind == ResourceKind::File && l.size == c.size && l.content_hash.is_some() {
                Assessment::NeedsContentCheck
            } else if matches!((l.file_id, c.file_id), (Some(a), Some(b)) if a != b) {
                Assessment::Changed(ChangeKind::Replaced)
            } else {
                Assessment::Changed(ChangeKind::Modified)
            }
        }
    }
}

/// 64-bit FNV-1a, streaming. Not cryptographic: it only has to tell "same bytes" from
/// "different bytes" for change confirmation, with no extra dependency.
#[derive(Clone, Copy, Debug)]
pub struct Fnv64(u64);

impl Default for Fnv64 {
    fn default() -> Self {
        Self(0xcbf2_9ce4_8422_2325)
    }
}

impl Fnv64 {
    pub fn update(&mut self, bytes: &[u8]) {
        for b in bytes {
            self.0 ^= u64::from(*b);
            self.0 = self.0.wrapping_mul(0x0100_0000_01b3);
        }
    }

    #[must_use]
    pub fn finish(self) -> u64 {
        self.0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn file(size: u64, mtime: u128, id: u64, hash: Option<u64>) -> ResourceVersion {
        ResourceVersion { kind: ResourceKind::File, size, modified_ns: Some(mtime), file_id: Some(id), content_hash: hash }
    }

    #[test]
    fn identical_versions_are_unchanged() {
        let v = file(10, 5, 1, Some(7));
        assert_eq!(assess(Some(&v), &Observed::Present(v.clone())), Assessment::Unchanged);
    }

    #[test]
    fn size_change_is_modified() {
        assert_eq!(
            assess(Some(&file(10, 5, 1, None)), &Observed::Present(file(11, 6, 1, None))),
            Assessment::Changed(ChangeKind::Modified)
        );
    }

    #[test]
    fn new_inode_with_different_size_is_replaced() {
        assert_eq!(
            assess(Some(&file(10, 5, 1, None)), &Observed::Present(file(12, 6, 2, None))),
            Assessment::Changed(ChangeKind::Replaced)
        );
    }

    #[test]
    fn mtime_only_change_with_loaded_hash_needs_content_check_and_touch_is_not_a_change() {
        let loaded = file(10, 5, 1, Some(42));
        let touched = file(10, 99, 1, None);
        assert_eq!(assess(Some(&loaded), &Observed::Present(touched)), Assessment::NeedsContentCheck);
        let confirmed_same = file(10, 99, 1, Some(42));
        assert!(loaded.same_content(&confirmed_same));
        let confirmed_diff = file(10, 99, 1, Some(43));
        assert!(!loaded.same_content(&confirmed_diff));
    }

    #[test]
    fn mtime_only_change_without_loaded_hash_is_modified() {
        assert_eq!(
            assess(Some(&file(10, 5, 1, None)), &Observed::Present(file(10, 6, 1, None))),
            Assessment::Changed(ChangeKind::Modified)
        );
    }

    #[test]
    fn missing_and_created() {
        assert_eq!(assess(Some(&file(1, 1, 1, None)), &Observed::Missing), Assessment::Changed(ChangeKind::Deleted));
        assert_eq!(assess(None, &Observed::Present(file(1, 1, 1, None))), Assessment::Changed(ChangeKind::Created));
        assert_eq!(assess(None, &Observed::Missing), Assessment::Unchanged);
    }

    #[test]
    fn unavailable_is_never_a_change() {
        assert_eq!(assess(Some(&file(1, 1, 1, None)), &Observed::Unavailable("denied".into())), Assessment::Unknown);
    }

    #[test]
    fn directory_listing_hash_decides() {
        let dir = |n, h| ResourceVersion { kind: ResourceKind::Directory, size: n, modified_ns: None, file_id: None, content_hash: Some(h) };
        assert!(dir(3, 1).same_stat(&dir(3, 1)));
        assert_eq!(assess(Some(&dir(3, 1)), &Observed::Present(dir(3, 2))), Assessment::Changed(ChangeKind::Modified));
    }

    #[test]
    fn fnv_distinguishes_and_is_streamable() {
        let mut a = Fnv64::default();
        a.update(b"hello ");
        a.update(b"world");
        let mut b = Fnv64::default();
        b.update(b"hello world");
        assert_eq!(a.finish(), b.finish());
        let mut c = Fnv64::default();
        c.update(b"hello w0rld");
        assert_ne!(b.finish(), c.finish());
    }
}
