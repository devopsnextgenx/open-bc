//! Compact, run-length-encoded description of how two files align.
//!
//! The alignment is a list of [`Segment`]s. Each segment covers `len` consecutive *display rows*:
//!
//! | kind      | left line of row `k`        | right line of row `k`        |
//! |-----------|-----------------------------|------------------------------|
//! | `Equal`   | `left_start + k`            | `right_start + k`            |
//! | `Replace` | `left_start + k`            | `right_start + k`            |
//! | `Delete`  | `left_start + k`            | none (filler)                |
//! | `Insert`  | none (filler)               | `right_start + k`            |
//!
//! A million identical lines are one segment. No line text is stored. Row, left-line and
//! right-line lookups are binary searches; changed blocks are precomputed for navigation.

/// Relationship of a displayed row.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum DiffType {
    /// Both sides have the line and they match.
    Equal = 0,
    /// Only the right side has the line.
    Insert = 1,
    /// Only the left side has the line.
    Delete = 2,
    /// Both sides have a line at this row but they differ.
    Replace = 3,
}

impl DiffType {
    fn has_left(self) -> bool {
        !matches!(self, Self::Insert)
    }
    fn has_right(self) -> bool {
        !matches!(self, Self::Delete)
    }
}

/// A run of rows of one kind.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Segment {
    /// First display row of the segment.
    pub row_start: u64,
    /// First left line (also the left cursor for `Insert`, which has no left lines).
    pub left_start: u64,
    /// First right line (also the right cursor for `Delete`).
    pub right_start: u64,
    /// Number of rows.
    pub len: u32,
    /// Kind.
    pub kind: DiffType,
}

/// One resolved display row.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DiffRow {
    /// Zero-based left line, if the left side has a line here.
    pub left_line: Option<u64>,
    /// Zero-based right line, if the right side has a line here.
    pub right_line: Option<u64>,
    /// Kind of the row.
    pub kind: DiffType,
}

/// A maximal run of consecutive non-`Equal` rows.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ChangeBlock {
    /// First row.
    pub start_row: u64,
    /// Number of rows.
    pub rows: u64,
}

/// Summary counters.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DiffStats {
    /// Rows where both sides match.
    pub equal_rows: u64,
    /// Rows only on the right.
    pub inserted_rows: u64,
    /// Rows only on the left.
    pub deleted_rows: u64,
    /// Rows that differ on both sides.
    pub replaced_rows: u64,
    /// Number of changed blocks.
    pub blocks: u64,
}

/// The alignment of two files.
#[derive(Clone, Debug, Default)]
pub struct DiffIndex {
    segments: Vec<Segment>,
    blocks: Vec<ChangeBlock>,
    rows: u64,
    left_lines: u64,
    right_lines: u64,
    /// True when part of the alignment was produced by the coarse fallback (see `DiffConfig`).
    degraded: bool,
}

impl DiffIndex {
    /// Builder that merges adjacent compatible runs and splits runs longer than `u32::MAX`.
    #[must_use]
    pub fn builder() -> DiffIndexBuilder {
        DiffIndexBuilder::default()
    }

    /// Total number of display rows.
    #[must_use]
    pub fn row_count(&self) -> u64 {
        self.rows
    }

    /// Lines on the left / right.
    #[must_use]
    pub fn line_counts(&self) -> (u64, u64) {
        (self.left_lines, self.right_lines)
    }

    /// True when every row is `Equal`.
    #[must_use]
    pub fn is_identical(&self) -> bool {
        self.blocks.is_empty()
    }

    /// True when the alignment used the coarse "replace the whole region" fallback somewhere.
    #[must_use]
    pub fn is_degraded(&self) -> bool {
        self.degraded
    }

    /// All segments (read-only).
    #[must_use]
    pub fn segments(&self) -> &[Segment] {
        &self.segments
    }

    /// All changed blocks (read-only).
    #[must_use]
    pub fn blocks(&self) -> &[ChangeBlock] {
        &self.blocks
    }

    /// Heap bytes used by the index.
    #[must_use]
    pub fn heap_bytes(&self) -> u64 {
        (self.segments.capacity() * std::mem::size_of::<Segment>()
            + self.blocks.capacity() * std::mem::size_of::<ChangeBlock>()) as u64
    }

    /// Counters.
    #[must_use]
    pub fn stats(&self) -> DiffStats {
        let mut s = DiffStats { blocks: self.blocks.len() as u64, ..DiffStats::default() };
        for seg in &self.segments {
            let n = u64::from(seg.len);
            match seg.kind {
                DiffType::Equal => s.equal_rows += n,
                DiffType::Insert => s.inserted_rows += n,
                DiffType::Delete => s.deleted_rows += n,
                DiffType::Replace => s.replaced_rows += n,
            }
        }
        s
    }

    fn segment_index_for_row(&self, row: u64) -> Option<usize> {
        if row >= self.rows {
            return None;
        }
        let idx = self.segments.partition_point(|s| s.row_start <= row);
        idx.checked_sub(1)
    }

    /// Resolve a display row.
    #[must_use]
    pub fn row(&self, row: u64) -> Option<DiffRow> {
        let seg = self.segments[self.segment_index_for_row(row)?];
        let k = row - seg.row_start;
        Some(DiffRow {
            left_line: seg.kind.has_left().then(|| seg.left_start + k),
            right_line: seg.kind.has_right().then(|| seg.right_start + k),
            kind: seg.kind,
        })
    }

    /// Row on which `line` of the left file is displayed.
    #[must_use]
    pub fn row_for_left_line(&self, line: u64) -> Option<u64> {
        self.row_for_line(line, true)
    }

    /// Row on which `line` of the right file is displayed.
    #[must_use]
    pub fn row_for_right_line(&self, line: u64) -> Option<u64> {
        self.row_for_line(line, false)
    }

    fn row_for_line(&self, line: u64, left: bool) -> Option<u64> {
        let (total, start_of): (u64, fn(&Segment) -> u64) = if left {
            (self.left_lines, |s| s.left_start)
        } else {
            (self.right_lines, |s| s.right_start)
        };
        if line >= total {
            return None;
        }
        let has = |k: DiffType| if left { k.has_left() } else { k.has_right() };
        let mut idx = self.segments.partition_point(|s| start_of(s) <= line);
        while idx > 0 {
            idx -= 1;
            let seg = &self.segments[idx];
            if has(seg.kind) && line < start_of(seg) + u64::from(seg.len) {
                return Some(seg.row_start + (line - start_of(seg)));
            }
        }
        None
    }

    /// Per-bucket change summary for an overview strip of `buckets` pixels. Bit 0 = contains
    /// inserted rows, bit 1 = deleted rows, bit 2 = replaced rows. O(segments + buckets); never
    /// touches line data, so it is cheap even for hundreds of millions of rows.
    #[must_use]
    pub fn overview(&self, buckets: usize) -> Vec<u8> {
        let mut out = vec![0u8; buckets];
        if buckets == 0 || self.rows == 0 {
            return out;
        }
        let bucket_of = |row: u64| ((u128::from(row) * buckets as u128) / u128::from(self.rows)) as usize;
        for seg in &self.segments {
            let bit = match seg.kind {
                DiffType::Equal => continue,
                DiffType::Insert => 1,
                DiffType::Delete => 2,
                DiffType::Replace => 4,
            };
            let first = bucket_of(seg.row_start);
            let last = bucket_of(seg.row_start + u64::from(seg.len) - 1).min(buckets - 1);
            for cell in &mut out[first..=last] {
                *cell |= bit;
            }
        }
        out
    }

    /// Index into [`blocks`](Self::blocks) of the block containing `row`, if any.
    #[must_use]
    pub fn block_containing(&self, row: u64) -> Option<usize> {
        let idx = self.blocks.partition_point(|b| b.start_row <= row).checked_sub(1)?;
        let b = self.blocks[idx];
        (row < b.start_row + b.rows).then_some(idx)
    }

    /// Start row of the first changed block that begins after `row`.
    #[must_use]
    pub fn next_difference(&self, row: u64) -> Option<u64> {
        let idx = self.blocks.partition_point(|b| b.start_row <= row);
        self.blocks.get(idx).map(|b| b.start_row)
    }

    /// Start row of the last changed block that begins before `row`.
    #[must_use]
    pub fn prev_difference(&self, row: u64) -> Option<u64> {
        let idx = self.blocks.partition_point(|b| b.start_row < row);
        idx.checked_sub(1).map(|i| self.blocks[i].start_row)
    }
}

/// Incrementally assembles a [`DiffIndex`].
#[derive(Debug, Default)]
pub struct DiffIndexBuilder {
    segments: Vec<Segment>,
    rows: u64,
    left: u64,
    right: u64,
    degraded: bool,
}

impl DiffIndexBuilder {
    /// Mark that a coarse fallback was used.
    pub fn mark_degraded(&mut self) {
        self.degraded = true;
    }

    /// Append `len` rows of `kind`. Left/right cursors advance automatically.
    pub fn push(&mut self, kind: DiffType, mut len: u64) {
        while len > 0 {
            let chunk = len.min(u64::from(u32::MAX)) as u32;
            len -= u64::from(chunk);
            if let Some(last) = self.segments.last_mut() {
                if last.kind == kind && u64::from(last.len) + u64::from(chunk) <= u64::from(u32::MAX) {
                    last.len += chunk;
                    self.advance(kind, u64::from(chunk));
                    continue;
                }
            }
            self.segments.push(Segment {
                row_start: self.rows,
                left_start: self.left,
                right_start: self.right,
                len: chunk,
                kind,
            });
            self.advance(kind, u64::from(chunk));
        }
    }

    fn advance(&mut self, kind: DiffType, n: u64) {
        self.rows += n;
        if kind.has_left() {
            self.left += n;
        }
        if kind.has_right() {
            self.right += n;
        }
    }

    /// Finish; computes the changed blocks.
    #[must_use]
    pub fn finish(mut self) -> DiffIndex {
        self.segments.shrink_to_fit();
        let mut blocks: Vec<ChangeBlock> = Vec::new();
        for seg in &self.segments {
            if seg.kind == DiffType::Equal {
                continue;
            }
            match blocks.last_mut() {
                Some(b) if b.start_row + b.rows == seg.row_start => b.rows += u64::from(seg.len),
                _ => blocks.push(ChangeBlock { start_row: seg.row_start, rows: u64::from(seg.len) }),
            }
        }
        blocks.shrink_to_fit();
        DiffIndex {
            segments: self.segments,
            blocks,
            rows: self.rows,
            left_lines: self.left,
            right_lines: self.right,
            degraded: self.degraded,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> DiffIndex {
        // rows: E E | R D | E | I I | E
        let mut b = DiffIndex::builder();
        b.push(DiffType::Equal, 2);
        b.push(DiffType::Replace, 1);
        b.push(DiffType::Delete, 1);
        b.push(DiffType::Equal, 1);
        b.push(DiffType::Insert, 2);
        b.push(DiffType::Equal, 1);
        b.finish()
    }

    #[test]
    fn maps_rows_to_lines_and_back() {
        let d = sample();
        assert_eq!(d.row_count(), 8);
        assert_eq!(d.line_counts(), (6, 7));
        assert_eq!(d.row(2), Some(DiffRow { left_line: Some(2), right_line: Some(2), kind: DiffType::Replace }));
        assert_eq!(d.row(3), Some(DiffRow { left_line: Some(3), right_line: None, kind: DiffType::Delete }));
        assert_eq!(d.row(5), Some(DiffRow { left_line: None, right_line: Some(4), kind: DiffType::Insert }));
        assert_eq!(d.row(8), None);
        for l in 0..6 {
            let r = d.row_for_left_line(l).unwrap();
            assert_eq!(d.row(r).unwrap().left_line, Some(l));
        }
        for l in 0..7 {
            let r = d.row_for_right_line(l).unwrap();
            assert_eq!(d.row(r).unwrap().right_line, Some(l));
        }
        assert_eq!(d.row_for_left_line(6), None);
        assert_eq!(d.row_for_right_line(7), None);
    }

    #[test]
    fn navigation_and_blocks() {
        let d = sample();
        assert_eq!(d.blocks().len(), 2);
        assert_eq!(d.stats(), DiffStats { equal_rows: 4, inserted_rows: 2, deleted_rows: 1, replaced_rows: 1, blocks: 2 });
        assert_eq!(d.next_difference(0), Some(2));
        assert_eq!(d.next_difference(2), Some(5));
        assert_eq!(d.next_difference(5), None);
        assert_eq!(d.prev_difference(7), Some(5));
        assert_eq!(d.prev_difference(5), Some(2));
        assert_eq!(d.prev_difference(2), None);
        assert_eq!(d.block_containing(3), Some(0));
        assert_eq!(d.block_containing(4), None);
    }

    #[test]
    fn overview_marks_buckets_without_touching_lines() {
        let d = sample(); // 8 rows: E E R D E I I E
        assert_eq!(d.overview(8), vec![0, 0, 4, 2, 0, 1, 1, 0]);
        // Coarser: 4 buckets of 2 rows each.
        assert_eq!(d.overview(4), vec![0, 4 | 2, 1, 1]);
        assert_eq!(d.overview(0), Vec::<u8>::new());
        assert_eq!(DiffIndex::default().overview(5), vec![0; 5]);
    }

    #[test]
    fn identical_files_are_one_segment() {
        let mut b = DiffIndex::builder();
        b.push(DiffType::Equal, 10_000_000);
        let d = b.finish();
        assert!(d.is_identical());
        assert_eq!(d.segments().len(), 1);
        assert_eq!(d.row(9_999_999).unwrap().left_line, Some(9_999_999));
    }

    #[test]
    fn rows_beyond_u32_are_split_but_addressable() {
        let mut b = DiffIndex::builder();
        b.push(DiffType::Equal, u64::from(u32::MAX) * 2 + 5);
        let d = b.finish();
        assert_eq!(d.segments().len(), 3);
        let last = d.row_count() - 1;
        assert_eq!(d.row(last).unwrap().right_line, Some(last));
        assert_eq!(d.row_for_left_line(last), Some(last));
    }
}
