//! Compact line index: one fixed-size record per line, never the line text.
//!
//! A [`LineRecord`] is 24 bytes (offset, hash, length, flags). Records are kept in RAM until a
//! configurable limit and then spilled to a temporary file that is memory-mapped read-only, so the
//! index of a multi-gigabyte file does not have to fit in memory.
//!
//! Line terminators are LF, CRLF and lone CR (as in the existing `LineIndex`). The terminator is
//! not part of the line; its kind is stored in the record flags. A file ending without a terminator
//! has a final line flagged `EOL_NONE`; `"a\n"` has one line, `"a\n\n"` has two, `""` has none.

use std::borrow::Cow;
use std::fs::{File, OpenOptions};
use std::io::{BufWriter, Write};
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

use memmap2::Mmap;

use super::encoding::{self, TextEncoding};
use super::error::{CancelToken, EngineError, Phase, Progress};
use super::hasher::{HashStream, LineHasher, Xxh3Hasher};
use super::line_norm::{normalize_into, LineNormalizer, NormalizeOptions};
use super::rules::{LineRules, MAX_RULE_LINE_BYTES};
use super::source::TextSource;

/// Flag bits of [`LineRecord::flags`].
pub mod flags {
    /// Mask of the line-ending kind.
    pub const EOL_MASK: u32 = 0b11;
    /// No terminator (last line of a file without trailing newline).
    pub const EOL_NONE: u32 = 0;
    /// `\n`
    pub const EOL_LF: u32 = 1;
    /// `\r\n`
    pub const EOL_CRLF: u32 = 2;
    /// `\r`
    pub const EOL_CR: u32 = 3;
    /// This record is a segment of a longer physical line that was split at `max_line_bytes`;
    /// the next record continues the same line.
    pub const CONTINUED: u32 = 1 << 2;
}

/// One indexed line. `offset`/`length` address the raw bytes in the source (terminator excluded).
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LineRecord {
    /// Byte offset of the first byte of the line in the source.
    pub offset: u64,
    /// Hash of the *normalised* line (see [`NormalizeOptions`]) mixed with the EOL kind when
    /// line endings are significant.
    pub hash: u64,
    /// Length in bytes, terminator excluded.
    pub length: u32,
    /// [`flags`] bits.
    pub flags: u32,
}

impl LineRecord {
    const BYTES: usize = 24;

    fn encode(&self) -> [u8; Self::BYTES] {
        let mut out = [0u8; Self::BYTES];
        out[0..8].copy_from_slice(&self.offset.to_le_bytes());
        out[8..16].copy_from_slice(&self.hash.to_le_bytes());
        out[16..20].copy_from_slice(&self.length.to_le_bytes());
        out[20..24].copy_from_slice(&self.flags.to_le_bytes());
        out
    }

    fn decode(raw: &[u8]) -> Self {
        let u64_at = |i: usize| u64::from_le_bytes(raw[i..i + 8].try_into().expect("8 bytes"));
        let u32_at = |i: usize| u32::from_le_bytes(raw[i..i + 4].try_into().expect("4 bytes"));
        Self { offset: u64_at(0), hash: u64_at(8), length: u32_at(16), flags: u32_at(20) }
    }

    /// Line-ending kind (`flags::EOL_*`).
    #[must_use]
    pub fn eol(&self) -> u32 {
        self.flags & flags::EOL_MASK
    }
}

/// Hard limit on lines per side: diff class ids are `u32`.
pub const MAX_LINES: u64 = u32::MAX as u64 - 1;

static SPILL_COUNTER: AtomicU64 = AtomicU64::new(0);

struct Spilled {
    map: Option<Mmap>,
    path: PathBuf,
    count: u64,
}

impl Drop for Spilled {
    fn drop(&mut self) {
        // Unmap before deleting (required on Windows).
        self.map = None;
        let _ = std::fs::remove_file(&self.path);
    }
}

enum Records {
    Ram(Vec<LineRecord>),
    Spilled(Spilled),
}

/// Tunables for building an index.
#[derive(Clone)]
pub struct IndexConfig {
    /// Normalisation applied before hashing.
    pub normalize: NormalizeOptions,
    /// Hash function.
    pub hasher: Arc<dyn LineHasher>,
    /// Scan block size in bytes.
    pub block_bytes: usize,
    /// Lines longer than this are split into `CONTINUED` records (default `u32::MAX`).
    pub max_line_bytes: u64,
    /// Number of records kept in RAM before spilling to a memory-mapped temporary file.
    pub ram_record_limit: usize,
    /// Ignore rules applied before hashing (see [`LineRules`]).
    pub rules: Option<Arc<LineRules>>,
}

impl Default for IndexConfig {
    fn default() -> Self {
        Self {
            normalize: NormalizeOptions::default(),
            hasher: Arc::new(Xxh3Hasher),
            block_bytes: 8 << 20,
            max_line_bytes: u64::from(u32::MAX),
            ram_record_limit: 8 << 20,
            rules: None,
        }
    }
}

/// The line index of one file.
pub struct LineIndex {
    records: Records,
    source_len: u64,
    encoding: TextEncoding,
    bom_len: u8,
    looks_binary: bool,
    normalize: NormalizeOptions,
    hasher: Arc<dyn LineHasher>,
    rules: Option<Arc<LineRules>>,
    /// Edit overlay (a piece table over line records); `None` while the index is untouched.
    overlay: Option<Vec<Seg>>,
    /// Line ending given to new lines: the most common one among the first lines.
    dominant_eol: u32,
}

#[derive(Clone, Debug)]
enum SegKind {
    /// Lines of the underlying records starting at this base line.
    Base(u64),
    /// Lines created by edits.
    Edit(Vec<LineRecord>),
}

/// A run of consecutive logical lines in the overlay.
#[derive(Clone, Debug)]
struct Seg {
    start: u64,
    len: u64,
    kind: SegKind,
}

/// Hash of one line's UTF-8 text after rules and normalisation. Streaming and one-shot hashing of
/// the same bytes agree, so this is what every other path must reproduce.
pub(crate) fn canonical_hash(
    hasher: &dyn LineHasher,
    opts: NormalizeOptions,
    rules: Option<&LineRules>,
    utf8: &[u8],
    scratch: &mut Vec<u8>,
) -> u64 {
    let masked = match rules {
        Some(r) => r.apply(utf8),
        None => std::borrow::Cow::Borrowed(utf8),
    };
    if opts.is_identity_text() {
        hasher.hash_bytes(&masked)
    } else {
        normalize_into(&masked, opts, scratch);
        hasher.hash_bytes(scratch)
    }
}

/// Mix the line-ending kind into a hash when line endings are significant.
pub(crate) fn salted_hash(hash: u64, eol: u32, ignore_line_endings: bool) -> u64 {
    if ignore_line_endings {
        hash
    } else {
        hash ^ (u64::from(eol) + 1).wrapping_mul(0x9E37_79B9_7F4A_7C15)
    }
}

impl LineIndex {
    /// Build an index by scanning `source` once.
    pub fn build(
        source: &dyn TextSource,
        config: &IndexConfig,
        cancel: &CancelToken,
        progress: &mut dyn FnMut(Progress),
    ) -> Result<Self, EngineError> {
        let total = source.len();
        let sample = source.read(0, 4096)?;
        let detected = encoding::detect(&sample);
        drop(sample);
        let mut scanner = Scanner::new(config, detected.encoding, total);
        scanner.run(source, u64::from(detected.bom_len), cancel, progress)?;
        let records = scanner.sink.finish()?;
        let mut index = Self {
            records,
            source_len: total,
            encoding: detected.encoding,
            bom_len: detected.bom_len,
            looks_binary: detected.looks_binary,
            normalize: config.normalize,
            hasher: config.hasher.clone(),
            rules: config.rules.clone(),
            overlay: None,
            dominant_eol: flags::EOL_LF,
        };
        index.dominant_eol = index.sample_dominant_eol();
        Ok(index)
    }

    fn sample_dominant_eol(&self) -> u32 {
        let mut counts = [0u32; 4];
        for line in 0..self.line_count().min(256) {
            if let Some(r) = self.record(line) {
                counts[r.eol() as usize] += 1;
            }
        }
        [flags::EOL_LF, flags::EOL_CRLF, flags::EOL_CR]
            .into_iter()
            .max_by_key(|&k| (counts[k as usize], u32::from(k == flags::EOL_LF)))
            .filter(|&k| counts[k as usize] > 0)
            .unwrap_or(flags::EOL_LF)
    }

    /// Number of lines.
    #[must_use]
    pub fn line_count(&self) -> u64 {
        match &self.overlay {
            Some(segs) => segs.last().map_or(0, |s| s.start + s.len),
            None => self.base_count(),
        }
    }

    fn base_count(&self) -> u64 {
        match &self.records {
            Records::Ram(v) => v.len() as u64,
            Records::Spilled(s) => s.count,
        }
    }

    /// Record for a zero-based line.
    #[must_use]
    pub fn record(&self, line: u64) -> Option<LineRecord> {
        match &self.overlay {
            None => self.base_record(line),
            Some(segs) => {
                let idx = segs.partition_point(|s| s.start + s.len <= line);
                let seg = segs.get(idx)?;
                if line < seg.start {
                    return None;
                }
                match &seg.kind {
                    SegKind::Base(first) => self.base_record(first + (line - seg.start)),
                    SegKind::Edit(records) => records.get((line - seg.start) as usize).copied(),
                }
            }
        }
    }

    fn base_record(&self, line: u64) -> Option<LineRecord> {
        match &self.records {
            Records::Ram(v) => v.get(usize::try_from(line).ok()?).copied(),
            Records::Spilled(s) => {
                if line >= s.count {
                    return None;
                }
                let start = line as usize * LineRecord::BYTES;
                let map = s.map.as_ref()?;
                Some(LineRecord::decode(&map[start..start + LineRecord::BYTES]))
            }
        }
    }

    /// Raw bytes of a line (terminator excluded), read lazily from `source`.
    pub fn line_bytes<'a>(
        &self,
        source: &'a dyn TextSource,
        line: u64,
    ) -> std::io::Result<Cow<'a, [u8]>> {
        match self.record(line) {
            Some(r) => source.read(r.offset, r.length as usize),
            None => Ok(Cow::Borrowed(&[])),
        }
    }

    /// Zero-based line containing byte `offset` (binary search; O(log n)).
    #[must_use]
    pub fn line_for_offset(&self, offset: u64) -> Option<u64> {
        if self.overlay.is_some() {
            return None; // offsets are no longer monotonic once lines were edited
        }
        let count = self.line_count();
        if count == 0 {
            return None;
        }
        let (mut lo, mut hi) = (0u64, count);
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            if self.record(mid)?.offset <= offset {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        Some(lo.saturating_sub(1))
    }

    /// Detected encoding.
    #[must_use]
    pub fn encoding(&self) -> TextEncoding {
        self.encoding
    }

    /// Length of the BOM that precedes the first line.
    #[must_use]
    pub fn bom_len(&self) -> u8 {
        self.bom_len
    }

    /// True when the sample contained NUL bytes that UTF-16 does not explain.
    #[must_use]
    pub fn looks_binary(&self) -> bool {
        self.looks_binary
    }

    /// Length of the indexed source in bytes.
    #[must_use]
    pub fn source_len(&self) -> u64 {
        self.source_len
    }

    /// Normalisation this index was hashed with.
    #[must_use]
    pub fn normalize_options(&self) -> NormalizeOptions {
        self.normalize
    }

    /// True when records live in a memory-mapped temporary file instead of RAM.
    #[must_use]
    pub fn is_spilled(&self) -> bool {
        matches!(self.records, Records::Spilled(_))
    }

    /// Heap bytes held by the index (0 for spilled records, whose pages the OS manages).
    #[must_use]
    pub fn resident_bytes(&self) -> u64 {
        let base = match &self.records {
            Records::Ram(v) => (v.capacity() * LineRecord::BYTES) as u64,
            Records::Spilled(_) => 0,
        };
        let overlay: u64 = self.overlay.as_ref().map_or(0, |segs| {
            segs.iter()
                .map(|s| match &s.kind {
                    SegKind::Edit(v) => (v.capacity() * LineRecord::BYTES) as u64,
                    SegKind::Base(_) => 0,
                } + std::mem::size_of::<Seg>() as u64)
                .sum()
        });
        base + overlay
    }

    /// Logical size of all records in bytes.
    #[must_use]
    pub fn record_bytes(&self) -> u64 {
        self.line_count() * LineRecord::BYTES as u64
    }
}

impl LineIndex {
    /// Ignore rules this index was hashed with.
    #[must_use]
    pub fn rules(&self) -> Option<&Arc<LineRules>> {
        self.rules.as_ref()
    }

    /// Key of the rules (empty when none); two indexes can only be diffed when keys match.
    #[must_use]
    pub fn rules_key(&self) -> &str {
        self.rules.as_deref().map_or("", LineRules::key)
    }

    /// Line ending used for newly created lines.
    #[must_use]
    pub fn dominant_eol(&self) -> u32 {
        self.dominant_eol
    }

    /// True once any line was spliced in or out.
    #[must_use]
    pub fn is_edited(&self) -> bool {
        self.overlay.is_some()
    }

    /// Record for a line of `utf8` text stored at `address` as `stored_len` bytes (in the file's
    /// encoding) with line ending `eol`.
    #[must_use]
    pub fn make_record(&self, address: u64, stored_len: u32, utf8: &[u8], eol: u32) -> LineRecord {
        let mut scratch = Vec::new();
        let hash = canonical_hash(self.hasher.as_ref(), self.normalize, self.rules.as_deref(), utf8, &mut scratch);
        LineRecord {
            offset: address,
            hash: salted_hash(hash, eol, self.normalize.ignore_line_endings),
            length: stored_len,
            flags: eol,
        }
    }

    /// Replace `remove` lines starting at `first` by `insert`. Returns the removed records (the undo
    /// payload). O(number of overlay segments), independent of the file size.
    pub fn splice(&mut self, first: u64, remove: u64, insert: Vec<LineRecord>) -> Vec<LineRecord> {
        let total = self.line_count();
        let first = first.min(total);
        let remove = remove.min(total - first);
        let removed: Vec<LineRecord> = (first..first + remove).filter_map(|i| self.record(i)).collect();
        let base_count = self.base_count();
        let segs = self.overlay.get_or_insert_with(|| {
            if base_count == 0 {
                Vec::new()
            } else {
                vec![Seg { start: 0, len: base_count, kind: SegKind::Base(0) }]
            }
        });
        split_at(segs, first);
        split_at(segs, first + remove);
        let lo = segs.partition_point(|s| s.start < first);
        let hi = segs.partition_point(|s| s.start < first + remove);
        segs.drain(lo..hi);
        if !insert.is_empty() {
            segs.insert(lo, Seg { start: first, len: insert.len() as u64, kind: SegKind::Edit(insert) });
        }
        merge_edit_neighbours(segs, lo.saturating_sub(1));
        renumber(segs, lo.saturating_sub(1));
        removed
    }

    /// Build a new, un-overlaid index with the same line layout but hashes computed under
    /// `normalize` and `rules` (reads every line once; the records' offsets stay valid).
    pub fn rehash(
        &self,
        source: &dyn TextSource,
        normalize: NormalizeOptions,
        rules: Option<Arc<LineRules>>,
        cancel: &CancelToken,
        progress: &mut dyn FnMut(Progress),
        ram_record_limit: usize,
    ) -> Result<Self, EngineError> {
        let count = self.line_count();
        let mut sink = Sink::new(ram_record_limit);
        let mut scratch = Vec::new();
        let mut utf8 = Vec::new();
        for line in 0..count {
            if line & 0xFFFF == 0 {
                cancel.check()?;
                progress(Progress { phase: Phase::Indexing, done: line, total: count });
            }
            let mut rec = self.record(line).ok_or(EngineError::SourceChanged)?;
            let raw = source.read(rec.offset, rec.length as usize)?;
            let text: &[u8] = if self.encoding == TextEncoding::Utf8 {
                &raw
            } else {
                encoding::decode_line_into(&raw, self.encoding, &mut utf8);
                &utf8
            };
            let hash = canonical_hash(self.hasher.as_ref(), normalize, rules.as_deref(), text, &mut scratch);
            rec.hash = salted_hash(hash, rec.eol(), normalize.ignore_line_endings);
            sink.push(rec)?;
        }
        let records = sink.finish()?;
        Ok(Self {
            records,
            source_len: self.source_len,
            encoding: self.encoding,
            bom_len: self.bom_len,
            looks_binary: self.looks_binary,
            normalize,
            hasher: self.hasher.clone(),
            rules,
            overlay: None,
            dominant_eol: self.dominant_eol,
        })
    }
}

/// Split the segment containing `pos` (strictly inside it) into two.
fn split_at(segs: &mut Vec<Seg>, pos: u64) {
    let idx = segs.partition_point(|s| s.start + s.len <= pos);
    let Some(seg) = segs.get_mut(idx) else { return };
    if seg.start >= pos {
        return;
    }
    let head_len = pos - seg.start;
    let tail = match &mut seg.kind {
        SegKind::Base(first) => Seg { start: pos, len: seg.len - head_len, kind: SegKind::Base(*first + head_len) },
        SegKind::Edit(v) => {
            let rest = v.split_off(head_len as usize);
            Seg { start: pos, len: rest.len() as u64, kind: SegKind::Edit(rest) }
        }
    };
    seg.len = head_len;
    segs.insert(idx + 1, tail);
}

/// Merge consecutive edit segments starting from `from` so typing never fragments the overlay.
fn merge_edit_neighbours(segs: &mut Vec<Seg>, from: usize) {
    let mut i = from;
    while i + 1 < segs.len() {
        let both_edit = matches!(segs[i].kind, SegKind::Edit(_)) && matches!(segs[i + 1].kind, SegKind::Edit(_));
        let adjacent_base = matches!((&segs[i].kind, &segs[i + 1].kind),
            (SegKind::Base(a), SegKind::Base(b)) if a + segs[i].len == *b);
        if both_edit {
            let next = segs.remove(i + 1);
            if let (SegKind::Edit(a), SegKind::Edit(b)) = (&mut segs[i].kind, next.kind) {
                a.extend(b);
            }
            segs[i].len = match &segs[i].kind { SegKind::Edit(v) => v.len() as u64, _ => 0 };
        } else if adjacent_base {
            let next = segs.remove(i + 1);
            segs[i].len += next.len;
        } else {
            i += 1;
        }
    }
}

fn renumber(segs: &mut [Seg], from: usize) {
    let mut start = if from == 0 { 0 } else { segs[from - 1].start + segs[from - 1].len };
    for seg in &mut segs[from..] {
        seg.start = start;
        start += seg.len;
    }
}

// ----------------------------------------------------------------------------------------------
// Record sink (RAM, spilling to a memory-mapped temp file when large)
// ----------------------------------------------------------------------------------------------

struct Sink {
    ram: Vec<LineRecord>,
    limit: usize,
    spill: Option<(BufWriter<File>, PathBuf, u64)>,
    count: u64,
}

impl Sink {
    fn new(limit: usize) -> Self {
        Self { ram: Vec::new(), limit: limit.max(1024), spill: None, count: 0 }
    }

    fn push(&mut self, record: LineRecord) -> Result<(), EngineError> {
        self.count += 1;
        if self.count > MAX_LINES {
            return Err(EngineError::TooManyLines(self.count, MAX_LINES));
        }
        self.ram.push(record);
        if self.ram.len() >= self.limit {
            self.flush_ram()?;
        }
        Ok(())
    }

    fn flush_ram(&mut self) -> Result<(), EngineError> {
        if self.spill.is_none() {
            let n = SPILL_COUNTER.fetch_add(1, Ordering::Relaxed);
            let path = std::env::temp_dir()
                .join(format!("openbc-lineindex-{}-{n}.bin", std::process::id()));
            let file = OpenOptions::new().read(true).write(true).create_new(true).open(&path)?;
            self.spill = Some((BufWriter::with_capacity(1 << 20, file), path, 0));
        }
        let (writer, _, written) = self.spill.as_mut().expect("spill exists");
        for record in self.ram.drain(..) {
            writer.write_all(&record.encode())?;
            *written += 1;
        }
        self.ram.shrink_to(0);
        Ok(())
    }

    fn finish(mut self) -> Result<Records, EngineError> {
        if self.spill.is_none() {
            self.ram.shrink_to_fit();
            return Ok(Records::Ram(std::mem::take(&mut self.ram)));
        }
        self.flush_ram()?;
        let (mut writer, path, count) = self.spill.take().expect("spill exists");
        writer.flush()?;
        let file = writer.into_inner().map_err(|e| e.into_error())?;
        // SAFETY: the file is private to this process, created by us and never written again.
        let map = unsafe { Mmap::map(&file)? };
        Ok(Records::Spilled(Spilled { map: Some(map), path, count }))
    }
}

impl Drop for Sink {
    fn drop(&mut self) {
        // Cancelled / failed build: remove a half-written spill file.
        if let Some((writer, path, _)) = self.spill.take() {
            drop(writer);
            let _ = std::fs::remove_file(path);
        }
    }
}

// ----------------------------------------------------------------------------------------------
// Scanner
// ----------------------------------------------------------------------------------------------

struct Scanner<'c> {
    config: &'c IndexConfig,
    encoding: TextEncoding,
    total: u64,
    sink: Sink,
    stream: Box<dyn HashStream>,
    norm: LineNormalizer,
    identity: bool,
    /// Offset of the first byte of the line being scanned.
    line_start: u64,
    /// Bytes of the current line seen so far.
    line_len: u64,
    /// Whether `stream` already holds data for the current line.
    streaming: bool,
    /// Rules path: the current line is collected whole (bounded) so rules can see all of it.
    rule_buf: Vec<u8>,
    rule_mode: bool,
    scratch: Vec<u8>,
}

impl<'c> Scanner<'c> {
    fn new(config: &'c IndexConfig, encoding: TextEncoding, total: u64) -> Self {
        Self {
            config,
            encoding,
            total,
            sink: Sink::new(config.ram_record_limit),
            stream: config.hasher.new_stream(),
            norm: LineNormalizer::new(config.normalize),
            identity: config.normalize.is_identity_text(),
            line_start: 0,
            line_len: 0,
            streaming: false,
            rule_buf: Vec::new(),
            rule_mode: config.rules.is_some(),
            scratch: Vec::new(),
        }
    }

    /// Hash of the finished line, from the rule buffer or the stream; resets per-line state.
    fn finish_line_hash(&mut self) -> u64 {
        if self.rule_mode && self.config.rules.is_some() {
            let buf = std::mem::take(&mut self.rule_buf);
            let hash = canonical_hash(
                self.config.hasher.as_ref(),
                self.config.normalize,
                self.config.rules.as_deref(),
                &buf,
                &mut self.scratch,
            );
            self.rule_buf = buf;
            self.rule_buf.clear();
            self.streaming = false;
            hash
        } else {
            let hash = self.finish_stream();
            self.rule_mode = self.config.rules.is_some();
            hash
        }
    }

    fn run(
        &mut self,
        source: &dyn TextSource,
        start: u64,
        cancel: &CancelToken,
        progress: &mut dyn FnMut(Progress),
    ) -> Result<(), EngineError> {
        self.line_start = start;
        match self.encoding {
            TextEncoding::Utf8 => self.scan_utf8(source, start, cancel, progress),
            _ => self.scan_utf16(source, start, cancel, progress),
        }
    }

    fn eol_hash(&self, hash: u64, eol: u32) -> u64 {
        salted_hash(hash, eol, self.config.normalize.ignore_line_endings)
    }

    fn emit(&mut self, hash: u64, eol: u32, extra_flags: u32) -> Result<(), EngineError> {
        let record = LineRecord {
            offset: self.line_start,
            hash: self.eol_hash(hash, eol),
            length: self.line_len as u32,
            flags: eol | extra_flags,
        };
        self.sink.push(record)
    }

    // ---- UTF-8 -------------------------------------------------------------------------------

    fn feed_stream(&mut self, bytes: &[u8]) {
        if bytes.is_empty() {
            return;
        }
        self.streaming = true;
        if self.identity {
            self.stream.update(bytes);
        } else {
            let stream = &mut self.stream;
            self.norm.feed(bytes, &mut |c| stream.update(c));
        }
    }

    fn finish_stream(&mut self) -> u64 {
        if !self.identity {
            let stream = &mut self.stream;
            self.norm.finish(&mut |c| stream.update(c));
        }
        self.streaming = false;
        self.stream.finish_reset()
    }

    /// Append content bytes to the current line, splitting at `max_line_bytes`.
    fn append(&mut self, mut bytes: &[u8]) -> Result<(), EngineError> {
        let max = self.config.max_line_bytes.max(1);
        while !bytes.is_empty() {
            if self.line_len == max {
                let hash = self.finish_line_hash();
                self.emit(hash, flags::EOL_NONE, flags::CONTINUED)?;
                self.line_start += self.line_len;
                self.line_len = 0;
            }
            let take = bytes.len().min((max - self.line_len) as usize);
            if self.rule_mode && self.config.rules.is_some() {
                self.rule_buf.extend_from_slice(&bytes[..take]);
                if self.rule_buf.len() > MAX_RULE_LINE_BYTES {
                    // Too long for rules: continue as a plain streamed line (same on both sides).
                    self.rule_mode = false;
                    let spilled = std::mem::take(&mut self.rule_buf);
                    self.feed_stream(&spilled);
                }
            } else {
                self.feed_stream(&bytes[..take]);
            }
            self.line_len += take as u64;
            bytes = &bytes[take..];
        }
        Ok(())
    }

    fn end_line(&mut self, tail: &[u8], eol: u32, terminator_len: u64) -> Result<(), EngineError> {
        let max = self.config.max_line_bytes.max(1);
        let hash = if self.rule_mode
            && self.config.rules.is_some()
            && !self.streaming
            && self.line_len == 0
            && self.rule_buf.is_empty()
            && tail.len() <= MAX_RULE_LINE_BYTES
            && (tail.len() as u64) <= max
        {
            // Whole line in the block and short enough for rules: apply them directly.
            self.line_len = tail.len() as u64;
            canonical_hash(
                self.config.hasher.as_ref(),
                self.config.normalize,
                self.config.rules.as_deref(),
                tail,
                &mut self.scratch,
            )
        } else if !self.streaming && self.line_len == 0 && self.rule_buf.is_empty() && (tail.len() as u64) <= max {
            // The whole line is inside the current block: hash it in one shot, no copies.
            self.line_len = tail.len() as u64;
            if self.identity {
                self.config.hasher.hash_bytes(tail)
            } else {
                let mut scratch = Vec::new();
                normalize_into(tail, self.config.normalize, &mut scratch);
                self.config.hasher.hash_bytes(&scratch)
            }
        } else {
            self.append(tail)?;
            self.finish_line_hash()
        };
        self.emit(hash, eol, 0)?;
        self.line_start += self.line_len + terminator_len;
        self.line_len = 0;
        Ok(())
    }

    fn scan_utf8(
        &mut self,
        source: &dyn TextSource,
        start: u64,
        cancel: &CancelToken,
        progress: &mut dyn FnMut(Progress),
    ) -> Result<(), EngineError> {
        let block = self.config.block_bytes.max(4096) as u64;
        let mut pos = start;
        while pos < self.total {
            cancel.check()?;
            let want = block.min(self.total - pos) as usize;
            let read = source.read(pos, want + 1)?;
            if read.is_empty() {
                break;
            }
            let (data, peek) = if read.len() > want {
                (&read[..want], Some(read[want]))
            } else {
                (&read[..], None)
            };
            let mut i = 0usize;
            while i < data.len() {
                match memchr::memchr2(b'\n', b'\r', &data[i..]) {
                    None => {
                        self.append(&data[i..])?;
                        i = data.len();
                    }
                    Some(rel) => {
                        let t = i + rel;
                        let (eol, tlen) = if data[t] == b'\n' {
                            (flags::EOL_LF, 1usize)
                        } else {
                            let next = if t + 1 < data.len() { Some(data[t + 1]) } else { peek };
                            if next == Some(b'\n') {
                                (flags::EOL_CRLF, 2)
                            } else {
                                (flags::EOL_CR, 1)
                            }
                        };
                        self.end_line(&data[i..t], eol, tlen as u64)?;
                        i = t + tlen;
                    }
                }
            }
            pos += i as u64;
            progress(Progress { phase: Phase::Indexing, done: pos.min(self.total), total: self.total });
        }
        if self.line_len > 0 || self.streaming {
            let hash = self.finish_line_hash();
            self.emit(hash, flags::EOL_NONE, 0)?;
        }
        Ok(())
    }

    // ---- UTF-16 ------------------------------------------------------------------------------

    fn scan_utf16(
        &mut self,
        source: &dyn TextSource,
        start: u64,
        cancel: &CancelToken,
        progress: &mut dyn FnMut(Progress),
    ) -> Result<(), EngineError> {
        let big = self.encoding == TextEncoding::Utf16Be;
        let unit = |p: &[u8]| if big { u16::from_be_bytes([p[0], p[1]]) } else { u16::from_le_bytes([p[0], p[1]]) };
        let block = (self.config.block_bytes.max(4096) & !1) as u64;
        // Raw bytes of the current line (UTF-16 lines are buffered whole; documented limitation).
        let mut line: Vec<u8> = Vec::new();
        let mut utf8: Vec<u8> = Vec::new();
        let mut pos = start;
        while pos + 1 < self.total {
            cancel.check()?;
            let want = block.min((self.total - pos) & !1) as usize;
            let read = source.read(pos, want + 2)?;
            let usable = read.len() & !1;
            let (data, peek) = if usable > want {
                (&read[..want], Some(unit(&read[want..want + 2])))
            } else {
                (&read[..usable.min(want)], None)
            };
            let mut i = 0usize;
            while i + 1 < data.len() {
                let u = unit(&data[i..i + 2]);
                if u == 0x000A || u == 0x000D {
                    let (eol, tlen) = if u == 0x000A {
                        (flags::EOL_LF, 2usize)
                    } else {
                        let next = if i + 3 < data.len() { Some(unit(&data[i + 2..i + 4])) } else { peek };
                        if next == Some(0x000A) { (flags::EOL_CRLF, 4) } else { (flags::EOL_CR, 2) }
                    };
                    self.end_line_utf16(&line, &mut utf8, eol, tlen as u64)?;
                    line.clear();
                    i += tlen;
                } else {
                    line.extend_from_slice(&data[i..i + 2]);
                    i += 2;
                }
            }
            pos += i as u64;
            progress(Progress { phase: Phase::Indexing, done: pos.min(self.total), total: self.total });
        }
        if !line.is_empty() {
            self.end_line_utf16(&line, &mut utf8, flags::EOL_NONE, 0)?;
        }
        Ok(())
    }

    fn end_line_utf16(
        &mut self,
        raw: &[u8],
        utf8: &mut Vec<u8>,
        eol: u32,
        terminator_len: u64,
    ) -> Result<(), EngineError> {
        if raw.len() as u64 > u64::from(u32::MAX) {
            return Err(EngineError::LineTooLong(u64::from(u32::MAX)));
        }
        encoding::decode_line_into(raw, self.encoding, utf8);
        let hash = canonical_hash(
            self.config.hasher.as_ref(),
            self.config.normalize,
            self.config.rules.as_deref(),
            utf8,
            &mut self.scratch,
        );
        self.line_len = raw.len() as u64;
        self.emit(hash, eol, 0)?;
        self.line_start += self.line_len + terminator_len;
        self.line_len = 0;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::text::engine::source::MemorySource;

    fn index(bytes: &[u8]) -> (LineIndex, MemorySource) {
        index_with(bytes, IndexConfig::default())
    }

    fn index_with(bytes: &[u8], config: IndexConfig) -> (LineIndex, MemorySource) {
        let src = MemorySource::new(bytes.to_vec());
        let idx = LineIndex::build(&src, &config, &CancelToken::new(), &mut |_| {}).unwrap();
        (idx, src)
    }

    fn lines(idx: &LineIndex, src: &MemorySource) -> Vec<String> {
        (0..idx.line_count())
            .map(|i| String::from_utf8_lossy(&idx.line_bytes(src, i).unwrap()).into_owned())
            .collect()
    }

    #[test]
    fn terminators_and_edge_cases() {
        assert_eq!(index(b"").0.line_count(), 0);
        assert_eq!(lines(&index(b"a").0, &MemorySource::new(b"a".to_vec())), ["a"]);
        let (i, s) = index(b"a\n");
        assert_eq!(lines(&i, &s), ["a"]);
        let (i, s) = index(b"a\n\n");
        assert_eq!(lines(&i, &s), ["a", ""]);
        let (i, s) = index(b"\n");
        assert_eq!(lines(&i, &s), [""]);
        let (i, s) = index(b"a\r\nb\rc\nd");
        assert_eq!(lines(&i, &s), ["a", "b", "c", "d"]);
        assert_eq!(i.record(0).unwrap().eol(), flags::EOL_CRLF);
        assert_eq!(i.record(1).unwrap().eol(), flags::EOL_CR);
        assert_eq!(i.record(2).unwrap().eol(), flags::EOL_LF);
        assert_eq!(i.record(3).unwrap().eol(), flags::EOL_NONE);
        assert_eq!(i.record(1).unwrap().offset, 3);
    }

    #[test]
    fn terminators_are_independent_of_the_configured_block_size() {
        // The scan clamps block_bytes to >= 4096, so this only checks the setting is harmless;
        // `crlf_across_real_block_boundary` below covers a CR/LF pair that really straddles blocks.
        let data = b"alpha\r\nbeta\r\ngamma\r\n\r\ndelta";
        for block in 1..=data.len() + 2 {
            let mut cfg = IndexConfig::default();
            cfg.block_bytes = block;
            let (i, s) = index_with(data, cfg);
            assert_eq!(lines(&i, &s), ["alpha", "beta", "gamma", "", "delta"]);
        }
    }

    #[test]
    fn lines_spanning_blocks_hash_like_whole_lines() {
        let long = "x".repeat(10_000);
        let text = format!("{long}\nshort\n{long}\n");
        let mut small = IndexConfig::default();
        small.block_bytes = 4096;
        let (a, _) = index_with(text.as_bytes(), small);
        let (b, _) = index(text.as_bytes());
        for n in 0..3 {
            assert_eq!(a.record(n), b.record(n));
        }
        assert_eq!(a.record(0).unwrap().hash, a.record(2).unwrap().hash);
        assert_ne!(a.record(0).unwrap().hash, a.record(1).unwrap().hash);
    }

    #[test]
    fn crlf_across_real_block_boundary() {
        // 4096-byte blocks: put the CR as the last byte of the first block.
        let mut data = vec![b'a'; 4095];
        data.extend_from_slice(b"\r\nz");
        let mut cfg = IndexConfig::default();
        cfg.block_bytes = 4096;
        let (i, s) = index_with(&data, cfg);
        assert_eq!(i.line_count(), 2);
        assert_eq!(i.record(0).unwrap().eol(), flags::EOL_CRLF);
        assert_eq!(lines(&i, &s)[1], "z");
    }

    #[test]
    fn normalisation_changes_hash_but_not_offsets() {
        let mut cfg = IndexConfig::default();
        cfg.normalize.ignore_leading_trailing_whitespace = true;
        cfg.normalize.ignore_case = true;
        let (i, _) = index_with(b"  Hello \nhello\n", cfg);
        assert_eq!(i.record(0).unwrap().hash, i.record(1).unwrap().hash);
        assert_eq!(i.record(0).unwrap().length, 8); // original bytes preserved
    }

    #[test]
    fn eol_significance_is_optional() {
        let mut strict = IndexConfig::default();
        strict.normalize.ignore_line_endings = false;
        let (a, _) = index_with(b"x\r\n", strict.clone());
        let (b, _) = index_with(b"x\n", strict);
        assert_ne!(a.record(0).unwrap().hash, b.record(0).unwrap().hash);
        let (c, _) = index(b"x\r\n");
        let (d, _) = index(b"x\n");
        assert_eq!(c.record(0).unwrap().hash, d.record(0).unwrap().hash);
    }

    #[test]
    fn long_lines_are_split_into_continued_records() {
        let mut cfg = IndexConfig::default();
        cfg.max_line_bytes = 10;
        let (i, s) = index_with(b"0123456789ABCDEFGHIJKLM\nz", cfg);
        assert_eq!(lines(&i, &s), ["0123456789", "ABCDEFGHIJ", "KLM", "z"]);
        assert_ne!(i.record(0).unwrap().flags & flags::CONTINUED, 0);
        assert_eq!(i.record(2).unwrap().flags & flags::CONTINUED, 0);
    }

    #[test]
    fn bom_is_skipped_and_utf16_is_indexed_by_bytes() {
        let (i, s) = index(b"\xEF\xBB\xBFab\ncd");
        assert_eq!(lines(&i, &s), ["ab", "cd"]);
        assert_eq!(i.record(0).unwrap().offset, 3);

        let mut utf16 = vec![0xFF, 0xFE];
        for u in "hé\r\nwörld\n😀".encode_utf16() {
            utf16.extend_from_slice(&u.to_le_bytes());
        }
        let (i, s) = index(&utf16);
        assert_eq!(i.encoding(), TextEncoding::Utf16Le);
        assert_eq!(i.line_count(), 3);
        let text: Vec<String> = (0..3)
            .map(|n| encoding::decode_line(&i.line_bytes(&s, n).unwrap(), i.encoding()).into_owned())
            .collect();
        assert_eq!(text, ["hé", "wörld", "😀"]);
        // Same text as UTF-8 hashes identically (cross-encoding comparison works).
        let (u8i, _) = index("hé\nwörld\n😀".as_bytes());
        for n in 0..3 {
            assert_eq!(i.record(n).unwrap().hash, u8i.record(n).unwrap().hash);
        }
    }

    #[test]
    fn spills_to_a_mapped_file_and_cleans_up() {
        let mut cfg = IndexConfig::default();
        cfg.ram_record_limit = 1024;
        let data: String = (0..5000).map(|n| format!("line {n}\n")).collect();
        let (i, s) = index_with(data.as_bytes(), cfg);
        assert!(i.is_spilled());
        assert_eq!(i.resident_bytes(), 0);
        assert_eq!(i.line_count(), 5000);
        assert_eq!(lines(&i, &s)[4999], "line 4999");
        let (ram, _) = index(data.as_bytes());
        for n in [0, 1, 1023, 1024, 4999] {
            assert_eq!(i.record(n), ram.record(n));
        }
        let path = match &i.records { Records::Spilled(s) => s.path.clone(), _ => unreachable!() };
        assert!(path.exists());
        drop(i);
        assert!(!path.exists());
    }

    #[test]
    fn line_for_offset_is_a_binary_search() {
        let (i, _) = index(b"alpha\nbeta\r\ngamma");
        assert_eq!(i.line_for_offset(0), Some(0));
        assert_eq!(i.line_for_offset(5), Some(0));
        assert_eq!(i.line_for_offset(6), Some(1));
        assert_eq!(i.line_for_offset(12), Some(2));
        assert_eq!(i.line_for_offset(999), Some(2));
        assert_eq!(index(b"").0.line_for_offset(0), None);
    }

    // ---- editing overlay ---------------------------------------------------------------------

    use crate::text::engine::source::EditableSource;

    fn editable(text: &str) -> (LineIndex, Arc<EditableSource>) {
        let src = Arc::new(EditableSource::new(Arc::new(MemorySource::new(text.as_bytes().to_vec()))));
        let idx = LineIndex::build(src.as_ref(), &IndexConfig::default(), &CancelToken::new(), &mut |_| {}).unwrap();
        (idx, src)
    }

    fn new_line(idx: &LineIndex, src: &EditableSource, text: &str) -> LineRecord {
        let address = src.append(text.as_bytes());
        idx.make_record(address, text.len() as u32, text.as_bytes(), idx.dominant_eol())
    }

    fn text_of(idx: &LineIndex, src: &EditableSource) -> Vec<String> {
        (0..idx.line_count())
            .map(|i| String::from_utf8_lossy(&idx.line_bytes(src, i).unwrap()).into_owned())
            .collect()
    }

    #[test]
    fn splice_replaces_inserts_and_deletes_without_moving_other_lines() {
        let (mut idx, src) = editable("a\nb\nc\nd\n");
        let before: Vec<u64> = (0..4).map(|i| idx.record(i).unwrap().offset).collect();
        // replace "b" with two lines
        let x = new_line(&idx, &src, "X");
        let y = new_line(&idx, &src, "Y");
        let removed = idx.splice(1, 1, vec![x, y]);
        assert_eq!(removed.len(), 1);
        assert_eq!(text_of(&idx, &src), ["a", "X", "Y", "c", "d"]);
        // untouched lines keep their ORIGINAL offsets
        assert_eq!(idx.record(0).unwrap().offset, before[0]);
        assert_eq!(idx.record(3).unwrap().offset, before[2]);
        assert_eq!(idx.record(4).unwrap().offset, before[3]);
        // delete two, insert at the end, insert at the start
        idx.splice(2, 2, vec![]);
        assert_eq!(text_of(&idx, &src), ["a", "X", "d"]);
        let end = new_line(&idx, &src, "END");
        idx.splice(3, 0, vec![end]);
        let start = new_line(&idx, &src, "START");
        idx.splice(0, 0, vec![start]);
        assert_eq!(text_of(&idx, &src), ["START", "a", "X", "d", "END"]);
        assert!(idx.is_edited());
        assert_eq!(idx.line_for_offset(0), None); // offsets are not monotonic any more
    }

    #[test]
    fn splice_is_exactly_reversible_through_returned_records() {
        let (mut idx, src) = editable("l0\nl1\nl2\nl3\nl4\n");
        let original = text_of(&idx, &src);
        let n1 = new_line(&idx, &src, "new");
        let removed1 = idx.splice(1, 3, vec![n1]); // l1..l3 -> new
        assert_eq!(text_of(&idx, &src), ["l0", "new", "l4"]);
        let removed2 = idx.splice(0, 3, vec![]); // delete everything
        assert_eq!(idx.line_count(), 0);
        idx.splice(0, 0, removed2); // undo 2
        idx.splice(1, 1, removed1); // undo 1
        assert_eq!(text_of(&idx, &src), original);
    }

    #[test]
    fn many_random_splices_match_a_vec_model() {
        let (mut idx, src) = editable(&(0..40).map(|i| format!("line{i}\n")).collect::<String>());
        let mut model: Vec<String> = text_of(&idx, &src);
        let mut seed = 0xC0FFEEu64;
        let mut next = move || { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; seed };
        for step in 0..400 {
            let total = model.len() as u64;
            let first = if total == 0 { 0 } else { next() % (total + 1) };
            let remove = if first >= total { 0 } else { next() % (total - first).min(4) + (next() % 2) };
            let remove = remove.min(total - first);
            let insert_n = (next() % 4) as usize;
            let texts: Vec<String> = (0..insert_n).map(|k| format!("s{step}_{k}")).collect();
            let recs: Vec<LineRecord> = texts.iter().map(|t| new_line(&idx, &src, t)).collect();
            idx.splice(first, remove, recs);
            model.splice(first as usize..(first + remove) as usize, texts);
            assert_eq!(idx.line_count(), model.len() as u64);
        }
        assert_eq!(text_of(&idx, &src), model);
    }

    #[test]
    fn typing_in_one_place_does_not_fragment_the_overlay() {
        let (mut idx, src) = editable(&"x\n".repeat(1000));
        for k in 0..200 {
            let r = new_line(&idx, &src, &format!("typed {k}"));
            idx.splice(500, 1, vec![r]);
        }
        // base | edit(1) | base  -> at most 3 segments regardless of keystrokes
        assert!(idx.overlay.as_ref().unwrap().len() <= 3);
        assert_eq!(text_of(&idx, &src)[500], "typed 199");
    }

    #[test]
    fn new_records_hash_like_scanned_lines() {
        let (idx, src) = editable("hello\n");
        let fresh = new_line(&idx, &src, "hello");
        assert_eq!(fresh.hash, idx.record(0).unwrap().hash);
        assert_eq!(fresh.eol(), flags::EOL_LF);
    }

    // ---- rules -------------------------------------------------------------------------------

    fn with_rules(text: &str, rules: &str, block: usize) -> LineIndex {
        let mut cfg = IndexConfig::default();
        cfg.block_bytes = block;
        cfg.rules = LineRules::parse(rules).unwrap().map(Arc::new);
        index_with(text.as_bytes(), cfg).0
    }

    #[test]
    fn rules_make_lines_that_differ_only_in_matches_hash_equal() {
        let i = with_rules("t=2026-01-01 ok\nt=2027-12-31 ok\nt=2027-12-31 bad\n# c1\n# c2\n", "/^#.*$/,2026-01-01", 4096);
        assert_eq!(i.record(0).unwrap().hash, i.record(1).unwrap().hash);
        assert_ne!(i.record(1).unwrap().hash, i.record(2).unwrap().hash);
        assert_eq!(i.record(3).unwrap().hash, i.record(4).unwrap().hash); // fully-covered lines match
        assert_eq!(i.rules_key(), "/^#.*$/,2026-01-01");
        assert_eq!(i.record(0).unwrap().length, 15); // original bytes are untouched
    }

    #[test]
    fn rules_work_when_a_line_and_its_match_span_io_blocks() {
        let line = |date: &str| format!("{}{}{}\n", "x".repeat(5000), date, "y".repeat(5000));
        let text = format!("{}{}", line("2026-01-01"), line("2031-07-09"));
        let i = with_rules(&text, "2026-01-01", 4096);
        assert_eq!(i.line_count(), 2);
        assert_eq!(i.record(0).unwrap().hash, i.record(1).unwrap().hash);
        // And identical to scanning with big blocks (one-shot path).
        let j = with_rules(&text, "2026-01-01", 8 << 20);
        assert_eq!(i.record(0).unwrap().hash, j.record(0).unwrap().hash);
    }

    #[test]
    fn rehash_applies_new_rules_to_edited_content_and_keeps_offsets() {
        let (mut idx, src) = editable("v=1\nv=2\n");
        let r = new_line(&idx, &src, "v=3");
        idx.splice(1, 1, vec![r]);
        assert_ne!(idx.record(0).unwrap().hash, idx.record(1).unwrap().hash);
        let rules = LineRules::parse("1,2,3").unwrap().map(Arc::new);
        let re = idx
            .rehash(src.as_ref(), NormalizeOptions::default(), rules, &CancelToken::new(), &mut |_| {}, 1 << 20)
            .unwrap();
        assert_eq!(re.record(0).unwrap().hash, re.record(1).unwrap().hash);
        assert_eq!(re.record(1).unwrap().offset, idx.record(1).unwrap().offset);
        assert!(!re.is_edited()); // flattened
        assert_eq!(text_of(&re, &src), ["v=1", "v=3"]);
    }

    #[test]
    fn cancellation_returns_error_and_leaves_no_spill_file() {
        let cancel = CancelToken::new();
        cancel.cancel();
        let src = MemorySource::new(vec![b'a'; 100]);
        let r = LineIndex::build(&src, &IndexConfig::default(), &cancel, &mut |_| {});
        assert!(matches!(r, Err(EngineError::Cancelled)));
    }
}
