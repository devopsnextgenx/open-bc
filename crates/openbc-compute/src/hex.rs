//! Byte-range comparison backend for hex compare.

use openbc_core::hex_compare::{diff_slices, push_run, ByteDiffRun};
use rayon::prelude::*;

use crate::BackendKind;

/// Chunks at or above this size may use the GPU once a kernel exists (policy: 4 MiB).
pub use crate::GPU_MIN_BYTES;
/// Chunks below this size never use the GPU.
pub const GPU_NEVER_BELOW_BYTES: usize = crate::GPU_HARD_FLOOR_BYTES;

/// Same two-way CPU/GPU enum the hashing dispatcher reports.
pub type ByteBackend = BackendKind;

/// Granularity of the Rayon split inside one chunk.
const SUB_CHUNK_BYTES: usize = 256 * 1024;

/// Backend choice for a chunk. The wgpu byte-compare kernel is not implemented yet, so
/// this always resolves to the CPU; the thresholds are kept here so callers never branch.
pub fn select_backend(chunk_len: usize, _gpu_available: bool) -> ByteBackend {
    debug_assert!(GPU_NEVER_BELOW_BYTES < GPU_MIN_BYTES);
    let _ = chunk_len;
    ByteBackend::Cpu
}

/// Compares the common prefix of two chunks on Rayon; `base` is the file offset of byte 0.
pub fn compare_chunk(left: &[u8], right: &[u8], base: u64) -> Vec<ByteDiffRun> {
    let n = left.len().min(right.len());
    let starts: Vec<usize> = (0..n).step_by(SUB_CHUNK_BYTES).collect();
    let parts: Vec<Vec<ByteDiffRun>> = starts
        .par_iter()
        .map(|&s| {
            let e = (s + SUB_CHUNK_BYTES).min(n);
            let mut runs = Vec::new();
            diff_slices(&left[s..e], &right[s..e], base + s as u64, &mut runs);
            runs
        })
        .collect();
    // Runs that touch across a sub-chunk boundary are merged by `push_run`.
    let mut out = Vec::new();
    for run in parts.into_iter().flatten() {
        push_run(&mut out, run.offset, run.len);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn identical_chunks_have_no_runs() {
        assert!(compare_chunk(&[7; 1000], &[7; 1000], 0).is_empty());
    }

    #[test]
    fn a_single_difference_is_reported_at_its_file_offset() {
        let left = vec![0u8; 64];
        let mut right = left.clone();
        right[10] = 1;
        let runs = compare_chunk(&left, &right, 1000);
        assert_eq!(runs.len(), 1);
        assert_eq!(runs[0].offset, 1010);
    }

    #[test]
    fn gpu_is_never_selected_yet() {
        assert_eq!(select_backend(usize::MAX, true), ByteBackend::Cpu);
    }
}
