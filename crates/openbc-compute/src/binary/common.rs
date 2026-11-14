//! Format-independent helpers shared by the ELF / PE / Mach-O extractors.

use std::collections::HashSet;

use openbc_core::binary_compare::fuzzy::simhash;
use openbc_core::binary_compare::strings::{extract_strings, MAX_STRINGS};
use openbc_core::binary_compare::{
    BinaryModel, FunctionInfo, MetaEntry, SectionInfo, SectionKind,
};

use super::{ParseOptions, CONTENT_HASH};
use crate::digest::hex_digest;

/// Raw bytes of one loadable region plus where it lives.
pub(super) struct Blob<'a> {
    pub name: String,
    pub address: u64,
    pub executable: bool,
    pub data: &'a [u8],
}

/// A function the format told us about.
pub(super) struct Candidate {
    pub name: String,
    pub address: u64,
    /// `0` when the format records no size.
    pub size: u64,
}

/// Build a [`SectionInfo`] from raw content.
pub(super) fn section_info(
    name: String,
    kind: SectionKind,
    address: u64,
    declared_size: u64,
    flags: String,
    content: &[u8],
) -> SectionInfo {
    SectionInfo {
        name,
        kind,
        address,
        size: if content.is_empty() { declared_size } else { content.len() as u64 },
        flags,
        sha256: hex_digest(CONTENT_HASH, content),
    }
}

pub(super) fn meta(model: &mut BinaryModel, key: &str, value: impl Into<String>) {
    let value = value.into();
    if !value.is_empty() {
        model.metadata.push(MetaEntry::new(key, value));
    }
}

pub(super) fn hex(v: u64) -> String {
    format!("0x{v:x}")
}

/// Seconds since the Unix epoch as `YYYY-MM-DD HH:MM:SS UTC` (proleptic Gregorian, no leap seconds).
pub(super) fn iso8601(secs: u64) -> String {
    let days = (secs / 86_400) as i64;
    let rem = secs % 86_400;
    // Howard Hinnant's civil-from-days.
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };
    format!(
        "{y:04}-{m:02}-{d:02} {:02}:{:02}:{:02} UTC",
        rem / 3600,
        (rem % 3600) / 60,
        rem % 60
    )
}

/// Locate `address` (optionally also as `address - image_base`) inside a blob.
fn locate<'b>(blobs: &'b [Blob<'_>], address: u64, image_base: u64) -> Option<(&'b Blob<'b>, usize)> {
    let try_one = |addr: u64| {
        blobs
            .iter()
            .filter(|b| b.address != 0 && !b.data.is_empty())
            .find(|b| addr >= b.address && addr - b.address < b.data.len() as u64)
            .map(|b| (b, (addr - b.address) as usize))
    };
    try_one(address).or_else(|| {
        (image_base != 0 && address >= image_base).then(|| try_one(address - image_base)).flatten()
    })
}

/// Turn candidates into hashed [`FunctionInfo`]s.
///
/// * zero-size candidates extend to the next candidate start inside the same blob (or the blob end);
/// * duplicates by name keep the first occurrence;
/// * candidates outside every blob (imports, stubs, bss) are skipped.
pub(super) fn build_functions(
    mut candidates: Vec<Candidate>,
    blobs: &[Blob<'_>],
    image_base: u64,
    options: &ParseOptions,
    model: &mut BinaryModel,
) {
    candidates.retain(|c| !c.name.is_empty());
    candidates.sort_by_key(|c| c.address);

    // Starts sorted per candidate list for size estimation.
    let starts: Vec<u64> = candidates.iter().map(|c| c.address).collect();
    let mut seen = HashSet::new();
    let mut skipped_big = 0usize;
    let mut out = Vec::new();

    for (i, c) in candidates.iter().enumerate() {
        if !seen.insert(c.name.clone()) {
            continue;
        }
        let Some((blob, offset)) = locate(blobs, c.address, image_base) else { continue };
        if !blob.executable {
            continue; // data symbols are not functions
        }
        let available = blob.data.len() - offset;
        let (size, estimated) = if c.size > 0 {
            ((c.size as usize).min(available), false)
        } else {
            let next = starts[i + 1..].iter().copied().find(|a| *a > c.address);
            let by_next = next.map_or(available, |n| ((n - c.address) as usize).min(available));
            (by_next, true)
        };
        if size == 0 {
            continue;
        }
        if size as u64 > options.max_function_bytes {
            skipped_big += 1;
            continue;
        }
        let bytes = &blob.data[offset..offset + size];
        out.push(FunctionInfo {
            name: c.name.clone(),
            address: c.address,
            size: size as u64,
            size_estimated: estimated,
            sha256: hex_digest(CONTENT_HASH, bytes),
            simhash: simhash(bytes),
        });
    }
    if skipped_big > 0 {
        model.notes.push(format!(
            "{skipped_big} function(s) above {} bytes were not hashed",
            options.max_function_bytes
        ));
    }
    if out.iter().any(|f| f.size_estimated) {
        model.notes.push(
            "some function sizes are estimated from the next function start (the format records none)"
                .to_owned(),
        );
    }
    model.functions = out;
}

/// Strings from every non-executable, loaded region, deduplicated in first-seen order.
pub(super) fn build_strings(blobs: &[Blob<'_>], options: &ParseOptions, model: &mut BinaryModel) {
    let mut seen = HashSet::new();
    let mut out = Vec::new();
    'outer: for blob in blobs.iter().filter(|b| !b.executable && b.address != 0) {
        for s in extract_strings(blob.data, options.min_string_len) {
            if seen.insert(s.clone()) {
                out.push(s);
                if out.len() >= MAX_STRINGS {
                    model.notes.push(format!("string list truncated at {MAX_STRINGS} entries"));
                    break 'outer;
                }
            }
        }
    }
    model.strings = out;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn iso8601_known_dates() {
        assert_eq!(iso8601(0), "1970-01-01 00:00:00 UTC");
        assert_eq!(iso8601(1_700_000_000), "2023-11-14 22:13:20 UTC");
        assert_eq!(iso8601(951_782_400), "2000-02-29 00:00:00 UTC");
    }

    #[test]
    fn sizes_are_estimated_and_clamped() {
        let data = (0u8..64).collect::<Vec<_>>();
        let blobs = [Blob { name: ".text".into(), address: 0x1000, executable: true, data: &data }];
        let cands = vec![
            Candidate { name: "a".into(), address: 0x1000, size: 0 },
            Candidate { name: "b".into(), address: 0x1010, size: 0 },
            Candidate { name: "c".into(), address: 0x1030, size: 1000 },
            Candidate { name: "outside".into(), address: 0x9000, size: 4 },
            Candidate { name: "a".into(), address: 0x1020, size: 4 },
        ];
        let mut model = BinaryModel::default();
        build_functions(cands, &blobs, 0, &ParseOptions::default(), &mut model);
        let by = |n: &str| model.functions.iter().find(|f| f.name == n).unwrap();
        assert_eq!(by("a").size, 0x10);
        assert!(by("a").size_estimated);
        assert_eq!(by("b").size, 0x10, "ends at the next function start, even a duplicate name");
        assert_eq!(by("c").size, 16, "clamped to the blob end");
        assert!(model.functions.iter().all(|f| f.name != "outside"));
        assert_eq!(model.functions.iter().filter(|f| f.name == "a").count(), 1);
    }
}
