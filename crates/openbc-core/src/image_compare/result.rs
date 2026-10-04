use serde::{Deserialize, Serialize};

use super::align::Rect;
use super::buffer::RgbaImage;
use super::options::ImageCompareOptions;
use super::pixel::{classify, PixelClass};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct DiffStats {
    pub compared_pixels: u64,
    pub identical_pixels: u64,
    /// 0 < delta <= tolerance
    pub similar_pixels: u64,
    /// delta > tolerance
    pub different_pixels: u64,
    pub sum_delta: u64,
    pub max_delta: u8,
}

impl DiffStats {
    #[inline]
    pub fn record(&mut self, delta: u8, tolerance: u8) {
        self.compared_pixels += 1;
        match classify(delta, tolerance) {
            PixelClass::Identical => self.identical_pixels += 1,
            PixelClass::Similar => self.similar_pixels += 1,
            PixelClass::Different => self.different_pixels += 1,
        }
        self.sum_delta += delta as u64;
        self.max_delta = self.max_delta.max(delta);
    }

    pub fn merge(self, o: Self) -> Self {
        Self {
            compared_pixels: self.compared_pixels + o.compared_pixels,
            identical_pixels: self.identical_pixels + o.identical_pixels,
            similar_pixels: self.similar_pixels + o.similar_pixels,
            different_pixels: self.different_pixels + o.different_pixels,
            sum_delta: self.sum_delta + o.sum_delta,
            max_delta: self.max_delta.max(o.max_delta),
        }
    }

    pub fn mean_delta(&self) -> f64 {
        if self.compared_pixels == 0 {
            0.0
        } else {
            self.sum_delta as f64 / self.compared_pixels as f64
        }
    }
}

/// Status-bar verdict ("Binary same", "Important differences", ...).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum Verdict {
    /// Encoded files are byte-identical (set by the engine).
    BinarySame,
    /// Different bytes, identical pixels over the whole canvas.
    PixelsSame,
    /// Every pixel is within tolerance.
    Similar,
    /// Some pixel exceeds tolerance, or the canvas is not fully covered.
    Different,
}

impl Verdict {
    pub fn from_stats(stats: &DiffStats, full_overlap: bool) -> Self {
        if !full_overlap || stats.different_pixels > 0 {
            Verdict::Different
        } else if stats.similar_pixels > 0 {
            Verdict::Similar
        } else {
            Verdict::PixelsSame
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum BackendKind {
    Cpu,
    Gpu,
}

/// Result of one compare pass.
#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct ImageCompareOutput {
    /// Canvas-sized visualisation for the active mode.
    pub overlay: RgbaImage,
    pub stats: DiffStats,
    pub verdict: Verdict,
    pub backend: BackendKind,
    pub overlap: Option<Rect>,
    /// Size of the right image after the resize policy (canvas = `overlay` size).
    /// Lets a UI map coordinates between a native-size preview and the canvas.
    pub right_size: (u32, u32),
    /// The sanitized options this output was computed with.
    pub options: ImageCompareOptions,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn verdicts() {
        let mut s = DiffStats::default();
        s.record(0, 25);
        assert_eq!(Verdict::from_stats(&s, true), Verdict::PixelsSame);
        s.record(10, 25);
        assert_eq!(Verdict::from_stats(&s, true), Verdict::Similar);
        s.record(40, 25);
        assert_eq!(Verdict::from_stats(&s, true), Verdict::Different);
        assert_eq!(Verdict::from_stats(&DiffStats::default(), false), Verdict::Different);
        assert_eq!(s.max_delta, 40);
        assert_eq!(s.merge(s).compared_pixels, 6);
    }
}
