//! [`TextCompareModel`]: the single object both editors render from.
//!
//! It owns the two sources, their line indexes and the alignment, and serves everything lazily:
//! decoded line text, windows into very long lines, inline differences (bounded cache), search and
//! difference navigation. It never materialises a file or a full set of line strings.

use std::borrow::Cow;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use super::cache::BoundedCache;
use super::diff::{DiffConfig, DiffEngine, Side};
use super::diff_index::{DiffIndex, DiffRow, DiffStats, DiffType};
use super::encoding::{self, TextEncoding};
use super::error::{CancelToken, EngineError, Progress};
use super::line_index::{IndexConfig, LineIndex};
use super::source::{MapPolicy, MappedTextFile, TextSource};
use crate::text::diff::compute_inline_diff;
use crate::text::models::InlineDiff;

/// Which file of the pair.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SideId {
    /// Left file.
    Left,
    /// Right file.
    Right,
}

/// Everything configurable about building a model.
#[derive(Clone)]
pub struct CompareConfig {
    /// Hashing / normalisation / index storage.
    pub index: IndexConfig,
    /// Alignment strategy.
    pub diff: DiffConfig,
    /// mmap vs buffered access.
    pub map_policy: MapPolicy,
    /// Maximum number of cached inline diffs.
    pub inline_cache_entries: usize,
    /// Lines longer than this many bytes get no inline diff (and are shown windowed).
    pub inline_max_line_bytes: usize,
}

impl Default for CompareConfig {
    fn default() -> Self {
        Self {
            index: IndexConfig::default(),
            diff: DiffConfig::default(),
            map_policy: MapPolicy::Auto,
            inline_cache_entries: 4096,
            inline_max_line_bytes: 16 * 1024,
        }
    }
}

struct SideData {
    source: Arc<dyn TextSource>,
    index: LineIndex,
    path: Option<PathBuf>,
}

/// A decoded line (or window of a line).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LineWindow {
    /// Decoded text of the window.
    pub text: String,
    /// Character offset in the line at which the window starts.
    pub start_char: u64,
    /// True when text exists beyond the end of the window.
    pub more_after: bool,
    /// Byte length of the whole line in the file (an upper bound of its length in characters).
    pub line_bytes: u32,
}

/// A search hit. Offsets are UTF-8 byte offsets in the decoded line text.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Match {
    /// Zero-based line on the searched side.
    pub line: u64,
    /// Byte offset of the match in the decoded line.
    pub start: usize,
    /// Byte length of the match in the decoded line.
    pub len: usize,
}

/// Search parameters.
#[derive(Clone, Debug)]
pub struct FindOptions {
    /// Text to find (never empty).
    pub needle: String,
    /// Case-sensitive matching.
    pub case_sensitive: bool,
}

/// Memory accounting for the benchmarks and the status bar.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct MemoryReport {
    /// Logical bytes of both line indexes.
    pub index_bytes: u64,
    /// Heap bytes of both indexes (0 for the part spilled to mapped files).
    pub index_resident_bytes: u64,
    /// Heap bytes of the diff index.
    pub diff_bytes: u64,
    /// Entries in the inline-diff cache.
    pub inline_cache_entries: u64,
}

/// The comparison model.
pub struct TextCompareModel {
    sides: [SideData; 2],
    diff: DiffIndex,
    config: CompareConfig,
    inline: Mutex<BoundedCache<u64, Option<Arc<InlineDiff>>>>,
}

impl TextCompareModel {
    /// Open two files and compare them. Either path may be `None` (treated as an empty file).
    pub fn open(
        left: Option<&Path>,
        right: Option<&Path>,
        config: CompareConfig,
        cancel: &CancelToken,
        progress: &mut dyn FnMut(Progress),
    ) -> Result<Self, EngineError> {
        let open = |p: Option<&Path>| -> Result<(Arc<dyn TextSource>, Option<PathBuf>), EngineError> {
            match p {
                Some(path) => Ok((
                    Arc::new(MappedTextFile::open_with(path, config.map_policy)?),
                    Some(path.to_path_buf()),
                )),
                None => Ok((Arc::new(super::source::MemorySource::new(Vec::new())), None)),
            }
        };
        let (ls, lp) = open(left)?;
        let (rs, rp) = open(right)?;
        let mut model = Self::from_sources(ls, rs, config, cancel, progress)?;
        model.sides[0].path = lp;
        model.sides[1].path = rp;
        Ok(model)
    }

    /// Build a model from arbitrary sources (tests, archives, already-resident small inputs).
    pub fn from_sources(
        left: Arc<dyn TextSource>,
        right: Arc<dyn TextSource>,
        config: CompareConfig,
        cancel: &CancelToken,
        progress: &mut dyn FnMut(Progress),
    ) -> Result<Self, EngineError> {
        let left_index = LineIndex::build(left.as_ref(), &config.index, cancel, progress)?;
        let right_index = LineIndex::build(right.as_ref(), &config.index, cancel, progress)?;
        let diff = DiffEngine::with_config(config.diff.clone()).diff(
            Side { source: left.as_ref(), index: &left_index },
            Side { source: right.as_ref(), index: &right_index },
            cancel,
            progress,
        )?;
        let cache = BoundedCache::new(config.inline_cache_entries.max(2));
        Ok(Self {
            sides: [
                SideData { source: left, index: left_index, path: None },
                SideData { source: right, index: right_index, path: None },
            ],
            diff,
            config,
            inline: Mutex::new(cache),
        })
    }

    fn side(&self, side: SideId) -> &SideData {
        &self.sides[usize::from(side == SideId::Right)]
    }

    // ---- structure -------------------------------------------------------------------------

    /// Number of display rows.
    #[must_use]
    pub fn row_count(&self) -> u64 {
        self.diff.row_count()
    }

    /// Resolve a display row.
    #[must_use]
    pub fn row(&self, row: u64) -> Option<DiffRow> {
        self.diff.row(row)
    }

    /// Number of lines in one file.
    #[must_use]
    pub fn line_count(&self, side: SideId) -> u64 {
        self.side(side).index.line_count()
    }

    /// The alignment.
    #[must_use]
    pub fn diff_index(&self) -> &DiffIndex {
        &self.diff
    }

    /// Alignment counters.
    #[must_use]
    pub fn stats(&self) -> DiffStats {
        self.diff.stats()
    }

    /// Row showing `line` of `side`.
    #[must_use]
    pub fn row_for_line(&self, side: SideId, line: u64) -> Option<u64> {
        match side {
            SideId::Left => self.diff.row_for_left_line(line),
            SideId::Right => self.diff.row_for_right_line(line),
        }
    }

    /// Next changed block start after `row`.
    #[must_use]
    pub fn next_difference(&self, row: u64) -> Option<u64> {
        self.diff.next_difference(row)
    }

    /// Previous changed block start before `row`.
    #[must_use]
    pub fn prev_difference(&self, row: u64) -> Option<u64> {
        self.diff.prev_difference(row)
    }

    /// Detected encoding of a side.
    #[must_use]
    pub fn encoding(&self, side: SideId) -> TextEncoding {
        self.side(side).index.encoding()
    }

    /// True when the file looks binary (NULs that UTF-16 does not explain).
    #[must_use]
    pub fn looks_binary(&self, side: SideId) -> bool {
        self.side(side).index.looks_binary()
    }

    /// File path, when the side was opened from one.
    #[must_use]
    pub fn path(&self, side: SideId) -> Option<&Path> {
        self.side(side).path.as_deref()
    }

    /// Size of the source in bytes.
    #[must_use]
    pub fn source_len(&self, side: SideId) -> u64 {
        self.side(side).source.len()
    }

    /// Fails when a backing file changed since it was opened.
    pub fn verify_unchanged(&self) -> Result<(), EngineError> {
        for s in &self.sides {
            s.source.verify_unchanged()?;
        }
        Ok(())
    }

    /// Memory accounting.
    #[must_use]
    pub fn memory_report(&self) -> MemoryReport {
        MemoryReport {
            index_bytes: self.sides.iter().map(|s| s.index.record_bytes()).sum(),
            index_resident_bytes: self.sides.iter().map(|s| s.index.resident_bytes()).sum(),
            diff_bytes: self.diff.heap_bytes(),
            inline_cache_entries: self.inline.lock().map(|c| c.len() as u64).unwrap_or(0),
        }
    }

    // ---- text ------------------------------------------------------------------------------

    /// Byte length of a line (no I/O).
    #[must_use]
    pub fn line_byte_len(&self, side: SideId, line: u64) -> Option<u32> {
        self.side(side).index.record(line).map(|r| r.length)
    }

    fn raw_line(&self, side: SideId, line: u64) -> io::Result<Cow<'_, [u8]>> {
        let s = self.side(side);
        s.index.line_bytes(s.source.as_ref(), line)
    }

    /// Whole decoded line (lossy). Prefer [`line_window`](Self::line_window) for unbounded input.
    pub fn line_text(&self, side: SideId, line: u64) -> io::Result<String> {
        let raw = self.raw_line(side, line)?;
        Ok(encoding::decode_line(&raw, self.encoding(side)).into_owned())
    }

    /// Decode at most `max_chars` characters starting at character `start_char`. Only the bytes
    /// that are needed are touched, so a 1 GB single-line file stays cheap near its beginning.
    pub fn line_window(
        &self,
        side: SideId,
        line: u64,
        start_char: u64,
        max_chars: usize,
    ) -> io::Result<LineWindow> {
        let raw = self.raw_line(side, line)?;
        let line_bytes = raw.len() as u32;
        match self.encoding(side) {
            TextEncoding::Utf8 => {
                // Skip `start_char` characters by counting non-continuation bytes.
                let mut skip = start_char;
                let mut pos = 0usize;
                if skip > 0 {
                    while pos < raw.len() {
                        if raw[pos] & 0xC0 != 0x80 {
                            if skip == 0 {
                                break;
                            }
                            skip -= 1;
                        }
                        pos += 1;
                    }
                    while pos < raw.len() && raw[pos] & 0xC0 == 0x80 {
                        pos += 1;
                    }
                }
                // Over-read generously (4 bytes per char), then cut to `max_chars`.
                let end = pos.saturating_add(max_chars.saturating_mul(4)).min(raw.len());
                let mut slice_end = end;
                while slice_end < raw.len() && slice_end > pos && raw[slice_end] & 0xC0 == 0x80 {
                    slice_end += 1;
                }
                let decoded = String::from_utf8_lossy(&raw[pos..slice_end]);
                let mut text = String::new();
                let mut count = 0usize;
                let mut consumed_all = true;
                for (i, c) in decoded.char_indices() {
                    if count == max_chars {
                        consumed_all = i == decoded.len();
                        break;
                    }
                    text.push(c);
                    count += 1;
                }
                let more = !consumed_all || slice_end < raw.len();
                Ok(LineWindow { text, start_char, more_after: more, line_bytes })
            }
            enc => {
                let all = encoding::decode_line(&raw, enc);
                let mut it = all.chars().skip(start_char as usize);
                let text: String = it.by_ref().take(max_chars).collect();
                let more = it.next().is_some();
                Ok(LineWindow { text, start_char, more_after: more, line_bytes })
            }
        }
    }

    /// Lazily computed, cached character-level difference for a `Replace` row. `None` for other
    /// rows and for lines above `inline_max_line_bytes`. Offsets are UTF-8 byte offsets in the
    /// decoded text of each line.
    pub fn inline_diff(&self, row: u64) -> Option<Arc<InlineDiff>> {
        let r = self.diff.row(row)?;
        if r.kind != DiffType::Replace {
            return None;
        }
        if let Some(hit) = self.inline.lock().ok()?.get(&row) {
            return hit;
        }
        let (l, rt) = (r.left_line?, r.right_line?);
        let max = self.config.inline_max_line_bytes as u32;
        let computed = if self.line_byte_len(SideId::Left, l)? > max
            || self.line_byte_len(SideId::Right, rt)? > max
        {
            None
        } else {
            let lt = self.line_text(SideId::Left, l).ok()?;
            let rtxt = self.line_text(SideId::Right, rt).ok()?;
            Some(Arc::new(compute_inline_diff(&lt, &rtxt)))
        };
        self.inline.lock().ok()?.insert(row, computed.clone());
        computed
    }

    // ---- search ----------------------------------------------------------------------------

    /// Find the next/previous match of `opts` on one side, starting after (forward) or before
    /// (backward) `(line, byte_col)`. Lines are visited in file order without loading the file.
    /// Pass `None` as start to search from the beginning (forward) or end (backward).
    pub fn find(
        &self,
        side: SideId,
        opts: &FindOptions,
        start: Option<(u64, usize)>,
        forward: bool,
        cancel: &CancelToken,
    ) -> Result<Option<Match>, EngineError> {
        if opts.needle.is_empty() {
            return Ok(None);
        }
        let count = self.line_count(side);
        if count == 0 {
            return Ok(None);
        }
        let needle_lower: Vec<char> = if opts.case_sensitive {
            Vec::new()
        } else {
            opts.needle.chars().map(fold).collect()
        };
        let visit = |line: u64, from: usize, until: usize| -> Result<Option<Match>, EngineError> {
            let raw = self.raw_line(side, line)?;
            let enc = self.encoding(side);
            // Fast path: case-sensitive search on valid UTF-8 bytes needs no decoding.
            if opts.case_sensitive && enc == TextEncoding::Utf8 {
                if let Ok(text) = std::str::from_utf8(&raw) {
                    return Ok(search_in(text, opts, &needle_lower, from, until, forward)
                        .map(|m| Match { line, ..m }));
                }
            }
            let text = encoding::decode_line(&raw, enc);
            Ok(search_in(&text, opts, &needle_lower, from, until, forward).map(|m| Match { line, ..m }))
        };
        let (first, first_from) = match start {
            Some((l, c)) => (l.min(count - 1), c),
            None => (if forward { 0 } else { count - 1 }, if forward { 0 } else { usize::MAX }),
        };
        // First line: partial range after/before the start column.
        let (from, until) = if forward {
            (if start.is_some() { first_from.saturating_add(1) } else { 0 }, usize::MAX)
        } else {
            (0, if start.is_some() { first_from } else { usize::MAX })
        };
        if let Some(m) = visit(first, from, until)? {
            return Ok(Some(m));
        }
        let mut line = first;
        for step in 1..count {
            if step & 0x3FF == 0 {
                cancel.check()?;
            }
            line = if forward { (line + 1) % count } else { (line + count - 1) % count };
            if let Some(m) = visit(line, 0, usize::MAX)? {
                return Ok(Some(m));
            }
        }
        // Wrapped all the way around: the other part of the first line.
        let (from, until) = if forward { (0, first_from.saturating_add(1)) } else { (first_from, usize::MAX) };
        if start.is_some() {
            if let Some(m) = visit(first, from, until)? {
                return Ok(Some(m));
            }
        }
        Ok(None)
    }
}

fn fold(c: char) -> char {
    c.to_lowercase().next().unwrap_or(c)
}

/// Search `text` for the needle within match-start range `[from, until)`.
fn search_in(
    text: &str,
    opts: &FindOptions,
    needle_lower: &[char],
    from: usize,
    until: usize,
    forward: bool,
) -> Option<Match> {
    let in_range = |s: usize| s >= from && s < until;
    if opts.case_sensitive {
        if forward {
            let mut pos = from.min(text.len());
            while !text.is_char_boundary(pos) {
                pos += 1;
            }
            let hit = text[pos..].find(&opts.needle)? + pos;
            return in_range(hit).then_some(Match { line: 0, start: hit, len: opts.needle.len() });
        }
        let mut best = None;
        let mut pos = 0;
        while let Some(i) = text[pos..].find(&opts.needle) {
            let abs = pos + i;
            if abs >= until {
                break;
            }
            if abs >= from {
                best = Some(abs);
            }
            pos = abs + text[abs..].chars().next().map_or(1, char::len_utf8);
        }
        return best.map(|s| Match { line: 0, start: s, len: opts.needle.len() });
    }
    let mut result = None;
    for (start, _) in text.char_indices() {
        if !in_range(start) {
            if start >= until {
                break;
            }
            continue;
        }
        let mut chars = text[start..].char_indices();
        let mut matched_len = 0;
        let mut ok = true;
        for &n in needle_lower {
            match chars.next() {
                Some((off, c)) if fold(c) == n => matched_len = off + c.len_utf8(),
                _ => {
                    ok = false;
                    break;
                }
            }
        }
        if ok {
            let m = Match { line: 0, start, len: matched_len };
            if forward {
                return Some(m);
            }
            result = Some(m);
        }
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::text::engine::source::MemorySource;

    fn model(l: &str, r: &str) -> TextCompareModel {
        model_cfg(l.as_bytes(), r.as_bytes(), CompareConfig::default())
    }

    fn model_cfg(l: &[u8], r: &[u8], cfg: CompareConfig) -> TextCompareModel {
        TextCompareModel::from_sources(
            Arc::new(MemorySource::new(l.to_vec())),
            Arc::new(MemorySource::new(r.to_vec())),
            cfg,
            &CancelToken::new(),
            &mut |_| {},
        )
        .unwrap()
    }

    #[test]
    fn rows_text_and_navigation() {
        let m = model("a\nb\nc\nd\ne\n", "a\nB\nc\nd\nX\nY\ne\n");
        assert_eq!(m.row_count(), 7);
        let r = m.row(1).unwrap();
        assert_eq!(r.kind, DiffType::Replace);
        assert_eq!(m.line_text(SideId::Left, r.left_line.unwrap()).unwrap(), "b");
        assert_eq!(m.line_text(SideId::Right, r.right_line.unwrap()).unwrap(), "B");
        assert_eq!(m.next_difference(0), Some(1));
        assert_eq!(m.next_difference(1), Some(4));
        assert_eq!(m.prev_difference(6), Some(4));
        assert_eq!(m.row_for_line(SideId::Right, 5), Some(5));
    }

    #[test]
    fn inline_diff_is_lazy_cached_and_bounded() {
        let mut cfg = CompareConfig::default();
        cfg.inline_cache_entries = 4;
        let l: String = (0..50).map(|i| format!("value {i} old\n")).collect();
        let r: String = (0..50).map(|i| format!("value {i} new\n")).collect();
        let m = model_cfg(l.as_bytes(), r.as_bytes(), cfg);
        assert_eq!(m.memory_report().inline_cache_entries, 0);
        for row in 0..50 {
            let d = m.inline_diff(row).expect("replace row");
            assert!(!d.left.is_empty());
            assert!(m.memory_report().inline_cache_entries <= 4);
        }
        assert!(m.inline_diff(999).is_none());
        let same = model("x\n", "x\n");
        assert!(same.inline_diff(0).is_none());
    }

    #[test]
    fn very_long_lines_are_windowed_without_decoding_everything() {
        let long = "ab".repeat(2_000_000); // 4 MB on one line
        let m = model(&format!("{long}\nshort\n"), "short\n");
        assert_eq!(m.line_byte_len(SideId::Left, 0), Some(4_000_000));
        let w = m.line_window(SideId::Left, 0, 0, 10).unwrap();
        assert_eq!((w.text.as_str(), w.more_after), ("ababababab", true));
        let w = m.line_window(SideId::Left, 0, 3_999_995, 100).unwrap();
        assert_eq!((w.text.as_str(), w.more_after), ("babab", false));
        // No inline diff is attempted for huge lines.
        assert!(m.inline_diff(0).is_none());
    }

    #[test]
    fn windows_count_characters_not_bytes() {
        let m = model("héllo wörld 😀 end\n", "x\n");
        let w = m.line_window(SideId::Left, 0, 3, 5).unwrap();
        assert_eq!(w.text, "lo wö");
        let w = m.line_window(SideId::Left, 0, 12, 50).unwrap();
        assert_eq!((w.text.as_str(), w.more_after), ("😀 end", false));
    }

    #[test]
    fn invalid_utf8_and_binary_are_handled_gracefully() {
        let m = model_cfg(b"ok\n\xFF\xFEbad\x00line\nend", b"ok\n", CompareConfig::default());
        assert_eq!(m.line_text(SideId::Left, 1).unwrap(), "\u{FFFD}\u{FFFD}bad\u{0}line");
        assert!(m.looks_binary(SideId::Left));
        assert_eq!(m.line_count(SideId::Left), 3);
    }

    #[test]
    fn empty_files() {
        let m = model("", "");
        assert_eq!(m.row_count(), 0);
        assert!(m.row(0).is_none());
        assert_eq!(m.next_difference(0), None);
        let m = model("", "a\n");
        assert_eq!(m.row_count(), 1);
        assert_eq!(m.row(0).unwrap().kind, DiffType::Insert);
    }

    fn find(m: &TextCompareModel, needle: &str, cs: bool, start: Option<(u64, usize)>, fwd: bool) -> Option<(u64, usize)> {
        let hit = m
            .find(SideId::Left, &FindOptions { needle: needle.into(), case_sensitive: cs }, start, fwd, &CancelToken::new())
            .unwrap()?;
        Some((hit.line, hit.start))
    }

    #[test]
    fn find_forward_backward_wraps_and_folds_case() {
        let m = model("one Foo\ntwo foo foo\nthree\nFOO four\n", "x\n");
        assert_eq!(find(&m, "foo", true, None, true), Some((1, 4)));
        assert_eq!(find(&m, "foo", true, Some((1, 4)), true), Some((1, 8)));
        assert_eq!(find(&m, "foo", true, Some((1, 8)), true), None.or(Some((1, 4))));
        assert_eq!(find(&m, "foo", false, Some((1, 8)), true), Some((3, 0)));
        assert_eq!(find(&m, "foo", false, Some((3, 0)), true), Some((0, 4)));
        assert_eq!(find(&m, "foo", false, Some((1, 4)), false), Some((0, 4)));
        assert_eq!(find(&m, "foo", false, None, false), Some((3, 0)));
        assert_eq!(find(&m, "nothing", false, None, true), None);
        assert_eq!(find(&m, "", false, None, true), None);
    }

    #[test]
    fn find_handles_unicode_offsets_and_invalid_lines() {
        let m = model_cfg("grüße\nÄrger ärger\n".as_bytes(), b"x\n", CompareConfig::default());
        assert_eq!(find(&m, "ärger", false, None, true), Some((1, 0)));
        assert_eq!(find(&m, "ärger", false, Some((1, 0)), true), Some((1, 7)));
        let m = model_cfg(b"bad \xFF here\nfind me\n", b"x\n", CompareConfig::default());
        assert_eq!(find(&m, "here", true, None, true), Some((0, 8)));
    }

    #[test]
    fn find_can_be_cancelled() {
        let text = "needle-free line\n".repeat(5000);
        let m = model(&text, "x\n");
        let cancel = CancelToken::new();
        cancel.cancel();
        let r = m.find(SideId::Left, &FindOptions { needle: "zzz".into(), case_sensitive: true }, None, true, &cancel);
        assert!(matches!(r, Err(EngineError::Cancelled)));
    }
}
