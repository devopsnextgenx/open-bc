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
}

impl Default for IndexConfig {
    fn default() -> Self {
        Self {
            normalize: NormalizeOptions::default(),
            hasher: Arc::new(Xxh3Hasher),
            block_bytes: 8 << 20,
            max_line_bytes: u64::from(u32::MAX),
            ram_record_limit: 8 << 20,
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
        Ok(Self {
            records,
            source_len: total,
            encoding: detected.encoding,
            bom_len: detected.bom_len,
            looks_binary: detected.looks_binary,
            normalize: config.normalize,
        })
    }

    /// Number of lines.
    #[must_use]
    pub fn line_count(&self) -> u64 {
        match &self.records {
            Records::Ram(v) => v.len() as u64,
            Records::Spilled(s) => s.count,
        }
    }

    /// Record for a zero-based line.
    #[must_use]
    pub fn record(&self, line: u64) -> Option<LineRecord> {
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
        match &self.records {
            Records::Ram(v) => (v.capacity() * LineRecord::BYTES) as u64,
            Records::Spilled(_) => 0,
        }
    }

    /// Logical size of all records in bytes.
    #[must_use]
    pub fn record_bytes(&self) -> u64 {
        self.line_count() * LineRecord::BYTES as u64
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
        if self.config.normalize.ignore_line_endings {
            hash
        } else {
            hash ^ (u64::from(eol) + 1).wrapping_mul(0x9E37_79B9_7F4A_7C15)
        }
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
                let hash = self.finish_stream();
                self.emit(hash, flags::EOL_NONE, flags::CONTINUED)?;
                self.line_start += self.line_len;
                self.line_len = 0;
            }
            let take = bytes.len().min((max - self.line_len) as usize);
            self.feed_stream(&bytes[..take]);
            self.line_len += take as u64;
            bytes = &bytes[take..];
        }
        Ok(())
    }

    fn end_line(&mut self, tail: &[u8], eol: u32, terminator_len: u64) -> Result<(), EngineError> {
        let max = self.config.max_line_bytes.max(1);
        let hash = if !self.streaming && self.line_len == 0 && (tail.len() as u64) <= max {
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
            self.finish_stream()
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
            let hash = self.finish_stream();
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
        let hash = if self.identity {
            self.config.hasher.hash_bytes(utf8)
        } else {
            let mut normalized = Vec::new();
            normalize_into(utf8, self.config.normalize, &mut normalized);
            self.config.hasher.hash_bytes(&normalized)
        };
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

    #[test]
    fn cancellation_returns_error_and_leaves_no_spill_file() {
        let cancel = CancelToken::new();
        cancel.cancel();
        let src = MemorySource::new(vec![b'a'; 100]);
        let r = LineIndex::build(&src, &IndexConfig::default(), &cancel, &mut |_| {});
        assert!(matches!(r, Err(EngineError::Cancelled)));
    }
}
