use openbc_core::image_compare::RgbaImage;
use rayon::prelude::*;

use super::cancel::CancelToken;
use super::error::ImageComputeError;

/// One bilinear sample position along an axis: `(low index, high index, weight of high)`.
/// Uses the pixel-centre convention (`+0.5` / `-0.5`), so equal sizes map to themselves.
fn tap(dst_index: usize, scale: f32, src_len: usize) -> (usize, usize, f32) {
    let f = ((dst_index as f32 + 0.5) * scale - 0.5).max(0.0);
    let lo = (f.floor() as usize).min(src_len - 1);
    (lo, (lo + 1).min(src_len - 1), f - lo as f32)
}

/// Bilinear resample, row-parallel. Pixel centres are aligned (the `+0.5/-0.5`
/// convention), so identical sizes are an exact copy.
///
/// Note: plain bilinear aliases on large downscales (>2x); an area filter is a
/// drop-in replacement here if that matters.
pub fn resize_bilinear(
    src: &RgbaImage,
    dst_w: u32,
    dst_h: u32,
    cancel: &CancelToken,
) -> Result<RgbaImage, ImageComputeError> {
    if dst_w == 0 || dst_h == 0 || src.width() == 0 || src.height() == 0 {
        return Err(ImageComputeError::too_large(dst_w, dst_h));
    }
    let mut dst = RgbaImage::new(dst_w, dst_h).ok_or_else(|| ImageComputeError::too_large(dst_w, dst_h))?;
    let (sw, sh) = (src.width() as usize, src.height() as usize);
    let x_scale = sw as f32 / dst_w as f32;
    let y_scale = sh as f32 / dst_h as f32;

    // Horizontal taps are identical for every row: compute them once.
    let taps: Vec<(usize, usize, f32)> = (0..dst_w as usize).map(|x| tap(x, x_scale, sw)).collect();

    let src_bytes = src.as_bytes();
    let stride = sw * 4;
    dst.as_bytes_mut()
        .par_chunks_mut(dst_w as usize * 4)
        .enumerate()
        .try_for_each(|(y, row)| {
            cancel.check()?;
            let (y0, y1, wy) = tap(y, y_scale, sh);
            let (r0, r1) = (&src_bytes[y0 * stride..(y0 + 1) * stride], &src_bytes[y1 * stride..(y1 + 1) * stride]);
            for (x, &(x0, x1, wx)) in taps.iter().enumerate() {
                for c in 0..4 {
                    let top = r0[x0 * 4 + c] as f32 * (1.0 - wx) + r0[x1 * 4 + c] as f32 * wx;
                    let bot = r1[x0 * 4 + c] as f32 * (1.0 - wx) + r1[x1 * 4 + c] as f32 * wx;
                    row[x * 4 + c] = (top * (1.0 - wy) + bot * wy + 0.5) as u8;
                }
            }
            Ok(())
        })?;
    Ok(dst)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn same_size_is_identity_and_constant_stays_constant() {
        let data: Vec<u8> = (0..4 * 4 * 4).map(|i| (i * 3) as u8).collect();
        let img = RgbaImage::from_raw(4, 4, data).unwrap();
        let same = resize_bilinear(&img, 4, 4, &CancelToken::new()).unwrap();
        assert_eq!(same, img);

        let flat = RgbaImage::from_raw(3, 5, vec![77; 3 * 5 * 4]).unwrap();
        let up = resize_bilinear(&flat, 9, 11, &CancelToken::new()).unwrap();
        assert!(up.as_bytes().iter().all(|&b| b == 77));
    }

    #[test]
    fn cancelled_job_reports_cancelled() {
        let img = RgbaImage::new(8, 8).unwrap();
        let c = CancelToken::new();
        c.cancel();
        assert_eq!(resize_bilinear(&img, 4, 4, &c), Err(ImageComputeError::Cancelled));
    }

    #[test]
    fn tap_clamps_at_both_edges() {
        // 10 -> 5 (downscale): last output pixel sits between source pixels 8 and 9.
        assert_eq!(tap(4, 2.0, 10), (8, 9, 0.5));
        // 10 -> 20 (upscale): first pixel clamps to the left edge, last to the right edge.
        assert_eq!(tap(0, 0.5, 10), (0, 1, 0.0));
        assert_eq!(tap(19, 0.5, 10), (9, 9, 0.25));
    }
}
