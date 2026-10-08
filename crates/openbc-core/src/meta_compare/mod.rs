//! Generic property-table comparison used by media compare.

use std::collections::HashMap;

/// One metadata property (`group` is e.g. "File", "Container", "Audio", "Video", "Tags").
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MetaRow {
    pub group: String,
    pub key: String,
    pub value: String,
}

/// One aligned row of the left/right property table.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MetaDiffRow {
    pub group: String,
    pub key: String,
    pub left: Option<String>,
    pub right: Option<String>,
    /// Values differ or the property exists on one side only.
    pub differs: bool,
    /// Always-different housekeeping rows (file name, size, date); excluded from counts.
    pub informational: bool,
}

const GROUP_ORDER: [&str; 6] = ["File", "Container", "Video", "Audio", "Subtitle", "Tags"];

fn group_rank(group: &str) -> usize {
    GROUP_ORDER.iter().position(|g| *g == group).unwrap_or(GROUP_ORDER.len())
}

/// Aligns two property lists by `(group, key)`; groups sorted in display order, keys in
/// first-seen order (left first).
pub fn diff_meta(left: &[MetaRow], right: &[MetaRow]) -> Vec<MetaDiffRow> {
    let mut order: Vec<(String, String)> = Vec::new();
    let mut seen: HashMap<(String, String), usize> = HashMap::new();
    let mut lv: HashMap<usize, &str> = HashMap::new();
    let mut rv: HashMap<usize, &str> = HashMap::new();
    for (rows, values) in [(left, &mut lv), (right, &mut rv)] {
        for row in rows {
            let id = (row.group.clone(), row.key.clone());
            let slot = *seen.entry(id.clone()).or_insert_with(|| {
                order.push(id);
                order.len() - 1
            });
            values.entry(slot).or_insert(row.value.as_str());
        }
    }
    let mut out: Vec<MetaDiffRow> = order
        .into_iter()
        .enumerate()
        .map(|(slot, (group, key))| {
            let (l, r) = (lv.get(&slot).map(|s| s.to_string()), rv.get(&slot).map(|s| s.to_string()));
            MetaDiffRow { informational: group == "File", differs: l != r, group, key, left: l, right: r }
        })
        .collect();
    out.sort_by_key(|row| group_rank(&row.group)); // stable: keeps first-seen key order
    out
}
