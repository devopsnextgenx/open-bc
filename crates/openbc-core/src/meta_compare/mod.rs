//! Generic property-table comparison used by media compare.

use std::collections::HashMap;

/// One metadata property (`group` is e.g. "File", "Container", "Audio", "Video", "Tags").
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MetaRow {
    /// Display group the property belongs to.
    pub group: String,
    /// Property name within the group.
    pub key: String,
    /// Property value as text.
    pub value: String,
}

/// One aligned row of the left/right property table.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MetaDiffRow {
    /// Display group of the property.
    pub group: String,
    /// Property name within the group.
    pub key: String,
    /// Value on the left side, when present.
    pub left: Option<String>,
    /// Value on the right side, when present.
    pub right: Option<String>,
    /// Values differ or the property exists on one side only.
    pub differs: bool,
    /// Always-different housekeeping rows (file name, size, date); excluded from counts.
    pub informational: bool,
}

/// Groups in display order; unknown groups sort after these.
const GROUP_ORDER: [&str; 6] = ["File", "Container", "Video", "Audio", "Subtitle", "Tags"];

/// Group whose rows are housekeeping (always differ) rather than real differences.
const INFORMATIONAL_GROUP: &str = "File";

fn group_rank(group: &str) -> usize {
    GROUP_ORDER.iter().position(|g| *g == group).unwrap_or(GROUP_ORDER.len())
}

/// A `(group, key)` property with the first value seen on each side.
struct Slot<'a> {
    group: &'a str,
    key: &'a str,
    /// `[left, right]`.
    values: [Option<&'a str>; 2],
}

/// Aligns two property lists by `(group, key)`; groups sorted in display order, keys in
/// first-seen order (left first). A repeated `(group, key)` on one side keeps its first value.
pub fn diff_meta(left: &[MetaRow], right: &[MetaRow]) -> Vec<MetaDiffRow> {
    let mut index: HashMap<(&str, &str), usize> = HashMap::new();
    let mut slots: Vec<Slot<'_>> = Vec::new();
    for (side, rows) in [left, right].into_iter().enumerate() {
        for row in rows {
            let slot = *index.entry((row.group.as_str(), row.key.as_str())).or_insert_with(|| {
                slots.push(Slot { group: &row.group, key: &row.key, values: [None, None] });
                slots.len() - 1
            });
            slots[slot].values[side].get_or_insert(row.value.as_str());
        }
    }
    let mut out: Vec<MetaDiffRow> = slots
        .into_iter()
        .map(|slot| {
            let [l, r] = slot.values;
            MetaDiffRow {
                group: slot.group.to_owned(),
                key: slot.key.to_owned(),
                left: l.map(str::to_owned),
                right: r.map(str::to_owned),
                differs: l != r,
                informational: slot.group == INFORMATIONAL_GROUP,
            }
        })
        .collect();
    out.sort_by_key(|row| group_rank(&row.group)); // stable: keeps first-seen key order
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn row(group: &str, key: &str, value: &str) -> MetaRow {
        MetaRow { group: group.into(), key: key.into(), value: value.into() }
    }

    #[test]
    fn aligns_rows_and_orders_groups() {
        let left = [row("Tags", "title", "x"), row("File", "name", "a")];
        let right = [row("File", "name", "b"), row("Tags", "title", "x"), row("Tags", "year", "1999")];
        let out = diff_meta(&left, &right);
        let ids: Vec<_> = out.iter().map(|r| (r.group.as_str(), r.key.as_str())).collect();
        assert_eq!(ids, [("File", "name"), ("Tags", "title"), ("Tags", "year")]);
        assert!(out[0].informational && out[0].differs);
        assert!(!out[1].differs);
        assert_eq!((out[2].left.as_deref(), out[2].right.as_deref()), (None, Some("1999")));
        assert!(out[2].differs);
    }

    #[test]
    fn first_value_per_side_wins() {
        let out = diff_meta(&[row("Audio", "codec", "aac"), row("Audio", "codec", "mp3")], &[]);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].left.as_deref(), Some("aac"));
    }
}
