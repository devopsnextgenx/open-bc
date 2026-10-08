//! Byte-level comparison primitives for hex compare (same-offset comparison).

/// Cap on stored runs so a pathological pair of files cannot exhaust memory.
pub const MAX_RUNS: usize = 1_000_000;

/// Block size used to skip equal regions quickly before scanning byte by byte.
const BLOCK: usize = 64;

/// A maximal range of differing bytes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ByteDiffRun {
    /// File offset of the first differing byte.
    pub offset: u64,
    /// Number of consecutive differing bytes.
    pub len: u64,
}

/// Appends a run, merging it into the previous one when adjacent.
pub fn push_run(out: &mut Vec<ByteDiffRun>, offset: u64, len: u64) {
    if let Some(last) = out.last_mut() {
        if last.offset + last.len == offset {
            last.len += len;
            return;
        }
    }
    out.push(ByteDiffRun { offset, len });
}

/// Compares the common prefix of two slices; `base` is the file offset of index 0.
pub fn diff_slices(left: &[u8], right: &[u8], base: u64, out: &mut Vec<ByteDiffRun>) {
    let n = left.len().min(right.len());
    let mut i = 0;
    while i < n {
        let end = (i + BLOCK).min(n);
        if left[i..end] == right[i..end] {
            i = end;
            continue;
        }
        while i < end {
            if left[i] == right[i] {
                i += 1;
                continue;
            }
            let start = i;
            while i < n && left[i] != right[i] {
                i += 1;
            }
            push_run(out, base + start as u64, (i - start) as u64);
        }
    }
}

/// The bytes present in only the longer file, as a run.
pub fn tail_run(left_len: u64, right_len: u64) -> Option<ByteDiffRun> {
    let common = left_len.min(right_len);
    (left_len != right_len).then(|| ByteDiffRun { offset: common, len: left_len.max(right_len) - common })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_and_merges_runs() {
        let a = [1u8, 2, 3, 4, 5, 6];
        let b = [1u8, 9, 9, 4, 5, 0];
        let mut runs = Vec::new();
        diff_slices(&a, &b, 100, &mut runs);
        assert_eq!(runs, vec![ByteDiffRun { offset: 101, len: 2 }, ByteDiffRun { offset: 105, len: 1 }]);
        assert_eq!(tail_run(6, 10), Some(ByteDiffRun { offset: 6, len: 4 }));
    }

    #[test]
    fn merges_runs_across_block_boundaries() {
        let a = vec![0u8; 200];
        let mut b = a.clone();
        b[62..66].fill(1); // straddles the 64-byte block edge
        let mut runs = Vec::new();
        diff_slices(&a, &b, 0, &mut runs);
        assert_eq!(runs, vec![ByteDiffRun { offset: 62, len: 4 }]);
        assert_eq!(tail_run(5, 5), None);
    }
}
