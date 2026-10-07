use openbc_core::image_compare::{AlignedPair, DiffStats, ImageCompareOptions, PixelRenderer, RgbaImage};
use rayon::prelude::*;

use super::cancel::CancelToken;
use super::error::ImageComputeError;

/// Reference CPU backend. Colours every canvas pixel with the core
/// [`PixelRenderer`], one Rayon task per row, accumulating [`DiffStats`].
pub fn compare_cpu(
    pair: &AlignedPair,
    options: &ImageCompareOptions,
    cancel: &CancelToken,
) -> Result<(RgbaImage, DiffStats), ImageComputeError> {
    let (cw, ch) = pair.canvas();
    if cw == 0 || ch == 0 {
        return Err(ImageComputeError::TooLarge { width: cw, height: ch }); // par_chunks_mut(0) would panic
    }
    let mut overlay =
        RgbaImage::new(cw, ch).ok_or(ImageComputeError::TooLarge { width: cw, height: ch })?;
    let renderer = options.renderer();
    let tolerance = options.tolerance;

    let stats = overlay
        .as_bytes_mut()
        .par_chunks_mut(cw as usize * 4)
        .enumerate()
        .map(|(y, row)| -> Result<DiffStats, ImageComputeError> {
            if cancel.is_cancelled() {
                return Err(ImageComputeError::Cancelled);
            }
            Ok(render_row(pair, &renderer, tolerance, y as u32, row))
        })
        .try_reduce(DiffStats::default, |a, b| Ok(a.merge(b)))?;
    Ok((overlay, stats))
}

fn render_row(pair: &AlignedPair, renderer: &PixelRenderer, tolerance: u8, y: u32, out: &mut [u8]) -> DiffStats {
    let mut stats = DiffStats::default();
    let left_row = pair.left.row(y);
    let (ox, oy) = pair.right_offset;
    let ry = y as i64 - oy as i64;
    let right = &pair.right;
    let right_row = (ry >= 0 && ry < right.height() as i64).then(|| right.row(ry as u32));
    let rw = right.width() as i64;

    for (x, dst) in out.chunks_exact_mut(4).enumerate() {
        let l = [left_row[x * 4], left_row[x * 4 + 1], left_row[x * 4 + 2], left_row[x * 4 + 3]];
        let rx = x as i64 - ox as i64;
        let r = right_row.and_then(|rr| {
            (rx >= 0 && rx < rw).then(|| {
                let i = rx as usize * 4;
                [rr[i], rr[i + 1], rr[i + 2], rr[i + 3]]
            })
        });
        let (delta, color) = renderer.render(l, r);
        if let Some(d) = delta {
            stats.record(d, tolerance);
        }
        dst.copy_from_slice(&color);
    }
    stats
}

#[cfg(test)]
mod tests {
    use super::*;
    use openbc_core::image_compare::{CompareMode, DIFFERENT_COLOR, SIMILAR_COLOR};
    use std::sync::Arc;

    fn img(w: u32, h: u32, f: impl Fn(u32, u32) -> [u8; 4]) -> Arc<RgbaImage> {
        let mut data = Vec::new();
        for y in 0..h {
            for x in 0..w {
                data.extend_from_slice(&f(x, y));
            }
        }
        Arc::new(RgbaImage::from_raw(w, h, data).unwrap())
    }

    #[test]
    fn tolerance_classifies_each_pixel() {
        let left = img(3, 1, |_, _| [100, 100, 100, 255]);
        let right = img(3, 1, |x, _| [100 + [0u8, 10, 90][x as usize], 100, 100, 255]);
        let pair = AlignedPair { left, right, right_offset: (0, 0) };
        let opts = ImageCompareOptions { tolerance: 25, ..Default::default() };
        let (out, stats) = compare_cpu(&pair, &opts, &CancelToken::new()).unwrap();
        assert_eq!(out.pixel(1, 0), SIMILAR_COLOR);
        assert_eq!(out.pixel(2, 0), DIFFERENT_COLOR);
        assert_eq!((stats.identical_pixels, stats.similar_pixels, stats.different_pixels), (1, 1, 1));
        assert_eq!(stats.max_delta, 90);
    }

    #[test]
    fn offset_limits_compared_area() {
        let left = img(10, 10, |_, _| [1, 2, 3, 255]);
        let right = img(4, 4, |_, _| [1, 2, 3, 255]);
        let pair = AlignedPair { left, right, right_offset: (8, 8) };
        let (out, stats) = compare_cpu(&pair, &ImageCompareOptions::default(), &CancelToken::new()).unwrap();
        assert_eq!(stats.compared_pixels, 4); // only the 2x2 overlap
        assert_eq!(out.pixel(0, 0), [1, 2, 3, 255]); // outside overlap = left, untouched
    }

    #[test]
    fn blend_and_range_modes() {
        let left = img(1, 1, |_, _| [0, 0, 0, 255]);
        let right = img(1, 1, |_, _| [200, 0, 0, 255]);
        let pair = AlignedPair { left, right, right_offset: (0, 0) };
        let blend = ImageCompareOptions { mode: CompareMode::Blend, blend_percent: 50, ..Default::default() };
        assert_eq!(compare_cpu(&pair, &blend, &CancelToken::new()).unwrap().0.pixel(0, 0)[0], 100);
        let range = ImageCompareOptions { mode: CompareMode::Range, ..Default::default() };
        assert_eq!(compare_cpu(&pair, &range, &CancelToken::new()).unwrap().0.pixel(0, 0)[2], 0);
    }

    #[test]
    fn cancelled_before_start() {
        let a = img(2, 2, |_, _| [0; 4]);
        let pair = AlignedPair { left: a.clone(), right: a, right_offset: (0, 0) };
        let c = CancelToken::new();
        c.cancel();
        assert_eq!(
            compare_cpu(&pair, &ImageCompareOptions::default(), &c).unwrap_err(),
            ImageComputeError::Cancelled
        );
    }
}
