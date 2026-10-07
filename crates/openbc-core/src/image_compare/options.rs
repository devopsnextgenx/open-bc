use serde::{Deserialize, Serialize};

use super::pixel::PixelRenderer;

/// Offsets are clamped to this magnitude so index math can never overflow.
pub const MAX_OFFSET: i32 = 1 << 20;

/// Visualisation mode, matching the Tol / Range / Blend buttons of the tab.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum CompareMode {
    /// Identical = washed gray, within tolerance = blue, beyond = red.
    #[default]
    Tolerance,
    /// Per-pixel difference magnitude mapped through a ramp over `range`.
    Range,
    /// Linear blend of both images (`blend_percent` = weight of the right one).
    Blend,
}

/// How the two images are brought to a common canvas. The canvas is always the
/// (possibly resized) *left* image.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum ResizePolicy {
    /// Scale the right image to the left one when aspect ratios match (within
    /// 1%), otherwise keep native sizes.
    #[default]
    Auto,
    /// Never resample; `offset` positions the right image on the left canvas.
    Native,
    FitToLeft,
    FitToRight,
    FitToLarger,
    FitToSmaller,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct PixelOffset {
    pub x: i32,
    pub y: i32,
}

impl PixelOffset {
    pub const ZERO: Self = Self { x: 0, y: 0 };
    pub fn negated(self) -> Self {
        Self { x: self.x.saturating_neg(), y: self.y.saturating_neg() }
    }
    pub fn tuple(self) -> (i32, i32) {
        (self.x, self.y)
    }
}

/// Inclusive window of per-pixel deltas (0..=255) shown by [`CompareMode::Range`].
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct DeltaRange {
    pub min: u8,
    pub max: u8,
}

impl Default for DeltaRange {
    fn default() -> Self {
        Self { min: 0, max: 255 }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct ImageCompareOptions {
    pub mode: CompareMode,
    /// Largest per-channel difference still considered "similar" (0..=255).
    pub tolerance: u8,
    pub range: DeltaRange,
    /// Weight of the right image in Blend mode, 0..=100.
    pub blend_percent: u8,
    /// Position of the right image relative to the left canvas.
    pub offset: PixelOffset,
    pub resize: ResizePolicy,
    /// Include the alpha channel in the delta.
    pub compare_alpha: bool,
}

impl Default for ImageCompareOptions {
    fn default() -> Self {
        Self {
            mode: CompareMode::Tolerance,
            tolerance: 25,
            range: DeltaRange::default(),
            blend_percent: 50,
            offset: PixelOffset::ZERO,
            resize: ResizePolicy::Auto,
            compare_alpha: false,
        }
    }
}

impl ImageCompareOptions {
    /// Clamp everything into the domain the kernels assume.
    pub fn sanitized(mut self) -> Self {
        self.blend_percent = self.blend_percent.min(100);
        if self.range.min > self.range.max {
            std::mem::swap(&mut self.range.min, &mut self.range.max);
        }
        self.offset.x = self.offset.x.clamp(-MAX_OFFSET, MAX_OFFSET);
        self.offset.y = self.offset.y.clamp(-MAX_OFFSET, MAX_OFFSET);
        self
    }

    /// Weight of the right image in 1/256ths (0..=256), integer-rounded.
    pub fn blend_weight_256(&self) -> u32 {
        (self.blend_percent.min(100) as u32 * 256 + 50) / 100
    }

    pub fn renderer(&self) -> PixelRenderer {
        PixelRenderer {
            mode: self.mode,
            tolerance: self.tolerance,
            range_min: self.range.min,
            range_max: self.range.max,
            weight256: self.blend_weight_256(),
            compare_alpha: self.compare_alpha,
        }
    }
}
