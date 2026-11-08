//! Global diff over indexed lines.
//!
//! Pipeline (all on `u32` line-class ids, never on text):
//!
//! 1. **Match** – every line of both sides is mapped to a *class id*. Lines with equal hashes are
//!    verified byte-for-byte (after the configured normalisation) before they share a class, so a
//!    hash collision can never produce a false match.
//! 2. **Align** – one global, non-paged pass: strip the common prefix/suffix, anchor on lines that
//!    are unique to both sides (patience diff, longest increasing subsequence), fall back to a
//!    histogram anchor (lowest-occurrence shared line) and finally to Myers on the small unmatched
//!    regions that remain. Work is driven by an explicit stack, so recursion depth is irrelevant.
//! 3. **Emit** – the alignment is streamed straight into the run-length [`DiffIndex`]; adjacent
//!    delete/insert runs are paired positionally into `Replace` rows.
//!
//! Alignment never depends on page boundaries. Cancellation is checked between work items and every
//! 64 Ki lines while matching; the result is built privately and returned only on success.
//!
//! Working memory is about `4 B` per line (ids) plus `~24 B` per *distinct* line (class table and
//! hash map) plus `16 B` per distinct line while a region is analysed.

use std::collections::HashMap;
use std::hash::{BuildHasherDefault, Hasher};
use std::time::{Duration, Instant};

use similar::algorithms::{myers, DiffHook};

use super::encoding::{decode_line_into, TextEncoding};
use super::diff_index::{DiffIndex, DiffIndexBuilder, DiffType};
use super::error::{CancelToken, EngineError, Phase, Progress};
use super::line_index::{LineIndex, MAX_LINES};
use super::line_norm::{normalized_eq, EqScratch, NormalizeOptions};
use super::source::TextSource;

/// Tunables for the alignment strategy.
#[derive(Clone, Debug)]
pub struct DiffConfig {
    /// A shared line occurring at most this many times on each side may be used as a histogram
    /// anchor when no unique anchor exists.
    pub histogram_max_occurrences: u32,
    /// Unmatched regions larger than this (sum of both sides, in lines) are not given to Myers;
    /// they are reported as one coarse replace block and the result is flagged `degraded`.
    pub myers_max_lines: u64,
    /// Wall-clock budget for one Myers call; on expiry `similar` emits a valid (possibly
    /// non-minimal) alignment.
    pub myers_time_budget: Duration,
}

impl Default for DiffConfig {
    fn default() -> Self {
        Self {
            histogram_max_occurrences: 64,
            myers_max_lines: 4_000_000,
            myers_time_budget: Duration::from_secs(5),
        }
    }
}

/// One side of a comparison.
#[derive(Clone, Copy)]
pub struct Side<'a> {
    /// Byte storage.
    pub source: &'a dyn TextSource,
    /// Its line index.
    pub index: &'a LineIndex,
}

/// The diff engine. Stateless; cheap to construct.
#[derive(Clone, Debug, Default)]
pub struct DiffEngine {
    config: DiffConfig,
}

#[derive(Default)]
struct IdentityHasher(u64);
impl Hasher for IdentityHasher {
    fn finish(&self) -> u64 {
        self.0
    }
    fn write(&mut self, bytes: &[u8]) {
        for &b in bytes {
            self.0 = (self.0 << 8) ^ u64::from(b);
        }
    }
    fn write_u64(&mut self, n: u64) {
        self.0 = n;
    }
}

const NONE: u32 = u32::MAX;

struct ClassRep {
    side: u8,
    line: u32,
    next: u32,
}

#[derive(Clone, Copy, Default)]
struct Occ {
    lcount: u32,
    rcount: u32,
    lfirst: u32,
    rfirst: u32,
}

enum Work {
    Range { l0: usize, l1: usize, r0: usize, r1: usize },
    Equal(usize),
}

/// Streams alignment ops into a [`DiffIndexBuilder`], pairing deletes with inserts.
struct Emitter {
    builder: DiffIndexBuilder,
    del: u64,
    ins: u64,
    resolved: u64,
}

impl Emitter {
    fn flush(&mut self) {
        let paired = self.del.min(self.ins);
        self.builder.push(DiffType::Replace, paired);
        self.builder.push(DiffType::Delete, self.del - paired);
        self.builder.push(DiffType::Insert, self.ins - paired);
        self.del = 0;
        self.ins = 0;
    }
    fn equal(&mut self, len: usize) {
        if len == 0 {
            return;
        }
        self.flush();
        self.builder.push(DiffType::Equal, len as u64);
        self.resolved += 2 * len as u64;
    }
    fn delete(&mut self, len: usize) {
        self.del += len as u64;
        self.resolved += len as u64;
    }
    fn insert(&mut self, len: usize) {
        self.ins += len as u64;
        self.resolved += len as u64;
    }
}

struct Collect(Vec<(u8, usize)>);
impl DiffHook for Collect {
    type Error = std::convert::Infallible;
    fn equal(&mut self, _: usize, _: usize, len: usize) -> Result<(), Self::Error> {
        self.0.push((0, len));
        Ok(())
    }
    fn delete(&mut self, _: usize, len: usize, _: usize) -> Result<(), Self::Error> {
        self.0.push((1, len));
        Ok(())
    }
    fn insert(&mut self, _: usize, _: usize, len: usize) -> Result<(), Self::Error> {
        self.0.push((2, len));
        Ok(())
    }
}

impl DiffEngine {
    /// Engine with default tuning.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Engine with explicit tuning.
    #[must_use]
    pub fn with_config(config: DiffConfig) -> Self {
        Self { config }
    }

    /// Align `left` and `right`.
    pub fn diff(
        &self,
        left: Side<'_>,
        right: Side<'_>,
        cancel: &CancelToken,
        progress: &mut dyn FnMut(Progress),
    ) -> Result<DiffIndex, EngineError> {
        let (nl, nr) = (left.index.line_count(), right.index.line_count());
        let builder =
            self.diff_region(left, right, (0, nl as usize), (0, nr as usize), cancel, progress)?;
        let index = builder.finish();
        debug_assert_eq!(index.line_counts(), (nl, nr), "alignment must cover every line exactly once");
        Ok(index)
    }

    /// Align only the line ranges `left_range` (`start..end`) and `right_range`. The result's
    /// segments are relative to the start of the ranges (cursors begin at 0), ready to be spliced
    /// into an existing [`DiffIndex`] with `DiffIndex::splice_region`. This is what makes editing
    /// cheap: only the region around an edit is re-aligned.
    pub fn diff_region(
        &self,
        left: Side<'_>,
        right: Side<'_>,
        left_range: (usize, usize),
        right_range: (usize, usize),
        cancel: &CancelToken,
        progress: &mut dyn FnMut(Progress),
    ) -> Result<DiffIndexBuilder, EngineError> {
        let opts = left.index.normalize_options();
        if opts != right.index.normalize_options() || left.index.rules_key() != right.index.rules_key() {
            return Err(EngineError::OptionsMismatch);
        }
        let (nl, nr) = (left.index.line_count(), right.index.line_count());
        if nl > MAX_LINES || nr > MAX_LINES {
            return Err(EngineError::TooManyLines(nl.max(nr), MAX_LINES));
        }
        let (l_first, l_len) = (left_range.0, left_range.1 - left_range.0);
        let (r_first, r_len) = (right_range.0, right_range.1 - right_range.0);
        let (prefix, suffix) = common_affixes(left, right, opts, cancel, left_range, right_range)?;
        let (mid_l, mid_r) = (l_len - prefix - suffix, r_len - prefix - suffix);
        let (lid, rid, classes) = self.intern(
            left,
            right,
            opts,
            (l_first + prefix, mid_l),
            (r_first + prefix, mid_r),
            cancel,
            progress,
        )?;
        let mut occ = vec![Occ::default(); classes];
        let mut emitter = Emitter {
            builder: DiffIndex::builder(),
            del: 0,
            ins: 0,
            resolved: 0,
        };
        let total = (l_len + r_len) as u64;
        let mut last_report = 0u64;
        // The identical head and tail never enter the matching/aligning machinery.
        emitter.equal(prefix);
        let mut stack = vec![Work::Equal(suffix), Work::Range { l0: 0, l1: lid.len(), r0: 0, r1: rid.len() }];
        while let Some(work) = stack.pop() {
            cancel.check()?;
            match work {
                Work::Equal(len) => emitter.equal(len),
                Work::Range { l0, l1, r0, r1 } => {
                    self.solve(&lid, &rid, &mut occ, &mut stack, &mut emitter, (l0, l1, r0, r1));
                }
            }
            if emitter.resolved - last_report >= 1 << 16 {
                last_report = emitter.resolved;
                progress(Progress { phase: Phase::Aligning, done: emitter.resolved.min(total), total });
            }
        }
        emitter.flush();
        progress(Progress { phase: Phase::Aligning, done: total, total });
        debug_assert_eq!(emitter.builder.line_counts(), (l_len as u64, r_len as u64));
        Ok(emitter.builder)
    }

    // ---- step 1: verified line classes ---------------------------------------------------------

    fn intern(
        &self,
        left: Side<'_>,
        right: Side<'_>,
        opts: NormalizeOptions,
        left_range: (usize, usize),
        right_range: (usize, usize),
        cancel: &CancelToken,
        progress: &mut dyn FnMut(Progress),
    ) -> Result<(Vec<u32>, Vec<u32>, usize), EngineError> {
        let sides = [left, right];
        let ranges = [left_range, right_range];
        let total = (left_range.1 + right_range.1) as u64;
        let mut map: HashMap<u64, u32, BuildHasherDefault<IdentityHasher>> = HashMap::default();
        let mut classes: Vec<ClassRep> = Vec::new();
        let mut ids: [Vec<u32>; 2] = [Vec::new(), Vec::new()];
        let mut scratch = EqScratch::default();
        let (mut buf_a, mut buf_b) = (Vec::new(), Vec::new());
        let mut done = 0u64;
        for (s, side) in sides.iter().enumerate() {
            let (first, count) = (ranges[s].0 as u64, ranges[s].1 as u64);
            ids[s].reserve(count as usize);
            for line in first..first + count {
                if done & 0xFFFF == 0 {
                    cancel.check()?;
                    progress(Progress { phase: Phase::Matching, done, total });
                }
                done += 1;
                let rec = side.index.record(line).ok_or(EngineError::SourceChanged)?;
                let head = map.get(&rec.hash).copied().unwrap_or(NONE);
                let mut found = NONE;
                let mut cur = head;
                while cur != NONE {
                    let rep = &classes[cur as usize];
                    let rep_side = sides[rep.side as usize];
                    let rep_rec = rep_side.index.record(u64::from(rep.line)).ok_or(EngineError::SourceChanged)?;
                    let eol_ok = opts.ignore_line_endings || rep_rec.eol() == rec.eol();
                    if eol_ok
                        && lines_equal(*side, line, rep_side, u64::from(rep.line), opts, &mut scratch, &mut buf_a, &mut buf_b)?
                    {
                        found = cur;
                        break;
                    }
                    cur = rep.next;
                }
                if found == NONE {
                    found = classes.len() as u32;
                    classes.push(ClassRep { side: s as u8, line: line as u32, next: head });
                    map.insert(rec.hash, found);
                }
                ids[s].push(found);
            }
        }
        let [l, r] = ids;
        Ok((l, r, classes.len()))
    }

    // ---- step 2: align one range ---------------------------------------------------------------

    #[allow(clippy::too_many_arguments)]
    fn solve(
        &self,
        lid: &[u32],
        rid: &[u32],
        occ: &mut [Occ],
        stack: &mut Vec<Work>,
        emitter: &mut Emitter,
        range: (usize, usize, usize, usize),
    ) {
        let (mut l0, mut l1, mut r0, mut r1) = range;
        let mut p = 0;
        while l0 + p < l1 && r0 + p < r1 && lid[l0 + p] == rid[r0 + p] {
            p += 1;
        }
        emitter.equal(p);
        l0 += p;
        r0 += p;
        let mut s = 0;
        while l1 - s > l0 && r1 - s > r0 && lid[l1 - s - 1] == rid[r1 - s - 1] {
            s += 1;
        }
        l1 -= s;
        r1 -= s;
        if s > 0 {
            stack.push(Work::Equal(s));
        }
        if l0 == l1 {
            emitter.insert(r1 - r0);
            return;
        }
        if r0 == r1 {
            emitter.delete(l1 - l0);
            return;
        }

        // Count occurrences inside this range only.
        for i in l0..l1 {
            let o = &mut occ[lid[i] as usize];
            if o.lcount == 0 {
                o.lfirst = i as u32;
            }
            o.lcount += 1;
        }
        for j in r0..r1 {
            let o = &mut occ[rid[j] as usize];
            if o.rcount == 0 {
                o.rfirst = j as u32;
            }
            o.rcount += 1;
        }

        // Patience: lines that occur exactly once on each side, longest increasing subsequence.
        let mut pairs: Vec<(u32, u32)> = Vec::new();
        for i in l0..l1 {
            let o = occ[lid[i] as usize];
            if o.lcount == 1 && o.rcount == 1 {
                pairs.push((i as u32, o.rfirst));
            }
        }
        let mut anchors = longest_increasing(&pairs);

        if anchors.is_empty() {
            // Histogram: the shared line with the fewest occurrences.
            let cap = self.config.histogram_max_occurrences;
            let mut best: Option<(u32, u32, u32)> = None;
            for i in l0..l1 {
                let o = occ[lid[i] as usize];
                if o.lfirst as usize == i && o.rcount > 0 && o.lcount <= cap && o.rcount <= cap {
                    let score = o.lcount.max(o.rcount);
                    if best.is_none_or(|(b, _, _)| score < b) {
                        best = Some((score, o.lfirst, o.rfirst));
                    }
                }
            }
            if let Some((_, l, r)) = best {
                anchors.push((l, r));
            }
        }

        // Reset the touched entries.
        for i in l0..l1 {
            occ[lid[i] as usize] = Occ::default();
        }
        for j in r0..r1 {
            occ[rid[j] as usize] = Occ::default();
        }

        if !anchors.is_empty() {
            let mut items: Vec<Work> = Vec::with_capacity(anchors.len() * 2 + 1);
            let (mut pl, mut pr) = (l0, r0);
            for (a, b) in anchors {
                let (a, b) = (a as usize, b as usize);
                if a > pl || b > pr {
                    items.push(Work::Range { l0: pl, l1: a, r0: pr, r1: b });
                }
                items.push(Work::Equal(1));
                pl = a + 1;
                pr = b + 1;
            }
            if l1 > pl || r1 > pr {
                items.push(Work::Range { l0: pl, l1, r0: pr, r1 });
            }
            stack.extend(items.into_iter().rev());
            return;
        }

        // No usable anchor: Myers on what is left, or a coarse replace when it is too big.
        if ((l1 - l0) + (r1 - r0)) as u64 > self.config.myers_max_lines {
            emitter.builder.mark_degraded();
            emitter.delete(l1 - l0);
            emitter.insert(r1 - r0);
            return;
        }
        let deadline = Instant::now().checked_add(self.config.myers_time_budget);
        let mut hook = Collect(Vec::new());
        let _ = myers::diff_deadline(&mut hook, lid, l0..l1, rid, r0..r1, deadline);
        for (kind, len) in hook.0 {
            match kind {
                0 => emitter.equal(len),
                1 => emitter.delete(len),
                _ => emitter.insert(len),
            }
        }
    }
}

/// Number of identical lines at the head and at the tail of both files (verified on the real
/// bytes). Sequential and cache-friendly; for "mostly similar" files this removes almost all of
/// the matching work and memory.
fn common_affixes(
    left: Side<'_>,
    right: Side<'_>,
    opts: NormalizeOptions,
    cancel: &CancelToken,
    left_range: (usize, usize),
    right_range: (usize, usize),
) -> Result<(usize, usize), EngineError> {
    let (l0, r0) = (left_range.0 as u64, right_range.0 as u64);
    let (nl, nr) = ((left_range.1 - left_range.0) as u64, (right_range.1 - right_range.0) as u64);
    let mut scratch = EqScratch::default();
    let (mut a, mut b) = (Vec::new(), Vec::new());
    let mut same = |li: u64, ri: u64| -> Result<bool, EngineError> {
        let (x, y) = (
            left.index.record(li).ok_or(EngineError::SourceChanged)?,
            right.index.record(ri).ok_or(EngineError::SourceChanged)?,
        );
        if x.hash != y.hash || (!opts.ignore_line_endings && x.eol() != y.eol()) {
            return Ok(false);
        }
        lines_equal(left, li, right, ri, opts, &mut scratch, &mut a, &mut b)
    };
    let limit = nl.min(nr);
    let mut prefix = 0u64;
    while prefix < limit {
        if prefix & 0xFFFF == 0 {
            cancel.check()?;
        }
        if !same(l0 + prefix, r0 + prefix)? {
            break;
        }
        prefix += 1;
    }
    let mut suffix = 0u64;
    while suffix < limit - prefix {
        if suffix & 0xFFFF == 0 {
            cancel.check()?;
        }
        if !same(l0 + nl - 1 - suffix, r0 + nr - 1 - suffix)? {
            break;
        }
        suffix += 1;
    }
    Ok((prefix as usize, suffix as usize))
}

/// Longest subsequence of `pairs` (sorted by `.0`) that is also increasing in `.1`.
fn longest_increasing(pairs: &[(u32, u32)]) -> Vec<(u32, u32)> {
    if pairs.is_empty() {
        return Vec::new();
    }
    let mut tails: Vec<usize> = Vec::new();
    let mut prev = vec![usize::MAX; pairs.len()];
    for (k, &(_, r)) in pairs.iter().enumerate() {
        let pos = tails.partition_point(|&t| pairs[t].1 < r);
        if pos > 0 {
            prev[k] = tails[pos - 1];
        }
        if pos == tails.len() {
            tails.push(k);
        } else {
            tails[pos] = k;
        }
    }
    let mut out = Vec::with_capacity(tails.len());
    let mut cur = *tails.last().expect("non-empty");
    loop {
        out.push(pairs[cur]);
        if prev[cur] == usize::MAX {
            break;
        }
        cur = prev[cur];
    }
    out.reverse();
    out
}

/// Text bytes of a line as UTF-8 (transcoding UTF-16 into `buf`).
fn text_bytes<'a>(
    side: Side<'a>,
    line: u64,
    buf: &'a mut Vec<u8>,
) -> Result<std::borrow::Cow<'a, [u8]>, EngineError> {
    let raw = side.index.line_bytes(side.source, line)?;
    if side.index.encoding() == TextEncoding::Utf8 {
        Ok(raw)
    } else {
        decode_line_into(&raw, side.index.encoding(), buf);
        Ok(std::borrow::Cow::Borrowed(&buf[..]))
    }
}

/// Verify that two indexed lines are equal after normalisation, reading the real bytes.
#[allow(clippy::too_many_arguments)]
fn lines_equal(
    a: Side<'_>,
    a_line: u64,
    b: Side<'_>,
    b_line: u64,
    opts: NormalizeOptions,
    scratch: &mut EqScratch,
    buf_a: &mut Vec<u8>,
    buf_b: &mut Vec<u8>,
) -> Result<bool, EngineError> {
    let x = text_bytes(a, a_line, buf_a)?;
    let y = text_bytes(b, b_line, buf_b)?;
    // Ignore rules first (same order as hashing), then whitespace/case normalisation.
    if let Some(rules) = a.index.rules() {
        let (mx, my) = (rules.apply(&x), rules.apply(&y));
        return Ok(normalized_eq(&mx, &my, opts, scratch));
    }
    Ok(normalized_eq(&x, &y, opts, scratch))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::text::engine::diff_index::DiffRow;
    use crate::text::engine::hasher::{HashStream, LineHasher};
    use crate::text::engine::line_index::IndexConfig;
    use crate::text::engine::source::MemorySource;
    use std::sync::Arc;

    struct Built {
        src: MemorySource,
        idx: LineIndex,
    }

    fn build(text: &str, cfg: &IndexConfig) -> Built {
        let src = MemorySource::new(text.as_bytes().to_vec());
        let idx = LineIndex::build(&src, cfg, &CancelToken::new(), &mut |_| {}).unwrap();
        Built { src, idx }
    }

    fn run(l: &str, r: &str, cfg: &IndexConfig, dc: DiffConfig) -> DiffIndex {
        let (a, b) = (build(l, cfg), build(r, cfg));
        DiffEngine::with_config(dc)
            .diff(
                Side { source: &a.src, index: &a.idx },
                Side { source: &b.src, index: &b.idx },
                &CancelToken::new(),
                &mut |_| {},
            )
            .unwrap()
    }

    fn lines_of(text: &str) -> Vec<&str> {
        text.lines().collect()
    }

    /// Alignment must cover each line once, in order, and Equal rows must really be equal.
    fn assert_valid(l: &str, r: &str, d: &DiffIndex, norm: impl Fn(&str) -> String) {
        let (ll, rl) = (lines_of(l), lines_of(r));
        let (mut nl, mut nr) = (0u64, 0u64);
        for row in 0..d.row_count() {
            let DiffRow { left_line, right_line, kind } = d.row(row).unwrap();
            if let Some(x) = left_line {
                assert_eq!(x, nl, "left lines must be contiguous");
                nl += 1;
            }
            if let Some(x) = right_line {
                assert_eq!(x, nr, "right lines must be contiguous");
                nr += 1;
            }
            if kind == DiffType::Equal {
                assert_eq!(norm(ll[left_line.unwrap() as usize]), norm(rl[right_line.unwrap() as usize]));
            }
        }
        assert_eq!((nl, nr), (ll.len() as u64, rl.len() as u64));
    }

    fn kinds(d: &DiffIndex) -> Vec<(DiffType, u32)> {
        d.segments().iter().map(|s| (s.kind, s.len)).collect()
    }

    fn id(s: &str) -> String {
        s.to_owned()
    }

    #[test]
    fn identical_empty_and_trivial_inputs() {
        let cfg = IndexConfig::default();
        assert!(run("", "", &cfg, DiffConfig::default()).is_identical());
        assert_eq!(run("", "", &cfg, DiffConfig::default()).row_count(), 0);
        let d = run("a\nb\n", "a\nb\n", &cfg, DiffConfig::default());
        assert!(d.is_identical());
        assert_eq!(kinds(&d), [(DiffType::Equal, 2)]);
        let d = run("", "x\ny\n", &cfg, DiffConfig::default());
        assert_eq!(kinds(&d), [(DiffType::Insert, 2)]);
        let d = run("x\ny\n", "", &cfg, DiffConfig::default());
        assert_eq!(kinds(&d), [(DiffType::Delete, 2)]);
    }

    #[test]
    fn single_line_insert_delete_replace() {
        let cfg = IndexConfig::default();
        let d = run("a\nb\nc\n", "a\nX\nb\nc\n", &cfg, DiffConfig::default());
        assert_eq!(kinds(&d), [(DiffType::Equal, 1), (DiffType::Insert, 1), (DiffType::Equal, 2)]);
        let d = run("a\nb\nc\n", "a\nc\n", &cfg, DiffConfig::default());
        assert_eq!(kinds(&d), [(DiffType::Equal, 1), (DiffType::Delete, 1), (DiffType::Equal, 1)]);
        let d = run("a\nb\nc\n", "a\nB\nc\n", &cfg, DiffConfig::default());
        assert_eq!(kinds(&d), [(DiffType::Equal, 1), (DiffType::Replace, 1), (DiffType::Equal, 1)]);
    }

    #[test]
    fn replace_block_pairs_positionally_with_remainder() {
        let cfg = IndexConfig::default();
        let d = run("k\na\nb\nc\nz\n", "k\nX\nz\n", &cfg, DiffConfig::default());
        assert_eq!(
            kinds(&d),
            [(DiffType::Equal, 1), (DiffType::Replace, 1), (DiffType::Delete, 2), (DiffType::Equal, 1)]
        );
    }

    #[test]
    fn large_blocks_inserted_and_deleted_near_start_middle_end() {
        let cfg = IndexConfig::default();
        let base: Vec<String> = (0..3000).map(|i| format!("line {i}")).collect();
        for pos in [0usize, 1500, 3000] {
            let mut with = base.clone();
            for k in 0..700 {
                with.insert(pos + k, format!("new {k}"));
            }
            let (l, r) = (base.join("\n") + "\n", with.join("\n") + "\n");
            let d = run(&l, &r, &cfg, DiffConfig::default());
            assert_valid(&l, &r, &d, id);
            assert_eq!(d.stats().inserted_rows, 700, "pos {pos}");
            assert_eq!(d.stats().blocks, 1);
            let d = run(&r, &l, &cfg, DiffConfig::default());
            assert_eq!(d.stats().deleted_rows, 700, "pos {pos}");
        }
    }

    #[test]
    fn alignment_does_not_drift_across_former_page_boundaries() {
        // The old pager used 512-line pages. Put a big insertion straddling several boundaries
        // plus a deletion later; a global diff must still find exactly those two changes.
        let cfg = IndexConfig::default();
        let base: Vec<String> = (0..5000).map(|i| format!("row {i} payload")).collect();
        let mut changed = base.clone();
        for k in 0..900 {
            changed.insert(300, format!("inserted {k}"));
        }
        changed.drain(3200..3700);
        let (l, r) = (base.join("\n") + "\n", changed.join("\n") + "\n");
        let d = run(&l, &r, &cfg, DiffConfig::default());
        assert_valid(&l, &r, &d, id);
        let s = d.stats();
        assert_eq!((s.inserted_rows, s.deleted_rows, s.replaced_rows, s.blocks), (900, 500, 0, 2));
    }

    #[test]
    fn repeated_identical_lines() {
        let cfg = IndexConfig::default();
        let l = "x\n".repeat(50_000);
        let r = "x\n".repeat(50_001);
        let d = run(&l, &r, &cfg, DiffConfig::default());
        assert_valid(&l, &r, &d, id);
        assert_eq!(d.stats().inserted_rows, 1);
        // Repetitive content with no unique anchors still produces a valid alignment.
        let l = "a\nb\na\nb\na\nb\nc\n".repeat(40);
        let r = "b\na\nb\na\nb\nc\nd\n".repeat(40);
        let d = run(&l, &r, &cfg, DiffConfig::default());
        assert_valid(&l, &r, &d, id);
    }

    #[test]
    fn coarse_fallback_is_flagged_and_still_valid() {
        let cfg = IndexConfig::default();
        let l = "a\nb\n".repeat(300);
        let r = "b\na\n".repeat(300);
        let mut dc = DiffConfig::default();
        dc.histogram_max_occurrences = 0;
        dc.myers_max_lines = 10;
        let d = run(&l, &r, &cfg, dc);
        assert!(d.is_degraded());
        assert_valid(&l, &r, &d, id);
    }

    /// Hash that maps every line to the same value: all lines collide.
    struct ConstHasher;
    struct ConstStream;
    impl HashStream for ConstStream {
        fn update(&mut self, _: &[u8]) {}
        fn finish_reset(&mut self) -> u64 {
            42
        }
    }
    impl LineHasher for ConstHasher {
        fn hash_bytes(&self, _: &[u8]) -> u64 {
            42
        }
        fn new_stream(&self) -> Box<dyn HashStream> {
            Box::new(ConstStream)
        }
    }

    #[test]
    fn hash_collisions_never_create_false_matches() {
        let real = IndexConfig::default();
        let mut bad = IndexConfig::default();
        bad.hasher = Arc::new(ConstHasher);
        let l = "alpha\nbeta\ngamma\ndelta\nepsilon\n";
        let r = "alpha\nBETA\ngamma\nzeta\nepsilon\nextra\n";
        let good = run(l, r, &real, DiffConfig::default());
        let colliding = run(l, r, &bad, DiffConfig::default());
        assert_valid(l, r, &colliding, id);
        assert_eq!(kinds(&good), kinds(&colliding));
        // And all lines really do collide in the bad index.
        let b = build(l, &bad);
        assert!((0..5).all(|n| b.idx.record(n).unwrap().hash == 42));
    }

    #[test]
    fn normalisation_options_change_the_alignment() {
        let l = "  Foo bar \nkeep\n";
        let r = "foo bar\nkeep\n";
        assert_eq!(run(l, r, &IndexConfig::default(), DiffConfig::default()).stats().replaced_rows, 1);
        let mut cfg = IndexConfig::default();
        cfg.normalize.ignore_leading_trailing_whitespace = true;
        cfg.normalize.ignore_case = true;
        let d = run(l, r, &cfg, DiffConfig::default());
        assert!(d.is_identical());
        let mut cfg = IndexConfig::default();
        cfg.normalize.ignore_all_whitespace = true;
        let d = run("a b c\n", "abc\n", &cfg, DiffConfig::default());
        assert!(d.is_identical());
    }

    #[test]
    fn line_endings_are_ignored_by_default_and_significant_on_request() {
        let l = "a\r\nb\r\n";
        let r = "a\nb";
        assert!(run(l, r, &IndexConfig::default(), DiffConfig::default()).is_identical());
        let mut strict = IndexConfig::default();
        strict.normalize.ignore_line_endings = false;
        let d = run(l, r, &strict, DiffConfig::default());
        assert_eq!(d.stats().replaced_rows, 2);
    }

    #[test]
    fn utf8_and_utf16_files_compare_equal() {
        let cfg = IndexConfig::default();
        let a = build("héllo\nwörld\n", &cfg);
        let mut bytes = vec![0xFF, 0xFE];
        for u in "héllo\r\nwörld\r\n".encode_utf16() {
            bytes.extend_from_slice(&u.to_le_bytes());
        }
        let b = build_bytes(&bytes, &cfg);
        let d = DiffEngine::new()
            .diff(
                Side { source: &a.src, index: &a.idx },
                Side { source: &b.src, index: &b.idx },
                &CancelToken::new(),
                &mut |_| {},
            )
            .unwrap();
        assert!(d.is_identical());
    }

    fn build_bytes(bytes: &[u8], cfg: &IndexConfig) -> Built {
        let src = MemorySource::new(bytes.to_vec());
        let idx = LineIndex::build(&src, cfg, &CancelToken::new(), &mut |_| {}).unwrap();
        Built { src, idx }
    }

    #[test]
    fn mismatched_options_are_rejected() {
        let a = build("a\n", &IndexConfig::default());
        let mut other = IndexConfig::default();
        other.normalize.ignore_case = true;
        let b = build("a\n", &other);
        let r = DiffEngine::new().diff(
            Side { source: &a.src, index: &a.idx },
            Side { source: &b.src, index: &b.idx },
            &CancelToken::new(),
            &mut |_| {},
        );
        assert!(matches!(r, Err(EngineError::OptionsMismatch)));
    }

    #[test]
    fn cancellation_discards_everything() {
        let cfg = IndexConfig::default();
        let text = "line\n".repeat(200_000);
        let a = build(&text, &cfg);
        let b = build(&(text.clone() + "tail\n"), &cfg);
        let cancel = CancelToken::new();
        let c2 = cancel.clone();
        let mut calls = 0;
        let r = DiffEngine::new().diff(
            Side { source: &a.src, index: &a.idx },
            Side { source: &b.src, index: &b.idx },
            &cancel,
            &mut |_| {
                calls += 1;
                if calls == 2 {
                    c2.cancel();
                }
            },
        );
        assert!(matches!(r, Err(EngineError::Cancelled)));
    }

    #[test]
    fn progress_reaches_the_total() {
        let cfg = IndexConfig::default();
        let a = build(&"a\n".repeat(1000), &cfg);
        let b = build(&"b\n".repeat(1000), &cfg);
        let mut last = None;
        DiffEngine::new()
            .diff(
                Side { source: &a.src, index: &a.idx },
                Side { source: &b.src, index: &b.idx },
                &CancelToken::new(),
                &mut |p| last = Some(p),
            )
            .unwrap();
        let p = last.unwrap();
        assert_eq!((p.phase, p.done, p.total), (Phase::Aligning, 2000, 2000));
    }

    /// Deterministic pseudo-random edits; every result must be a valid alignment.
    #[test]
    fn random_edits_always_produce_valid_alignments() {
        let mut seed = 0x1234_5678_9ABC_DEF0u64;
        let mut next = move || {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            seed
        };
        let cfg = IndexConfig::default();
        for _ in 0..60 {
            let n = (next() % 120) as usize;
            let alphabet = 2 + (next() % 30) as u64;
            let base: Vec<String> = (0..n).map(|_| format!("v{}", next() % alphabet)).collect();
            let mut other = base.clone();
            for _ in 0..(next() % 15) {
                let at = if other.is_empty() { 0 } else { (next() as usize) % other.len().max(1) };
                match next() % 3 {
                    0 => other.insert(at.min(other.len()), format!("v{}", next() % alphabet)),
                    1 if !other.is_empty() => {
                        other.remove(at);
                    }
                    _ if !other.is_empty() => other[at] = format!("w{}", next() % alphabet),
                    _ => {}
                }
            }
            let (l, r) = (base.join("\n") + "\n", other.join("\n") + "\n");
            let (l, r) = (if base.is_empty() { String::new() } else { l }, if other.is_empty() { String::new() } else { r });
            let d = run(&l, &r, &cfg, DiffConfig::default());
            assert_valid(&l, &r, &d, id);
        }
    }
}
