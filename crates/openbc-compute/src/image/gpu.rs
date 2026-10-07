//! GPU contract for image compare.
//!
//! The compute crate's existing wgpu backend implements [`GpuImageExecutor`]
//! (upload `left`/`right`, bind them with a [`GpuCompareParams`] uniform, run
//! [`IMAGE_COMPARE_WGSL`], read back `overlay` + the 7-word stats buffer).
//! Keeping the contract here means no wgpu type crosses into callers and this
//! file compiles without wgpu.

use openbc_core::image_compare::{CompareMode, DiffStats, ImageCompareOptions};

/// WGSL source mirroring `PixelRenderer` bit-for-bit.
pub const IMAGE_COMPARE_WGSL: &str = include_str!("image_compare.wgsl");

/// Uniform block, 16 x 32-bit words, matching `struct Params` in the shader.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GpuCompareParams {
    pub canvas: (u32, u32),
    pub right_size: (u32, u32),
    pub offset: (i32, i32),
    pub mode: u32,
    pub tolerance: u32,
    pub range_min: u32,
    pub range_max: u32,
    pub weight256: u32,
    pub compare_alpha: u32,
}

impl GpuCompareParams {
    pub const WORDS: usize = 16;

    pub fn new(canvas: (u32, u32), right_size: (u32, u32), options: &ImageCompareOptions) -> Self {
        Self {
            canvas,
            right_size,
            offset: options.offset.tuple(),
            mode: match options.mode {
                CompareMode::Tolerance => 0,
                CompareMode::Range => 1,
                CompareMode::Blend => 2,
            },
            tolerance: options.tolerance as u32,
            range_min: options.range.min as u32,
            range_max: options.range.max as u32,
            weight256: options.blend_weight_256(),
            compare_alpha: options.compare_alpha as u32,
        }
    }

    pub fn to_words(&self) -> [u32; Self::WORDS] {
        [
            self.canvas.0,
            self.canvas.1,
            self.right_size.0,
            self.right_size.1,
            self.offset.0 as u32,
            self.offset.1 as u32,
            self.mode,
            self.tolerance,
            self.range_min,
            self.range_max,
            self.weight256,
            self.compare_alpha,
            0,
            0,
            0,
            0,
        ]
    }
}

/// Raw RGBA8 inputs, row-major, tightly packed.
pub struct GpuImageJob<'a> {
    pub left: &'a [u8],
    pub right: &'a [u8],
    pub params: GpuCompareParams,
}

pub struct GpuImageOutput {
    /// Canvas-sized RGBA8, same layout as the CPU overlay.
    pub overlay: Vec<u8>,
    pub stats: DiffStats,
}

impl GpuImageOutput {
    /// Decode the shader's 7-word stats buffer.
    pub fn stats_from_words(w: &[u32; 7]) -> DiffStats {
        DiffStats {
            compared_pixels: w[0] as u64,
            identical_pixels: w[1] as u64,
            similar_pixels: w[2] as u64,
            different_pixels: w[3] as u64,
            sum_delta: (w[5] as u64) << 32 | w[4] as u64,
            max_delta: w[6].min(255) as u8,
        }
    }
}

/// Implemented by the GPU backend. Errors make the dispatcher fall back to the
/// CPU, so a flaky adapter never affects correctness.
pub trait GpuImageExecutor: Send + Sync {
    fn compare(&self, job: &GpuImageJob<'_>) -> Result<GpuImageOutput, String>;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn params_layout_and_stats_decode() {
        let p = GpuCompareParams::new((10, 20), (5, 6), &ImageCompareOptions { offset: openbc_core::image_compare::PixelOffset { x: -3, y: 4 }, ..Default::default() });
        let w = p.to_words();
        assert_eq!(&w[..4], &[10, 20, 5, 6]);
        assert_eq!(w[4] as i32, -3);
        assert_eq!(w[10], 128);
        let s = GpuImageOutput::stats_from_words(&[4, 1, 1, 2, 5, 1, 90]);
        assert_eq!(s.sum_delta, (1u64 << 32) + 5);
        assert_eq!(s.max_delta, 90);
        assert!(IMAGE_COMPARE_WGSL.contains("@compute"));
    }
}
