//! Image compare domain: owned pixel buffers, option/result contracts and the
//! *reference* per-pixel semantics every compute backend must reproduce.
//!
//! Everything here is pure and dependency-light (serde only). Backends in
//! `openbc-compute` (Rayon today, GPU later) must be bit-exact with
//! [`PixelRenderer`]; the integer-only math is deliberate so that parity is
//! testable.

mod align;
mod buffer;
mod options;
mod pixel;
mod probe;
mod result;

pub use align::{overlap_rect, plan_resize, Rect, ResizePlan};
pub use buffer::{ImageMeta, RgbaImage};
pub use options::{
    CompareMode, DeltaRange, ImageCompareOptions, PixelOffset, ResizePolicy, MAX_OFFSET,
};
pub use pixel::{
    blend_pixel, channel_delta, classify, render_range, washed_gray, PixelClass, PixelRenderer,
    DIFFERENT_COLOR, RANGE_BACKGROUND, SIMILAR_COLOR,
};
pub use probe::{AlignedPair, PixelProbe, PROBE_CELLS, PROBE_RADIUS, PROBE_SIZE};
pub use result::{BackendKind, DiffStats, ImageCompareOutput, Verdict};
