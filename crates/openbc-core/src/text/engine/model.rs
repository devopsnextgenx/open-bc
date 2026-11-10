//! [`TextCompareModel`]: the single object both editors render from.
//!
//! It owns the two sources, their line indexes and the alignment, and serves everything lazily:
//! decoded line text, windows into very long lines, inline differences (bounded cache), search and
//! difference navigation. It never materialises a file or a full set of line strings.

use std::io;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, RwLock, RwLockReadGuard, RwLockWriteGuard};
use std::time::Instant;

use super::cache::BoundedCache;
use super::diff::{DiffConfig, DiffEngine, Side};
use super::diff_index::{DiffIndex, DiffRow, DiffStats, DiffType};
use super::encoding::{self, TextEncoding};
use super::error::{CancelToken, EngineError, Progress};
use super::line_index::{IndexConfig, LineIndex};
use super::line_index::{flags, LineRecord};
use super::rules::LineRules;
use super::source::{EditableSource, MapPolicy, MappedTextFile, TextSource};
use crate::telemetry::{self, Sample};
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
    /// Bytes held by the edit buffers (the memory cost of all edits so far).
    pub edit_bytes: u64,
}

/// What an edit changed in the displayed rows: rows `[first_row, first_row + old_rows)` were
/// replaced by `new_rows` rows (everything after shifted). Editors invalidate their caches from
/// `first_row` on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EditOutcome {
    /// Side whose lines were edited.
    pub side: SideId,
    /// First affected display row.
    pub first_row: u64,
    /// Rows that were replaced.
    pub old_rows: u64,
    /// Rows that replaced them.
    pub new_rows: u64,
    /// First edited line of `side` (zero-based).
    pub first_line: u64,
    /// Number of lines now occupying the edited position.
    pub inserted_lines: u64,
}

/// Result of a successful save.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SaveReport {
    /// File that was written.
    pub path: PathBuf,
    /// Bytes written.
    pub bytes: u64,
}

#[derive(Clone)]
struct UndoEntry {
    id: u64,
    first_line: u64,
    removed: Vec<LineRecord>,
    inserted: Vec<LineRecord>,
    /// Single line replaced by a single line: re-aligned locally, may absorb further typing.
    in_place: bool,
    sealed: bool,
    epoch: u64,
}

/// A line to insert: bytes as stored in the file's encoding plus its UTF-8 form for hashing.
struct NewLine {
    stored: Vec<u8>,
    utf8: Vec<u8>,
}

const MAX_UNDO_ENTRIES: usize = 100_000;

struct State {
    sources: [Arc<EditableSource>; 2],
    index: [LineIndex; 2],
    paths: [Option<PathBuf>; 2],
    diff: DiffIndex,
    undo: [Vec<UndoEntry>; 2],
    redo: [Vec<UndoEntry>; 2],
    next_id: u64,
    saved_id: [u64; 2],
    modified: [bool; 2],
    rules_epoch: u64,
}

fn slot(side: SideId) -> usize {
    usize::from(side == SideId::Right)
}

fn side_id(slot: usize) -> SideId {
    if slot == 0 { SideId::Left } else { SideId::Right }
}

static SAVE_COUNTER: AtomicU64 = AtomicU64::new(0);

/// The comparison model. Shared between the two editors (`Arc`); every method takes `&self`, edits
/// take a short exclusive lock.
pub struct TextCompareModel {
    state: RwLock<State>,
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
        let model = Self::from_sources(ls, rs, config, cancel, progress)?;
        {
            let mut st = model.write();
            st.paths = [lp, rp];
        }
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
        let byte_sizes = (left.len() as usize, right.len() as usize);
        let sources = [Arc::new(EditableSource::new(left)), Arc::new(EditableSource::new(right))];
        let index_started = Instant::now();
        let left_index = LineIndex::build(sources[0].as_ref(), &config.index, cancel, progress)?;
        let right_index = LineIndex::build(sources[1].as_ref(), &config.index, cancel, progress)?;
        let line_sizes = (left_index.line_count() as usize, right_index.line_count() as usize);
        telemetry::record(
            "text_engine_index",
            index_started,
            Sample {
                sizes: Some(byte_sizes),
                lines: Some(line_sizes),
                detail: Some("line index and hashes built".to_owned()),
                ..Sample::default()
            },
        );
        let diff_started = Instant::now();
        let diff = DiffEngine::with_config(config.diff.clone()).diff(
            Side { source: sources[0].as_ref(), index: &left_index },
            Side { source: sources[1].as_ref(), index: &right_index },
            cancel,
            progress,
        )?;
        let diff_stats = diff.stats();
        telemetry::record(
            "text_engine_diff",
            diff_started,
            Sample {
                sizes: Some(byte_sizes),
                lines: Some(line_sizes),
                detail: Some(format!(
                    "rows={} blocks={} inserted_rows={} deleted_rows={} replaced_rows={} degraded={}",
                    diff.row_count(),
                    diff_stats.blocks,
                    diff_stats.inserted_rows,
                    diff_stats.deleted_rows,
                    diff_stats.replaced_rows,
                    diff.is_degraded()
                )),
                ..Sample::default()
            },
        );
        let cache = BoundedCache::new(config.inline_cache_entries.max(2));
        Ok(Self {
            state: RwLock::new(State {
                sources,
                index: [left_index, right_index],
                paths: [None, None],
                diff,
                undo: [Vec::new(), Vec::new()],
                redo: [Vec::new(), Vec::new()],
                next_id: 1,
                saved_id: [0, 0],
                modified: [false, false],
                rules_epoch: 0,
            }),
            config,
            inline: Mutex::new(cache),
        })
    }

    fn read(&self) -> RwLockReadGuard<'_, State> {
        self.state.read().unwrap_or_else(|e| e.into_inner())
    }

    fn write(&self) -> RwLockWriteGuard<'_, State> {
        self.state.write().unwrap_or_else(|e| e.into_inner())
    }

    fn clear_inline_cache(&self) {
        if let Ok(mut cache) = self.inline.lock() {
            cache.clear();
        }
    }

    // ---- structure -------------------------------------------------------------------------

    /// Number of display rows.
    #[must_use]
    pub fn row_count(&self) -> u64 {
        self.read().diff.row_count()
    }

    /// Resolve a display row.
    #[must_use]
    pub fn row(&self, row: u64) -> Option<DiffRow> {
        self.read().diff.row(row)
    }

    /// Number of lines in one file (edits included).
    #[must_use]
    pub fn line_count(&self, side: SideId) -> u64 {
        self.read().index[slot(side)].line_count()
    }

    /// Lines per side as the alignment sees them.
    #[must_use]
    pub fn line_counts(&self) -> (u64, u64) {
        self.read().diff.line_counts()
    }

    /// True when part of the alignment used the coarse fallback.
    #[must_use]
    pub fn is_degraded(&self) -> bool {
        self.read().diff.is_degraded()
    }

    /// Per-bucket change summary for an overview strip.
    #[must_use]
    pub fn overview(&self, buckets: usize) -> Vec<u8> {
        self.read().diff.overview(buckets)
    }

    /// Alignment counters.
    #[must_use]
    pub fn stats(&self) -> DiffStats {
        self.read().diff.stats()
    }

    /// Row showing `line` of `side`.
    #[must_use]
    pub fn row_for_line(&self, side: SideId, line: u64) -> Option<u64> {
        let st = self.read();
        match side {
            SideId::Left => st.diff.row_for_left_line(line),
            SideId::Right => st.diff.row_for_right_line(line),
        }
    }

    /// Next changed block start after `row`.
    #[must_use]
    pub fn next_difference(&self, row: u64) -> Option<u64> {
        self.read().diff.next_difference(row)
    }

    /// Previous changed block start before `row`.
    #[must_use]
    pub fn prev_difference(&self, row: u64) -> Option<u64> {
        self.read().diff.prev_difference(row)
    }

    /// Lines of each side consumed by the rows before `row` (= the next line index per side).
    #[must_use]
    pub fn line_cursors(&self, row: u64) -> (u64, u64) {
        self.read().diff.cursors_at_row(row)
    }

    /// Start row and length of the changed block containing `row`.
    #[must_use]
    pub fn block_at(&self, row: u64) -> Option<(u64, u64)> {
        let st = self.read();
        let b = st.diff.blocks()[st.diff.block_containing(row)?];
        Some((b.start_row, b.rows))
    }

    /// Detected encoding of a side.
    #[must_use]
    pub fn encoding(&self, side: SideId) -> TextEncoding {
        self.read().index[slot(side)].encoding()
    }

    /// True when the file looks binary (NULs that UTF-16 does not explain).
    #[must_use]
    pub fn looks_binary(&self, side: SideId) -> bool {
        self.read().index[slot(side)].looks_binary()
    }

    /// File path, when the side has one.
    #[must_use]
    pub fn path(&self, side: SideId) -> Option<PathBuf> {
        self.read().paths[slot(side)].clone()
    }

    /// Size of the file as it was opened, in bytes.
    #[must_use]
    pub fn source_len(&self, side: SideId) -> u64 {
        self.read().sources[slot(side)].base_len()
    }

    /// Fails when a backing file changed since it was opened. Sides this model edited or saved
    /// itself are skipped (they legitimately differ from what was opened).
    pub fn verify_unchanged(&self) -> Result<(), EngineError> {
        let st = self.read();
        for i in 0..2 {
            if !st.modified[i] {
                st.sources[i].verify_unchanged()?;
            }
        }
        Ok(())
    }

    /// Memory accounting.
    #[must_use]
    pub fn memory_report(&self) -> MemoryReport {
        let st = self.read();
        MemoryReport {
            index_bytes: st.index.iter().map(LineIndex::record_bytes).sum(),
            index_resident_bytes: st.index.iter().map(LineIndex::resident_bytes).sum(),
            diff_bytes: st.diff.heap_bytes(),
            inline_cache_entries: self.inline.lock().map(|c| c.len() as u64).unwrap_or(0),
            edit_bytes: st.sources.iter().map(|s| s.edit_bytes()).sum(),
        }
    }

    // ---- text ------------------------------------------------------------------------------

    /// Byte length of a line (no I/O).
    #[must_use]
    pub fn line_byte_len(&self, side: SideId, line: u64) -> Option<u32> {
        self.read().index[slot(side)].record(line).map(|r| r.length)
    }

    /// Run `f` on the raw bytes of a line (zero-copy for mapped files) and its encoding. The read
    /// lock is held only for the duration of `f`.
    fn with_raw<R>(
        &self,
        side: SideId,
        line: u64,
        f: impl FnOnce(&[u8], TextEncoding) -> R,
    ) -> io::Result<R> {
        let st = self.read();
        let i = slot(side);
        let raw = st.index[i].line_bytes(st.sources[i].as_ref(), line)?;
        Ok(f(&raw, st.index[i].encoding()))
    }

    /// Whole decoded line (lossy). Prefer [`line_window`](Self::line_window) for unbounded input.
    pub fn line_text(&self, side: SideId, line: u64) -> io::Result<String> {
        self.with_raw(side, line, |raw, enc| encoding::decode_line(raw, enc).into_owned())
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
        self.with_raw(side, line, |raw, enc| window_from_raw(raw, enc, start_char, max_chars))
    }
}

// ---- editing -------------------------------------------------------------------------------------

fn invalid_input(message: impl Into<String>) -> EngineError {
    EngineError::Io(io::Error::new(io::ErrorKind::InvalidInput, message.into()))
}

fn utf8_of(raw: &[u8], enc: TextEncoding) -> Vec<u8> {
    match enc {
        TextEncoding::Utf8 => raw.to_vec(),
        _ => encoding::decode_line(raw, enc).into_owned().into_bytes(),
    }
}

/// Split text into lines on LF, CRLF or CR (no terminators in the result).
fn split_lines(text: &str) -> Vec<&str> {
    let mut out = Vec::new();
    let mut start = 0;
    let bytes = text.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'\n' => { out.push(&text[start..i]); i += 1; start = i; }
            b'\r' => {
                out.push(&text[start..i]);
                i += if bytes.get(i + 1) == Some(&b'\n') { 2 } else { 1 };
                start = i;
            }
            _ => i += 1,
        }
    }
    out.push(&text[start..]);
    out
}

impl State {
    fn side<'a>(&'a self, i: usize) -> Side<'a> {
        Side { source: self.sources[i].as_ref(), index: &self.index[i] }
    }

    fn top_id(&self, i: usize) -> u64 {
        self.undo[i].last().map_or(0, |e| e.id)
    }

    /// Turn new lines into records, applying the line-ending rules: a replaced line keeps its
    /// ending, others take the file's dominant one, and appending after a last line that has no
    /// terminator gives that line one and leaves the new last line without (so a file that did not
    /// end with a newline still does not).
    fn prepare_insert(
        &mut self,
        i: usize,
        first: u64,
        remove: u64,
        lines: Vec<NewLine>,
    ) -> Result<(u64, u64, Vec<LineRecord>), EngineError> {
        let total = self.index[i].line_count();
        let mut first = first.min(total);
        let mut remove = remove.min(total - first);
        let dominant = self.index[i].dominant_eol();
        let enc = self.index[i].encoding();
        let mut last_eol = if remove > 0 {
            self.index[i].record(first + remove - 1).map_or(dominant, |r| r.eol())
        } else {
            dominant
        };
        let mut prepend = None;
        if remove == 0 && first == total && total > 0 && !lines.is_empty() {
            let prev = self.index[i].record(total - 1).ok_or(EngineError::SourceChanged)?;
            if prev.eol() == flags::EOL_NONE {
                let raw = self.sources[i].read(prev.offset, prev.length as usize)?;
                let utf8 = utf8_of(&raw, enc);
                prepend = Some(self.index[i].make_record(prev.offset, prev.length, &utf8, dominant));
                first = total - 1;
                remove = 1;
                last_eol = flags::EOL_NONE;
            } else {
                last_eol = prev.eol();
            }
        }
        let n = lines.len();
        let mut records = Vec::with_capacity(n + 1);
        if let Some(p) = prepend {
            records.push(p);
        }
        for (k, line) in lines.into_iter().enumerate() {
            let eol = if k + 1 == n { last_eol } else { dominant };
            let address = self.sources[i].append(&line.stored);
            records.push(self.index[i].make_record(address, line.stored.len() as u32, &line.utf8, eol));
        }
        Ok((first, remove, records))
    }

    /// Splice records into one side and re-align only the affected region of the diff.
    fn apply_records(
        &mut self,
        engine: &DiffEngine,
        i: usize,
        first: u64,
        remove: u64,
        insert: Vec<LineRecord>,
        local_only: bool,
    ) -> Result<(EditOutcome, Vec<LineRecord>), EngineError> {
        let total = self.index[i].line_count();
        let first = first.min(total);
        let remove = remove.min(total - first);
        let insert_n = insert.len() as u64;
        let rows = self.diff.row_count();
        let row_of = |d: &DiffIndex, line: u64| {
            if i == 0 { d.row_for_left_line(line) } else { d.row_for_right_line(line) }
        };
        let row_a = if first < total { row_of(&self.diff, first).unwrap_or(rows) } else { rows };
        let row_b = if remove > 0 {
            row_of(&self.diff, first + remove - 1).map_or(row_a, |r| r + 1)
        } else {
            row_a
        };
        let (mut r0, mut r1) = (row_a, row_b.max(row_a));
        if !local_only {
            // Grow to the change blocks the edit touches so the whole block is re-paired.
            let block = |d: &DiffIndex, row: u64| d.block_containing(row).map(|b| d.blocks()[b]);
            if r0 < rows {
                if let Some(b) = block(&self.diff, r0) {
                    r0 = r0.min(b.start_row);
                }
            }
            if r0 > 0 {
                if let Some(b) = block(&self.diff, r0 - 1) {
                    r0 = b.start_row;
                }
            }
            if r1 > 0 {
                if let Some(b) = block(&self.diff, r1 - 1) {
                    r1 = r1.max(b.start_row + b.rows);
                }
            }
            if r1 < rows {
                if let Some(b) = block(&self.diff, r1) {
                    r1 = b.start_row + b.rows;
                }
            }
        }
        let (l0, q0) = self.diff.cursors_at_row(r0);
        let (l1, q1) = self.diff.cursors_at_row(r1);
        let removed = self.index[i].splice(first, remove, insert);
        let delta = insert_n as i64 - remove as i64;
        let shift = |v: u64| (v as i64 + delta) as u64;
        let (lr, rr) = if i == 0 { ((l0, shift(l1)), (q0, q1)) } else { ((l0, l1), (q0, shift(q1))) };
        let cancel = CancelToken::new();
        let region = engine.diff_region(
            self.side(0),
            self.side(1),
            (lr.0 as usize, lr.1 as usize),
            (rr.0 as usize, rr.1 as usize),
            &cancel,
            &mut |_| {},
        );
        match region {
            Ok(builder) => {
                let region = builder.finish();
                let new_rows = region.row_count();
                self.diff.splice_region(r0, r1, &region);
                self.modified[i] = true;
                let outcome = EditOutcome {
                    side: side_id(i),
                    first_row: r0,
                    old_rows: r1 - r0,
                    new_rows,
                    first_line: first,
                    inserted_lines: insert_n,
                };
                Ok((outcome, removed))
            }
            Err(e) => {
                // Put the lines back so the model never ends up half-edited.
                self.index[i].splice(first, insert_n, removed);
                Err(e)
            }
        }
    }

    fn refresh_records(&self, i: usize, records: &[LineRecord]) -> Result<Vec<LineRecord>, EngineError> {
        let enc = self.index[i].encoding();
        records
            .iter()
            .map(|r| {
                let raw = self.sources[i].read(r.offset, r.length as usize)?;
                Ok(self.index[i].make_record(r.offset, r.length, &utf8_of(&raw, enc), r.eol()))
            })
            .collect()
    }
}

impl TextCompareModel {
    fn engine(&self) -> DiffEngine {
        DiffEngine::with_config(self.config.diff.clone())
    }

    fn push_undo(
        st: &mut State,
        i: usize,
        first_line: u64,
        removed: Vec<LineRecord>,
        inserted: Vec<LineRecord>,
        in_place: bool,
        coalesce: bool,
    ) {
        st.redo[i].clear();
        let epoch = st.rules_epoch;
        if coalesce && in_place {
            if let Some(last) = st.undo[i].last_mut() {
                if last.in_place && !last.sealed && last.first_line == first_line && last.epoch == epoch {
                    last.inserted = inserted; // keep the ORIGINAL removed text: one undo step for a run of typing
                    return;
                }
            }
        }
        let id = st.next_id;
        st.next_id += 1;
        st.undo[i].push(UndoEntry { id, first_line, removed, inserted, in_place, sealed: !coalesce, epoch });
        if st.undo[i].len() > MAX_UNDO_ENTRIES {
            st.undo[i].remove(0);
        }
    }

    /// Replace `remove` lines of `side` starting at `first_line` by `lines` (texts may themselves
    /// contain line breaks; they are split). `coalesce` merges consecutive in-place edits of the
    /// same line into one undo step (typing); call [`seal_undo`](Self::seal_undo) to end the run.
    pub fn replace_lines(
        &self,
        side: SideId,
        first_line: u64,
        remove: u64,
        lines: &[&str],
        coalesce: bool,
    ) -> Result<EditOutcome, EngineError> {
        let mut st = self.write();
        let i = slot(side);
        let enc = st.index[i].encoding();
        let new: Vec<NewLine> = lines
            .iter()
            .flat_map(|t| split_lines(t))
            .map(|t| NewLine { stored: encoding::encode_line(t, enc), utf8: t.as_bytes().to_vec() })
            .collect();
        let local = remove == 1 && new.len() == 1;
        let (first, remove, records) = st.prepare_insert(i, first_line, remove, new)?;
        let inserted = records.clone();
        let local = local && first == first_line.min(first) && remove == 1 && records.len() == 1;
        let (outcome, removed) = st.apply_records(&self.engine(), i, first, remove, records, local)?;
        Self::push_undo(&mut st, i, first, removed, inserted, local, coalesce);
        drop(st);
        self.clear_inline_cache();
        Ok(outcome)
    }

    /// End the current typing run so the next edit starts a new undo step.
    pub fn seal_undo(&self, side: SideId) {
        if let Some(last) = self.write().undo[slot(side)].last_mut() {
            last.sealed = true;
        }
    }

    /// Whether there is something to undo / redo on `side`.
    #[must_use]
    pub fn can_undo(&self, side: SideId) -> bool {
        !self.read().undo[slot(side)].is_empty()
    }

    /// See [`can_undo`](Self::can_undo).
    #[must_use]
    pub fn can_redo(&self, side: SideId) -> bool {
        !self.read().redo[slot(side)].is_empty()
    }

    /// Undo the last edit of `side`.
    pub fn undo(&self, side: SideId) -> Result<Option<EditOutcome>, EngineError> {
        let mut st = self.write();
        let i = slot(side);
        let Some(mut entry) = st.undo[i].pop() else { return Ok(None) };
        if entry.epoch != st.rules_epoch {
            entry.removed = st.refresh_records(i, &entry.removed)?;
            entry.inserted = st.refresh_records(i, &entry.inserted)?;
            entry.epoch = st.rules_epoch;
        }
        match st.apply_records(&self.engine(), i, entry.first_line, entry.inserted.len() as u64, entry.removed.clone(), entry.in_place) {
            Ok((outcome, _)) => {
                entry.sealed = true;
                st.redo[i].push(entry);
                drop(st);
                self.clear_inline_cache();
                Ok(Some(outcome))
            }
            Err(e) => {
                st.undo[i].push(entry);
                Err(e)
            }
        }
    }

    /// Redo the last undone edit of `side`.
    pub fn redo(&self, side: SideId) -> Result<Option<EditOutcome>, EngineError> {
        let mut st = self.write();
        let i = slot(side);
        let Some(mut entry) = st.redo[i].pop() else { return Ok(None) };
        if entry.epoch != st.rules_epoch {
            entry.removed = st.refresh_records(i, &entry.removed)?;
            entry.inserted = st.refresh_records(i, &entry.inserted)?;
            entry.epoch = st.rules_epoch;
        }
        match st.apply_records(&self.engine(), i, entry.first_line, entry.removed.len() as u64, entry.inserted.clone(), entry.in_place) {
            Ok((outcome, _)) => {
                st.undo[i].push(entry);
                drop(st);
                self.clear_inline_cache();
                Ok(Some(outcome))
            }
            Err(e) => {
                st.redo[i].push(entry);
                Err(e)
            }
        }
    }

    /// Number of undoable edit steps applied to `side` (what the unsaved-changes dialog reports).
    #[must_use]
    pub fn edit_count(&self, side: SideId) -> usize {
        self.read().undo[slot(side)].len()
    }

    /// True when `side` has edits that were not saved.
    #[must_use]
    pub fn is_dirty(&self, side: SideId) -> bool {
        let st = self.read();
        let i = slot(side);
        st.top_id(i) != st.saved_id[i]
    }

    /// Make the other side's lines of the changed block containing `row` identical to `from`'s
    /// (insert, delete or overwrite). Returns the outcome on the *destination* side, or `None` when
    /// `row` is not inside a changed block. One undo step on the destination.
    pub fn copy_block(&self, from: SideId, row: u64) -> Result<Option<EditOutcome>, EngineError> {
        let mut st = self.write();
        let Some(bi) = st.diff.block_containing(row) else { return Ok(None) };
        let block = st.diff.blocks()[bi];
        let (l0, q0) = st.diff.cursors_at_row(block.start_row);
        let (l1, q1) = st.diff.cursors_at_row(block.start_row + block.rows);
        let (src, dst) = (slot(from), 1 - slot(from));
        let (src_range, dst_range) = if src == 0 { ((l0, l1), (q0, q1)) } else { ((q0, q1), (l0, l1)) };
        let (src_enc, dst_enc) = (st.index[src].encoding(), st.index[dst].encoding());
        let mut lines = Vec::with_capacity((src_range.1 - src_range.0) as usize);
        for line in src_range.0..src_range.1 {
            let rec = st.index[src].record(line).ok_or(EngineError::SourceChanged)?;
            let raw = st.sources[src].read(rec.offset, rec.length as usize)?;
            let utf8 = utf8_of(&raw, src_enc);
            let stored = if src_enc == dst_enc {
                raw.into_owned() // byte-exact, including invalid sequences
            } else {
                encoding::encode_line(&String::from_utf8_lossy(&utf8), dst_enc)
            };
            lines.push(NewLine { stored, utf8 });
        }
        let (first, remove, records) = st.prepare_insert(dst, dst_range.0, dst_range.1 - dst_range.0, lines)?;
        let inserted = records.clone();
        let (outcome, removed) = st.apply_records(&self.engine(), dst, first, remove, records, false)?;
        Self::push_undo(&mut st, dst, first, removed, inserted, false, false);
        drop(st);
        self.clear_inline_cache();
        Ok(Some(outcome))
    }

    /// Install ignore rules (see [`LineRules`]; empty text clears them) and re-compare. Edits and
    /// undo history are kept. `Err` for an invalid regular expression, leaving everything as it was.
    pub fn set_rules(
        &self,
        text: &str,
        cancel: &CancelToken,
        progress: &mut dyn FnMut(Progress),
    ) -> Result<(), EngineError> {
        let rules: Option<Arc<LineRules>> = LineRules::parse(text).map_err(invalid_input)?.map(Arc::new);
        let mut st = self.write();
        let normalize = st.index[0].normalize_options();
        let limit = self.config.index.ram_record_limit;
        let left = st.index[0].rehash(st.sources[0].as_ref(), normalize, rules.clone(), cancel, progress, limit)?;
        let right = st.index[1].rehash(st.sources[1].as_ref(), normalize, rules, cancel, progress, limit)?;
        let diff = self.engine().diff(
            Side { source: st.sources[0].as_ref(), index: &left },
            Side { source: st.sources[1].as_ref(), index: &right },
            cancel,
            progress,
        )?;
        st.index = [left, right];
        st.diff = diff;
        st.rules_epoch += 1;
        drop(st);
        self.clear_inline_cache();
        Ok(())
    }

    /// The rule text currently in effect (empty when none).
    #[must_use]
    pub fn rules_text(&self) -> String {
        self.read().index[0].rules_key().to_owned()
    }

    /// Re-align both sides from scratch (after many edits the local alignment can be less than
    /// optimal; this restores the global optimum). Edits are kept.
    pub fn rediff(&self, cancel: &CancelToken, progress: &mut dyn FnMut(Progress)) -> Result<(), EngineError> {
        let mut st = self.write();
        let diff = self.engine().diff(st.side(0), st.side(1), cancel, progress)?;
        st.diff = diff;
        drop(st);
        self.clear_inline_cache();
        Ok(())
    }

    /// Write `side` to its file (or to `save_as` when given, which then becomes its path). The new
    /// content goes to a temporary file in the same directory that atomically replaces the target;
    /// encoding, BOM and every line's original ending are preserved. Undo history is kept.
    pub fn save(&self, side: SideId, save_as: Option<&Path>) -> Result<SaveReport, EngineError> {
        use std::io::Write;
        let mut st = self.write();
        let i = slot(side);
        let path = match save_as {
            Some(p) => p.to_path_buf(),
            None => st.paths[i].clone().ok_or_else(|| invalid_input("this side has no file to save to"))?,
        };
        // Write through symlinks instead of replacing them.
        let target = std::fs::canonicalize(&path).unwrap_or_else(|_| path.clone());
        let dir = target.parent().map(Path::to_path_buf).unwrap_or_default();
        let name = target.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        let tmp = dir.join(format!(
            ".{name}.openbc-save-{}-{}.tmp",
            std::process::id(),
            SAVE_COUNTER.fetch_add(1, Ordering::Relaxed)
        ));
        let result = (|| -> Result<u64, EngineError> {
            let file = std::fs::File::create(&tmp)?;
            let mut out = std::io::BufWriter::with_capacity(1 << 20, file);
            let enc = st.index[i].encoding();
            let mut bytes = 0u64;
            let bom = encoding::bom_bytes(enc, st.index[i].bom_len());
            out.write_all(bom)?;
            bytes += bom.len() as u64;
            for line in 0..st.index[i].line_count() {
                let rec = st.index[i].record(line).ok_or(EngineError::SourceChanged)?;
                let raw = st.sources[i].read(rec.offset, rec.length as usize)?;
                out.write_all(&raw)?;
                let eol = encoding::eol_bytes(rec.eol(), enc);
                out.write_all(&eol)?;
                bytes += raw.len() as u64 + eol.len() as u64;
            }
            let file = out.into_inner().map_err(|e| e.into_error())?;
            if let Ok(meta) = std::fs::metadata(&target) {
                let _ = std::fs::set_permissions(&tmp, meta.permissions());
            }
            file.sync_all()?;
            drop(file);
            std::fs::rename(&tmp, &target)?;
            Ok(bytes)
        })();
        match result {
            Ok(bytes) => {
                st.saved_id[i] = st.top_id(i);
                st.modified[i] = true;
                if save_as.is_some() {
                    st.paths[i] = Some(path.clone());
                }
                Ok(SaveReport { path: target, bytes })
            }
            Err(e) => {
                let _ = std::fs::remove_file(&tmp);
                Err(e)
            }
        }
    }
}

/// Decode a window of one raw line.
fn window_from_raw(raw: &[u8], encoding: TextEncoding, start_char: u64, max_chars: usize) -> LineWindow {
    {
        let line_bytes = raw.len() as u32;
        match encoding {
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
                LineWindow { text, start_char, more_after: more, line_bytes }
            }
            enc => {
                let all = encoding::decode_line(raw, enc);
                let mut it = all.chars().skip(start_char as usize);
                let text: String = it.by_ref().take(max_chars).collect();
                let more = it.next().is_some();
                LineWindow { text, start_char, more_after: more, line_bytes }
            }
        }
    }
}

impl TextCompareModel {
    /// Lazily computed, cached character-level difference for a `Replace` row. `None` for other
    /// rows and for lines above `inline_max_line_bytes`. Offsets are UTF-8 byte offsets in the
    /// decoded text of each line.
    pub fn inline_diff(&self, row: u64) -> Option<Arc<InlineDiff>> {
        let r = self.row(row)?;
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
            Ok(self.with_raw(side, line, |raw, enc| {
                // Fast path: case-sensitive search on valid UTF-8 bytes needs no decoding.
                if opts.case_sensitive && enc == TextEncoding::Utf8 {
                    if let Ok(text) = std::str::from_utf8(raw) {
                        return search_in(text, opts, &needle_lower, from, until, forward)
                            .map(|m| Match { line, ..m });
                    }
                }
                let text = encoding::decode_line(raw, enc);
                search_in(&text, opts, &needle_lower, from, until, forward).map(|m| Match { line, ..m })
            })?)
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


    // ---- editing ---------------------------------------------------------------------------

    fn all_lines(m: &TextCompareModel, side: SideId) -> Vec<String> {
        (0..m.line_count(side)).map(|l| m.line_text(side, l).unwrap()).collect()
    }

    /// Alignment must cover each line once and in order; Equal rows must really be equal and
    /// Replace rows must really differ.
    fn assert_valid(m: &TextCompareModel) {
        let (l, r) = (all_lines(m, SideId::Left), all_lines(m, SideId::Right));
        let (mut nl, mut nr) = (0u64, 0u64);
        for row in 0..m.row_count() {
            let d = m.row(row).unwrap();
            if let Some(x) = d.left_line {
                assert_eq!(x, nl, "left lines contiguous at row {row}");
                nl += 1;
            }
            if let Some(x) = d.right_line {
                assert_eq!(x, nr, "right lines contiguous at row {row}");
                nr += 1;
            }
            match d.kind {
                DiffType::Equal => assert_eq!(l[d.left_line.unwrap() as usize], r[d.right_line.unwrap() as usize], "row {row}"),
                DiffType::Replace => assert_ne!(l[d.left_line.unwrap() as usize], r[d.right_line.unwrap() as usize], "row {row}"),
                DiffType::Insert => assert!(d.left_line.is_none() && d.right_line.is_some()),
                DiffType::Delete => assert!(d.right_line.is_none() && d.left_line.is_some()),
            }
        }
        assert_eq!((nl, nr), (l.len() as u64, r.len() as u64));
        // line -> row lookups agree with the rows
        for (line, _) in l.iter().enumerate() {
            let row = m.row_for_line(SideId::Left, line as u64).unwrap();
            assert_eq!(m.row(row).unwrap().left_line, Some(line as u64));
        }
        for (line, _) in r.iter().enumerate() {
            let row = m.row_for_line(SideId::Right, line as u64).unwrap();
            assert_eq!(m.row(row).unwrap().right_line, Some(line as u64));
        }
    }

    fn joined(lines: &[String]) -> String {
        lines.iter().map(|l| format!("{l}\n")).collect()
    }

    fn snapshot(m: &TextCompareModel) -> (Vec<String>, Vec<String>, Vec<(Option<u64>, Option<u64>, DiffType)>) {
        let rows = (0..m.row_count()).map(|r| m.row(r).unwrap()).map(|d| (d.left_line, d.right_line, d.kind)).collect();
        (all_lines(m, SideId::Left), all_lines(m, SideId::Right), rows)
    }

    #[test]
    fn in_place_edit_reclassifies_only_its_row_and_undo_restores_it() {
        let m = model("a\nb\nc\n", "a\nb\nc\n");
        assert!(m.stats().blocks == 0);
        let out = m.replace_lines(SideId::Right, 1, 1, &["B"], true).unwrap();
        assert_eq!((out.first_row, out.old_rows, out.new_rows), (1, 1, 1));
        assert_eq!(m.row(1).unwrap().kind, DiffType::Replace);
        assert_eq!(m.line_text(SideId::Right, 1).unwrap(), "B");
        assert!(m.is_dirty(SideId::Right) && !m.is_dirty(SideId::Left));
        assert_valid(&m);
        // typing the original text back makes it Equal again
        m.replace_lines(SideId::Right, 1, 1, &["b"], true).unwrap();
        assert_eq!(m.row(1).unwrap().kind, DiffType::Equal);
        assert_eq!(m.stats().blocks, 0);
        // inline diff reflects the new text, not a stale cache
        m.replace_lines(SideId::Right, 2, 1, &["c d"], false).unwrap();
        let before = m.inline_diff(2).unwrap();
        m.replace_lines(SideId::Right, 2, 1, &["zzz"], false).unwrap();
        let after = m.inline_diff(2).unwrap();
        assert_ne!(format!("{:?}", before), format!("{:?}", after));
    }

    #[test]
    fn inserting_and_deleting_lines_shifts_rows_and_keeps_alignment_valid() {
        let m = model("a\nb\nc\nd\n", "a\nb\nc\nd\n");
        m.replace_lines(SideId::Left, 2, 0, &["NEW1", "NEW2"], false).unwrap();
        assert_eq!(all_lines(&m, SideId::Left), ["a", "b", "NEW1", "NEW2", "c", "d"]);
        assert_eq!(m.row_count(), 6);
        assert_eq!(m.stats().deleted_rows, 2);
        assert_valid(&m);
        m.replace_lines(SideId::Right, 0, 2, &[], false).unwrap(); // delete a,b on the right
        assert_valid(&m);
        assert_eq!(all_lines(&m, SideId::Right), ["c", "d"]);
        // multi-line text in one string is split into lines
        m.replace_lines(SideId::Right, 1, 0, &["x\r\ny\nz"], false).unwrap();
        assert_eq!(all_lines(&m, SideId::Right), ["c", "x", "y", "z", "d"]);
        assert_valid(&m);
    }

    #[test]
    fn random_edits_keep_the_alignment_valid_and_undo_everything_restores_the_original() {
        let base: Vec<String> = (0..60).map(|i| format!("line {i}")).collect();
        let mut other = base.clone();
        other[10] = "changed 10".into();
        other.remove(30);
        other.insert(45, "inserted".into());
        let m = model(&joined(&base), &joined(&other));
        let original = snapshot(&m);
        let mut seed = 0xFEEDu64;
        let mut next = move || { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; seed };
        let mut applied = [0usize; 2];
        for step in 0..150 {
            let side = if next() % 2 == 0 { SideId::Left } else { SideId::Right };
            let total = m.line_count(side);
            let first = if total == 0 { 0 } else { next() % (total + 1) };
            let remove = if first >= total { 0 } else { next() % 4.min(total - first + 1) }.min(total.saturating_sub(first));
            let n_ins = (next() % 4) as usize;
            let texts: Vec<String> = (0..n_ins).map(|k| format!("edit{step}_{k}")).collect();
            let refs: Vec<&str> = texts.iter().map(String::as_str).collect();
            if remove == 0 && refs.is_empty() { continue; }
            m.replace_lines(side, first, remove, &refs, false).unwrap();
            applied[slot(side)] += 1;
            assert_valid(&m);
        }
        // a global re-diff equals diffing the same content from scratch
        let edited = snapshot(&m);
        m.rediff(&CancelToken::new(), &mut |_| {}).unwrap();
        assert_valid(&m);
        let fresh = model(&joined(&edited.0), &joined(&edited.1));
        assert_eq!(snapshot(&m).2, snapshot(&fresh).2);
        // undo everything (each side independently) -> exactly the original content and alignment
        for side in [SideId::Left, SideId::Right] {
            for _ in 0..applied[slot(side)] {
                assert!(m.undo(side).unwrap().is_some());
                assert_valid(&m);
            }
            assert!(m.undo(side).unwrap().is_none());
            assert!(!m.is_dirty(side));
        }
        let restored = snapshot(&m);
        assert_eq!((&restored.0, &restored.1), (&original.0, &original.1));
        // redo everything -> the edited content again
        for side in [SideId::Left, SideId::Right] {
            while m.redo(side).unwrap().is_some() {}
        }
        let again = snapshot(&m);
        assert_eq!((&again.0, &again.1), (&edited.0, &edited.1));
        assert_valid(&m);
    }

    #[test]
    fn typing_is_one_undo_step_until_sealed() {
        let m = model("hello\nworld\n", "hello\nworld\n");
        for text in ["h", "he", "hel", "hell", "hello!"] {
            m.replace_lines(SideId::Left, 0, 1, &[text], true).unwrap();
        }
        m.seal_undo(SideId::Left);
        m.replace_lines(SideId::Left, 0, 1, &["hello!!"], true).unwrap(); // a new run
        m.undo(SideId::Left).unwrap();
        assert_eq!(m.line_text(SideId::Left, 0).unwrap(), "hello!");
        m.undo(SideId::Left).unwrap();
        assert_eq!(m.line_text(SideId::Left, 0).unwrap(), "hello"); // whole first run undone at once
        assert!(!m.can_undo(SideId::Left));
        assert!(m.can_redo(SideId::Left));
        // a new edit discards the redo history
        m.replace_lines(SideId::Left, 1, 1, &["w"], false).unwrap();
        assert!(!m.can_redo(SideId::Left));
    }

    #[test]
    fn dirty_follows_the_saved_state_through_undo_and_redo() {
        let dir = std::env::temp_dir().join(format!("openbc_dirty_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let (lp, rp) = (dir.join("l.txt"), dir.join("r.txt"));
        std::fs::write(&lp, "a\nb\n").unwrap();
        std::fs::write(&rp, "a\nb\n").unwrap();
        let m = TextCompareModel::open(Some(&lp), Some(&rp), CompareConfig::default(), &CancelToken::new(), &mut |_| {}).unwrap();
        assert!(!m.is_dirty(SideId::Left));
        m.replace_lines(SideId::Left, 0, 1, &["A"], false).unwrap();
        assert!(m.is_dirty(SideId::Left));
        m.undo(SideId::Left).unwrap();
        assert!(!m.is_dirty(SideId::Left)); // back at the saved state
        m.redo(SideId::Left).unwrap();
        m.save(SideId::Left, None).unwrap();
        assert!(!m.is_dirty(SideId::Left));
        assert_eq!(std::fs::read_to_string(&lp).unwrap(), "A\nb\n");
        m.replace_lines(SideId::Left, 1, 1, &["B"], false).unwrap();
        assert!(m.is_dirty(SideId::Left));
        m.undo(SideId::Left).unwrap();
        assert!(!m.is_dirty(SideId::Left));
        m.undo(SideId::Left).unwrap(); // undo the already-saved edit: now differs from disk
        assert!(m.is_dirty(SideId::Left));
        assert!(m.verify_unchanged().is_ok()); // our own save is not an "external change"
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn copy_block_makes_the_sides_equal_in_both_directions_and_is_undoable() {
        let l = "same\nleft-only 1\nleft-only 2\nsame2\nL change\nsame3\n";
        let r = "same\nsame2\nR change\nright-only\nsame3\n";
        let m = model(l, r);
        let before = snapshot(&m);
        assert_eq!(m.stats().blocks, 2); // the Replace + Insert at the end form one block
        // Left -> Right on a Delete block (left has two lines the right lacks)
        let out = m.copy_block(SideId::Left, 1).unwrap().unwrap();
        assert_eq!(out.side, SideId::Right);
        assert_eq!(all_lines(&m, SideId::Right)[1..3], ["left-only 1", "left-only 2"]);
        assert_valid(&m);
        // Right -> Left on the Replace/Insert block at the end
        let row = m.next_difference(2).unwrap();
        m.copy_block(SideId::Right, row).unwrap();
        assert_valid(&m);
        // undoing the copies restores the original
        while m.undo(SideId::Right).unwrap().is_some() {}
        while m.undo(SideId::Left).unwrap().is_some() {}
        assert_eq!(snapshot(&m), before);
        // copying with the caret outside any block does nothing
        assert!(m.copy_block(SideId::Left, 0).unwrap().is_none());
    }

    #[test]
    fn copy_block_until_identical() {
        let m = model("a\nX\nb\nY\nc\n", "a\nb\nZ\nc\nW\n");
        while let Some(row) = m.next_difference(u64::MAX - 1).or_else(|| m.next_difference(0)).or_else(|| (m.stats().blocks > 0).then_some(0)) {
            if m.copy_block(SideId::Left, row).unwrap().is_none() { break; }
            assert_valid(&m);
        }
        assert_eq!(m.stats().blocks, 0);
        assert_eq!(all_lines(&m, SideId::Left), all_lines(&m, SideId::Right));
    }

    #[test]
    fn copy_between_different_encodings_transcodes() {
        let mut u16 = vec![0xFF, 0xFE];
        for u in "keep\r\nold\r\n".encode_utf16() { u16.extend_from_slice(&u.to_le_bytes()); }
        let m = model_cfg(&u16, "keep\nnew é\n".as_bytes(), CompareConfig::default());
        assert_eq!(m.encoding(SideId::Left), TextEncoding::Utf16Le);
        let row = m.next_difference(0).unwrap();
        m.copy_block(SideId::Right, row).unwrap(); // UTF-8 -> UTF-16 side
        assert_eq!(all_lines(&m, SideId::Left), ["keep", "new é"]);
        assert_eq!(m.stats().blocks, 0);
    }

    #[test]
    fn line_endings_and_missing_final_newline_are_preserved_through_edits() {
        let dir = std::env::temp_dir().join(format!("openbc_eol_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let (lp, rp) = (dir.join("l.txt"), dir.join("r.txt"));
        std::fs::write(&lp, "a\r\nb\r\nlast").unwrap(); // CRLF, no trailing newline
        std::fs::write(&rp, "x\n").unwrap();
        let m = TextCompareModel::open(Some(&lp), Some(&rp), CompareConfig::default(), &CancelToken::new(), &mut |_| {}).unwrap();
        m.replace_lines(SideId::Left, 1, 1, &["B"], false).unwrap();           // in place keeps CRLF
        m.replace_lines(SideId::Left, 3, 0, &["appended"], false).unwrap();     // after a no-EOL last line
        m.replace_lines(SideId::Left, 0, 0, &["first"], false).unwrap();        // new lines use the dominant EOL
        m.save(SideId::Left, None).unwrap();
        assert_eq!(std::fs::read(&lp).unwrap(), b"first\r\na\r\nB\r\nlast\r\nappended".to_vec());
        // the last line without terminator is still without one after replacing it
        m.replace_lines(SideId::Left, 4, 1, &["END"], false).unwrap();
        m.save(SideId::Left, None).unwrap();
        assert!(std::fs::read(&lp).unwrap().ends_with(b"\r\nEND"));
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn save_round_trips_utf16_bom_permissions_and_leaves_no_temp_files() {
        let dir = std::env::temp_dir().join(format!("openbc_save_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let (lp, rp) = (dir.join("l.txt"), dir.join("r.txt"));
        let mut u16 = vec![0xFE, 0xFF];                                       // UTF-16 BE with BOM
        for u in "α\nβ\n".encode_utf16() { u16.extend_from_slice(&u.to_be_bytes()); }
        std::fs::write(&lp, &u16).unwrap();
        std::fs::write(&rp, "α\nβ\n").unwrap();
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            std::fs::set_permissions(&lp, std::fs::Permissions::from_mode(0o640)).unwrap();
        }
        for policy in [MapPolicy::Auto, MapPolicy::Buffered] {
            let mut cfg = CompareConfig::default();
            cfg.map_policy = policy;
            let m = TextCompareModel::open(Some(&lp), Some(&rp), cfg, &CancelToken::new(), &mut |_| {}).unwrap();
            m.replace_lines(SideId::Left, 1, 1, &["γ😀"], false).unwrap();
            let report = m.save(SideId::Left, None).unwrap();
            assert!(report.bytes > 0);
            let bytes = std::fs::read(&lp).unwrap();
            assert_eq!(&bytes[..2], [0xFE, 0xFF]);
            let units: Vec<u16> = bytes[2..].chunks_exact(2).map(|p| u16::from_be_bytes([p[0], p[1]])).collect();
            assert_eq!(String::from_utf16(&units).unwrap(), "α\nγ😀\n");
            // reopen: the saved file is a clean, independent file
            let again = TextCompareModel::open(Some(&lp), Some(&rp), CompareConfig::default(), &CancelToken::new(), &mut |_| {}).unwrap();
            assert_eq!(again.line_text(SideId::Left, 1).unwrap(), "γ😀");
            // restore for the next policy
            std::fs::write(&lp, &u16).unwrap();
        }
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(std::fs::metadata(&lp).unwrap().permissions().mode() & 0o777, 0o640);
        }
        let leftovers: Vec<_> = std::fs::read_dir(&dir).unwrap().filter_map(Result::ok)
            .filter(|e| e.file_name().to_string_lossy().contains("openbc-save")).collect();
        assert!(leftovers.is_empty(), "temporary save files must be cleaned up");
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn save_as_assigns_a_path_and_save_without_one_fails_cleanly() {
        let m = model("a\n", "a\n");
        assert!(matches!(m.save(SideId::Left, None), Err(EngineError::Io(_))));
        let dir = std::env::temp_dir().join(format!("openbc_saveas_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let target = dir.join("new.txt");
        m.replace_lines(SideId::Left, 0, 1, &["changed"], false).unwrap();
        m.save(SideId::Left, Some(&target)).unwrap();
        assert_eq!(std::fs::read_to_string(&target).unwrap(), "changed\n");
        assert_eq!(m.path(SideId::Left).as_deref(), Some(target.as_path()));
        assert!(!m.is_dirty(SideId::Left));
        // unwritable location -> error, nothing left behind, model still dirty-free but intact
        m.replace_lines(SideId::Left, 0, 1, &["again"], false).unwrap();
        let bad = dir.join("no-such-dir").join("x.txt");
        assert!(m.save(SideId::Left, Some(&bad)).is_err());
        assert!(m.is_dirty(SideId::Left));
        assert_eq!(m.line_text(SideId::Left, 0).unwrap(), "again");
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn rules_make_lines_match_and_survive_edits_and_undo() {
        let m = model("id=1 ok\n# comment A\nkeep\n", "id=2 ok\n# comment B\nkeep\n");
        assert_eq!(m.stats().replaced_rows, 2);
        m.set_rules("/^#.*$/,1", &CancelToken::new(), &mut |_| {}).unwrap();
        assert_eq!(m.rules_text(), "/^#.*$/,1");
        assert_eq!(m.stats().blocks, 0);                     // masked parts count as matched
        assert_valid_ignoring_rules(&m);
        // edits honour the rules: still matched when only the masked part differs ...
        m.replace_lines(SideId::Right, 0, 1, &["id=99 ok"], false).unwrap();
        assert_eq!(m.stats().blocks, 0);
        // ... and different once the unmasked text differs
        m.replace_lines(SideId::Right, 0, 1, &["id=99 BAD"], false).unwrap();
        assert_eq!(m.stats().replaced_rows, 1);
        // changing the rules re-hashes edited content too; undo keeps working afterwards
        m.set_rules("", &CancelToken::new(), &mut |_| {}).unwrap();
        assert_eq!(m.stats().replaced_rows, 2);
        m.undo(SideId::Right).unwrap();
        m.undo(SideId::Right).unwrap();
        assert_eq!(m.line_text(SideId::Right, 0).unwrap(), "id=2 ok");
        // an invalid regex is rejected and changes nothing
        assert!(m.set_rules("/(bad/", &CancelToken::new(), &mut |_| {}).is_err());
        assert_eq!(m.rules_text(), "");
    }

    fn assert_valid_ignoring_rules(m: &TextCompareModel) {
        let (mut nl, mut nr) = (0, 0);
        for row in 0..m.row_count() {
            let d = m.row(row).unwrap();
            nl += u64::from(d.left_line.is_some());
            nr += u64::from(d.right_line.is_some());
        }
        assert_eq!((nl, nr), (m.line_count(SideId::Left), m.line_count(SideId::Right)));
    }

    #[test]
    fn edits_cost_memory_proportional_to_the_edit_not_the_file() {
        let big: String = (0..200_000).map(|i| format!("line number {i}\n")).collect();
        let m = model(&big, &big);
        let before = m.memory_report();
        for k in 0..500 {
            m.replace_lines(SideId::Left, 100_000, 1, &[&format!("typed {k}")], true).unwrap();
        }
        let after = m.memory_report();
        assert!(after.edit_bytes < 20_000, "edit buffer grew by {}", after.edit_bytes);
        assert!(after.index_resident_bytes <= before.index_resident_bytes + 4096);
        assert_eq!(m.stats().replaced_rows, 1);
        m.undo(SideId::Left).unwrap();
        assert_eq!(m.stats().blocks, 0);
    }

    #[test]
    fn readers_and_a_writer_can_run_concurrently() {
        let text: String = (0..5000).map(|i| format!("line {i}\n")).collect();
        let m = Arc::new(model(&text, &text));
        let mut handles = Vec::new();
        for _ in 0..3 {
            let m = m.clone();
            handles.push(std::thread::spawn(move || {
                for n in 0..3000u64 {
                    let row = n % m.row_count().max(1);
                    if let Some(d) = m.row(row) {
                        if let Some(l) = d.left_line { let _ = m.line_window(SideId::Left, l, 0, 50); }
                        if let Some(r) = d.right_line { let _ = m.line_text(SideId::Right, r); }
                    }
                    let _ = m.inline_diff(row);
                    let _ = m.next_difference(row);
                }
            }));
        }
        for k in 0..200 {
            m.replace_lines(SideId::Right, (k * 7) % 5000, 1, &[&format!("w{k}")], false).unwrap();
        }
        for h in handles { h.join().unwrap(); }
        assert_valid(&m);
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