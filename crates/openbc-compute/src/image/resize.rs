use openbc_core::image_compare::RgbaImage;
use rayon::prelude::*;

use super::cancel::CancelToken;
use super::error::ImageComputeError;

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
    let too_large = ImageComputeError::TooLarge { width: dst_w, height: dst_h };
    if dst_w == 0 || dst_h == 0 || src.width() == 0 || src.height() == 0 {
        return Err(too_large);
    }
    let mut dst = RgbaImage::new(dst_w, dst_h).ok_or(too_large)?;
    let (sw, sh) = (src.width() as usize, src.height() as usize);
    let x_scale = sw as f32 / dst_w as f32;
    let y_scale = sh as f32 / dst_h as f32;

    // Horizontal taps are identical for every row: compute them once.
    let taps: Vec<(usize, usize, f32)> = (0..dst_w as usize)
        .map(|x| {
            let fx = ((x as f32 + 0.5) * x_scale - 0.5).max(0.0);
            let x0 = (fx.floor() as usize).min(sw - 1);
            (x0, (x0 + 1).min(sw - 1), fx - x0 as f32)
        })
        .collect();

    let src_bytes = src.as_bytes();
    let stride = sw * 4;
    dst.as_bytes_mut()
        .par_chunks_mut(dst_w as usize * 4)
        .enumerate()
        .try_for_each(|(y, row)| {
            if cancel.is_cancelled() {
                return Err(ImageComputeError::Cancelled);
            }
            let fy = ((y as f32 + 0.5) * y_scale - 0.5).max(0.0);
            let y0 = (fy.floor() as usize).min(sh - 1);
            let y1 = (y0 + 1).min(sh - 1);
            let wy = fy - y0 as f32;
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
}
