//! Reference per-pixel semantics. Integer math only so CPU and GPU agree
//! bit-for-bit.

use super::options::CompareMode;

pub const SIMILAR_COLOR: [u8; 4] = [0, 0, 255, 255];
pub const DIFFERENT_COLOR: [u8; 4] = [255, 0, 0, 255];
pub const RANGE_BACKGROUND: [u8; 4] = [0, 0, 0, 255];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PixelClass {
    Identical,
    Similar,
    Different,
}

/// Largest absolute per-channel difference (RGB, plus alpha if requested).
#[inline]
pub fn channel_delta(a: [u8; 4], b: [u8; 4], compare_alpha: bool) -> u8 {
    let mut d = a[0].abs_diff(b[0]).max(a[1].abs_diff(b[1])).max(a[2].abs_diff(b[2]));
    if compare_alpha {
        d = d.max(a[3].abs_diff(b[3]));
    }
    d
}

#[inline]
pub fn classify(delta: u8, tolerance: u8) -> PixelClass {
    if delta == 0 {
        PixelClass::Identical
    } else if delta <= tolerance {
        PixelClass::Similar
    } else {
        PixelClass::Different
    }
}

/// Desaturated, lifted gray used for identical pixels in Tolerance mode.
#[inline]
pub fn washed_gray(p: [u8; 4]) -> [u8; 4] {
    let lum = (77 * p[0] as u32 + 150 * p[1] as u32 + 29 * p[2] as u32) >> 8;
    let v = ((lum >> 1) + 64) as u8;
    [v, v, v, 255]
}

/// `weight256` is the right image's weight in 1/256ths (0..=256).
#[inline]
pub fn blend_pixel(l: [u8; 4], r: [u8; 4], weight256: u32) -> [u8; 4] {
    let mix = |a: u8, b: u8| ((a as u32 * (256 - weight256) + b as u32 * weight256 + 128) >> 8) as u8;
    [mix(l[0], r[0]), mix(l[1], r[1]), mix(l[2], r[2]), mix(l[3], r[3])]
}

/// Dark-to-yellow ramp over `[min, max]`. Zero deltas and deltas below `min`
/// are black; anything inside the window is at least visibly dim.
#[inline]
pub fn render_range(delta: u8, min: u8, max: u8) -> [u8; 4] {
    if delta == 0 || delta < min {
        return RANGE_BACKGROUND;
    }
    // A collapsed window (min >= max) shows everything at or above it at full brightness.
    let t = if max <= min {
        207
    } else {
        let clipped = delta.min(max);
        (((clipped - min) as u32 * 207) / (max - min) as u32).min(207)
    };
    let v = (48 + t) as u8;
    [v, v, 0, 255]
}

/// Everything a backend needs to colour one canvas pixel.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PixelRenderer {
    pub mode: CompareMode,
    pub tolerance: u8,
    pub range_min: u8,
    pub range_max: u8,
    pub weight256: u32,
    pub compare_alpha: bool,
}

impl PixelRenderer {
    /// `right = None` means the right image does not cover this pixel.
    /// Returns the delta (when overlapping) and the output colour.
    #[inline]
    pub fn render(&self, left: [u8; 4], right: Option<[u8; 4]>) -> (Option<u8>, [u8; 4]) {
        let Some(r) = right else {
            return match self.mode {
                CompareMode::Range => (None, RANGE_BACKGROUND),
                _ => (None, left),
            };
        };
        let delta = channel_delta(left, r, self.compare_alpha);
        let color = match self.mode {
            CompareMode::Tolerance => match classify(delta, self.tolerance) {
                PixelClass::Identical => washed_gray(left),
                PixelClass::Similar => SIMILAR_COLOR,
                PixelClass::Different => DIFFERENT_COLOR,
            },
            CompareMode::Range => render_range(delta, self.range_min, self.range_max),
            CompareMode::Blend => blend_pixel(left, r, self.weight256),
        };
        (Some(delta), color)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn delta_ignores_alpha_unless_asked() {
        assert_eq!(channel_delta([10, 10, 10, 0], [10, 10, 10, 255], false), 0);
        assert_eq!(channel_delta([10, 10, 10, 0], [10, 10, 10, 255], true), 255);
        assert_eq!(channel_delta([0, 50, 0, 255], [9, 0, 0, 255], false), 50);
    }

    #[test]
    fn classification_boundaries() {
        assert_eq!(classify(0, 0), PixelClass::Identical);
        assert_eq!(classify(1, 0), PixelClass::Different);
        assert_eq!(classify(25, 25), PixelClass::Similar);
        assert_eq!(classify(26, 25), PixelClass::Different);
    }

    #[test]
    fn blend_endpoints_are_exact() {
        let (a, b) = ([10, 200, 7, 255], [250, 0, 99, 128]);
        assert_eq!(blend_pixel(a, b, 0), a);
        assert_eq!(blend_pixel(a, b, 256), b);
        assert_eq!(blend_pixel([0; 4], [255; 4], 128), [128; 4]);
    }

    #[test]
    fn range_ramp_is_monotonic_and_bounded() {
        assert_eq!(render_range(0, 0, 255), RANGE_BACKGROUND);
        assert_eq!(render_range(9, 10, 20), RANGE_BACKGROUND);
        let mut last = 0;
        for d in 1..=255u8 {
            let v = render_range(d, 1, 255)[0];
            assert!(v >= last && v >= 48);
            last = v;
        }
        assert_eq!(render_range(255, 1, 255)[0], 255);
        assert_eq!(render_range(200, 10, 10)[0], 255); // degenerate window
    }

    #[test]
    fn renderer_outside_overlap() {
        let mut r = crate::image_compare::ImageCompareOptions::default().renderer();
        assert_eq!(r.render([1, 2, 3, 255], None), (None, [1, 2, 3, 255]));
        r.mode = CompareMode::Range;
        assert_eq!(r.render([1, 2, 3, 255], None), (None, RANGE_BACKGROUND));
    }
}
