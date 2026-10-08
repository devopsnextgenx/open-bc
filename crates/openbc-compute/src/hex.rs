//! Byte-range comparison backend for hex compare.

use openbc_core::hex_compare::{diff_slices, push_run, ByteDiffRun};
use rayon::prelude::*;

/// Chunks at or above this size may use the GPU once a kernel exists (policy: 4 MiB).
pub const GPU_MIN_BYTES: usize = 4 * 1024 * 1024;
/// Chunks below this size never use the GPU.
pub const GPU_NEVER_BELOW_BYTES: usize = 1024 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ByteBackend {
    Cpu,
    Gpu,
}

/// Backend choice for a chunk. The wgpu byte-compare kernel is not implemented yet, so
/// this always resolves to the CPU; the thresholds are kept here so callers never branch.
pub fn select_backend(chunk_len: usize, _gpu_available: bool) -> ByteBackend {
    debug_assert!(GPU_NEVER_BELOW_BYTES < GPU_MIN_BYTES);
    let _ = chunk_len;
    ByteBackend::Cpu
}

/// Compares the common prefix of two chunks on Rayon; `base` is the file offset of byte 0.
pub fn compare_chunk(left: &[u8], right: &[u8], base: u64) -> Vec<ByteDiffRun> {
    const SUB: usize = 256 * 1024;
    let n = left.len().min(right.len());
    let starts: Vec<usize> = (0..n).step_by(SUB).collect();
    let parts: Vec<Vec<ByteDiffRun>> = starts
        .par_iter()
        .map(|&s| {
            let e = (s + SUB).min(n);
            let mut runs = Vec::new();
            diff_slices(&left[s..e], &right[s..e], base + s as u64, &mut runs);
            runs
        })
        .collect();
    let mut out = Vec::new();
    for run in parts.into_iter().flatten() {
        push_run(&mut out, run.offset, run.len);
    }
    out
}
