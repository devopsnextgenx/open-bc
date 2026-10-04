use std::sync::Arc;

use super::align::{overlap_rect, Rect};
use super::buffer::RgbaImage;

pub const PROBE_RADIUS: i32 = 4;
pub const PROBE_SIZE: usize = 9;
pub const PROBE_CELLS: usize = PROBE_SIZE * PROBE_SIZE;

/// 9x9 neighbourhoods around one canvas coordinate, row-major. `None` = the
/// cell lies outside that image.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PixelProbe {
    pub x: i32,
    pub y: i32,
    pub left: [Option<[u8; 4]>; PROBE_CELLS],
    pub right: [Option<[u8; 4]>; PROBE_CELLS],
}

/// Both images in canvas space. `left` defines the canvas; `right` has already
/// been resized by the policy and sits at `right_offset`. Cheap to clone and
/// share (Arc) so the UI can probe without touching the compare pipeline.
#[derive(Clone, Debug)]
pub struct AlignedPair {
    pub left: Arc<RgbaImage>,
    pub right: Arc<RgbaImage>,
    pub right_offset: (i32, i32),
}

impl AlignedPair {
    pub fn canvas(&self) -> (u32, u32) {
        self.left.dimensions()
    }

    pub fn overlap(&self) -> Option<Rect> {
        overlap_rect(self.canvas(), self.right.dimensions(), self.right_offset)
    }

    /// True when the right image covers the entire canvas (no uncompared area
    /// on the canvas; the right image may still extend beyond it).
    pub fn full_overlap(&self) -> bool {
        let (w, h) = self.canvas();
        self.overlap() == Some(Rect { x0: 0, y0: 0, x1: w, y1: h })
    }

    pub fn probe(&self, x: i32, y: i32) -> PixelProbe {
        let mut out = PixelProbe { x, y, left: [None; PROBE_CELLS], right: [None; PROBE_CELLS] };
        let (ox, oy) = self.right_offset;
        for dy in -PROBE_RADIUS..=PROBE_RADIUS {
            for dx in -PROBE_RADIUS..=PROBE_RADIUS {
                let cx = x as i64 + dx as i64;
                let cy = y as i64 + dy as i64;
                let idx = ((dy + PROBE_RADIUS) as usize) * PROBE_SIZE + (dx + PROBE_RADIUS) as usize;
                out.left[idx] = self.left.get(cx, cy);
                out.right[idx] = self.right.get(cx - ox as i64, cy - oy as i64);
            }
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn solid(w: u32, h: u32, v: u8) -> Arc<RgbaImage> {
        Arc::new(RgbaImage::from_raw(w, h, vec![v; (w * h * 4) as usize]).unwrap())
    }

    #[test]
    fn probe_respects_offset_and_edges() {
        let pair = AlignedPair { left: solid(20, 20, 1), right: solid(10, 10, 2), right_offset: (5, 5) };
        let p = pair.probe(0, 0);
        let center = PROBE_RADIUS as usize * PROBE_SIZE + PROBE_RADIUS as usize;
        assert_eq!(p.left[center], Some([1; 4]));
        assert_eq!(p.right[center], None); // (0,0) is left of the right image
        assert_eq!(p.left[0], None); // (-4,-4) is outside the canvas
        let p = pair.probe(5, 5);
        assert_eq!(p.right[center], Some([2; 4]));
        assert!(!pair.full_overlap());
    }
}
