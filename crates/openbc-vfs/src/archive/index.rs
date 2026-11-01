//! In-memory directory index of one archive, plus entry-name sanitising.
//!
//! Entry names come from untrusted files. They are normalised here, once, so nothing downstream
//! (listing, copy-out through `engine::copy_entry`) can ever see `..`, absolute paths, drive
//! prefixes or empty components. Entries that cannot be made safe are skipped and counted.

use std::collections::{BTreeSet, HashMap};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

#[derive(Clone, Debug)]
pub(crate) struct Node {
    pub dir: bool,
    pub link: bool,
    pub size: u64,
    pub modified: Option<SystemTime>,
    /// Backend handle (meaning is private to the backend). Unused for directories.
    pub id: usize,
}

#[derive(Default)]
pub(crate) struct Index {
    nodes: HashMap<String, Node>,
    children: HashMap<String, BTreeSet<String>>,
    pub skipped: usize,
}

/// `a\b//./c/` -> `a/b/c`. `None` for names that escape the archive root or are unusable.
pub(crate) fn normalize_entry_name(raw: &str) -> Option<String> {
    if raw.contains('\0') {
        return None;
    }
    let mut parts: Vec<&str> = Vec::new();
    for part in raw.split(['/', '\\']) {
        match part {
            "" | "." => {}
            ".." => return None,
            // `C:evil` would make `Path::join` replace the destination on Windows.
            p if cfg!(windows) && p.contains(':') => return None,
            p => parts.push(p),
        }
    }
    (!parts.is_empty()).then(|| parts.join("/"))
}

impl Index {
    pub fn len(&self) -> usize {
        self.nodes.len()
    }

    /// Adds an entry (creating missing parent directories). Later entries replace earlier ones
    /// with the same name, like every extractor does. Returns false when the name was rejected.
    pub fn add(&mut self, raw_name: &str, node: Node) -> bool {
        let Some(name) = normalize_entry_name(raw_name) else {
            self.skipped += 1;
            return false;
        };
        if !node.dir && self.children.get(&name).is_some_and(|c| !c.is_empty()) {
            self.skipped += 1; // a file that would shadow a directory with content
            return false;
        }
        let mut child = name.clone();
        loop {
            let (parent, leaf) = match child.rsplit_once('/') {
                Some((p, l)) => (p.to_owned(), l.to_owned()),
                None => (String::new(), child.clone()),
            };
            self.children.entry(parent.clone()).or_default().insert(leaf);
            if parent.is_empty() {
                break;
            }
            // Synthesize the parent as a directory unless an explicit entry exists.
            match self.nodes.get(&parent) {
                Some(existing) if existing.dir => {}
                _ => {
                    self.nodes.insert(parent.clone(), Node { dir: true, link: false, size: 0, modified: None, id: 0 });
                }
            }
            child = parent;
        }
        if node.dir {
            self.children.entry(name.clone()).or_default();
        }
        self.nodes.insert(name, node);
        true
    }

    pub fn get(&self, key: &str) -> Option<&Node> {
        self.nodes.get(key)
    }

    /// Children of directory `key` as `(leaf name, full key, node)`.
    pub fn list(&self, key: &str) -> Option<Vec<(String, String, &Node)>> {
        let kids = self.children.get(key)?;
        Some(
            kids.iter()
                .filter_map(|leaf| {
                    let full = if key.is_empty() { leaf.clone() } else { format!("{key}/{leaf}") };
                    self.nodes.get(&full).map(|n| (leaf.clone(), full, n))
                })
                .collect(),
        )
    }

    pub fn total_file_bytes(&self) -> u64 {
        self.nodes.values().filter(|n| !n.dir).map(|n| n.size).sum()
    }
}

/// Days since 1970-01-01 for a proleptic Gregorian date (Hinnant's algorithm).
fn days_from_civil(y: i64, m: i64, d: i64) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = y - era * 400;
    let doy = (153 * (if m > 2 { m - 3 } else { m + 9 }) + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe - 719_468
}

/// Archive timestamps without a zone (zip, rar, cab) are read as UTC so that two archives
/// compare consistently with each other.
pub(crate) fn civil_to_system_time(y: i64, mo: i64, d: i64, h: i64, mi: i64, s: i64) -> Option<SystemTime> {
    if !(1..=12).contains(&mo) || !(1..=31).contains(&d) {
        return None;
    }
    let secs = days_from_civil(y, mo, d) * 86_400 + h * 3600 + mi * 60 + s;
    u64::try_from(secs).ok().map(|s| UNIX_EPOCH + Duration::from_secs(s))
}

/// MS-DOS packed `date<<16 | time` (as UnRAR reports it).
#[cfg_attr(not(feature = "archive-rar"), allow(dead_code))]
pub(crate) fn dos_to_system_time(packed: u32) -> Option<SystemTime> {
    let (date, time) = ((packed >> 16) as i64, (packed & 0xffff) as i64);
    civil_to_system_time(1980 + (date >> 9), (date >> 5) & 15, date & 31, time >> 11, (time >> 5) & 63, (time & 31) * 2)
}

pub(crate) fn unix_to_system_time(secs: u64) -> Option<SystemTime> {
    (secs != 0).then(|| UNIX_EPOCH + Duration::from_secs(secs))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hostile_names_are_rejected_or_flattened() {
        assert_eq!(normalize_entry_name("a/b/c.txt").as_deref(), Some("a/b/c.txt"));
        assert_eq!(normalize_entry_name("/abs/x").as_deref(), Some("abs/x"));
        assert_eq!(normalize_entry_name("./a//b\\c/").as_deref(), Some("a/b/c"));
        assert_eq!(normalize_entry_name("../evil"), None);
        assert_eq!(normalize_entry_name("a/../../b"), None);
        assert_eq!(normalize_entry_name("a\\..\\b"), None);
        assert_eq!(normalize_entry_name(""), None);
        assert_eq!(normalize_entry_name("nul\0x"), None);
    }

    #[test]
    fn parents_are_synthesized_and_listing_is_sorted() {
        let mut i = Index::default();
        let file = |size| Node { dir: false, link: false, size, modified: None, id: 1 };
        assert!(i.add("z/y/f.txt", file(3)));
        assert!(i.add("a.txt", file(1)));
        let root: Vec<_> = i.list("").unwrap().into_iter().map(|(n, ..)| n).collect();
        assert_eq!(root, ["a.txt", "z"]);
        assert!(i.get("z/y").unwrap().dir);
        assert!(!i.add("z", file(9)), "file must not shadow a populated directory");
        assert_eq!(i.skipped, 1);
    }

    #[test]
    fn dos_and_civil_dates() {
        // 2020-02-29 12:34:56 UTC
        let t = civil_to_system_time(2020, 2, 29, 12, 34, 56).unwrap();
        assert_eq!(t.duration_since(UNIX_EPOCH).unwrap().as_secs(), 1_582_979_696);
        let packed = ((2020u32 - 1980) << 25) | (2 << 21) | (29 << 16) | (12 << 11) | (34 << 5) | 28;
        assert_eq!(dos_to_system_time(packed), Some(t));
        assert_eq!(civil_to_system_time(2020, 13, 1, 0, 0, 0), None);
    }
}
