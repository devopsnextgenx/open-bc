//! Align (resize + offset) and compare: the blocking pipeline behind `ImageCompute::compare_async`.

use std::sync::Arc;

use openbc_core::image_compare::{
    plan_resize, AlignedPair, BackendKind, ImageCompareOptions, ImageCompareOutput, RgbaImage, Verdict,
};

use super::cancel::CancelToken;
use super::dispatch::CompareDone;
use super::error::ImageComputeError;
use super::gpu::{GpuCompareParams, GpuImageExecutor, GpuImageJob, GpuImageOutput};
use super::kernel::compare_cpu;
use super::policy::ImageComputePolicy;
use super::resize::resize_bilinear;

/// Resizes `img` to `target`, sharing the allocation when it already fits.
fn fit(img: &Arc<RgbaImage>, target: (u32, u32), cancel: &CancelToken) -> Result<Arc<RgbaImage>, ImageComputeError> {
    if img.dimensions() == target {
        Ok(img.clone())
    } else {
        resize_bilinear(img, target.0, target.1, cancel).map(Arc::new)
    }
}

/// GPU attempt. `None` means "use the CPU": no executor, not eligible, the executor failed,
/// or it returned a wrongly sized overlay. A flaky adapter never affects correctness.
fn try_gpu(
    gpu: Option<&Arc<dyn GpuImageExecutor>>,
    policy: &ImageComputePolicy,
    pair: &AlignedPair,
    options: &ImageCompareOptions,
) -> Option<GpuImageOutput> {
    let gpu = gpu?;
    let canvas = pair.canvas();
    let canvas_bytes = RgbaImage::byte_len(canvas.0, canvas.1).unwrap_or(usize::MAX) as u64;
    let right_bytes = pair.right.as_bytes().len() as u64;
    if !policy.gpu_eligible(canvas_bytes, right_bytes) {
        return None;
    }
    let job = GpuImageJob {
        left: pair.left.as_bytes(),
        right: pair.right.as_bytes(),
        params: GpuCompareParams::new(canvas, pair.right.dimensions(), options),
    };
    gpu.compare(&job).ok().filter(|o| o.overlay.len() as u64 == canvas_bytes)
}

pub(super) fn compare_blocking(
    gpu: Option<Arc<dyn GpuImageExecutor>>,
    policy: ImageComputePolicy,
    left: Arc<RgbaImage>,
    right: Arc<RgbaImage>,
    options: ImageCompareOptions,
    cancel: &CancelToken,
) -> Result<CompareDone, ImageComputeError> {
    let options = options.sanitized();
    let plan = plan_resize(left.dimensions(), right.dimensions(), options.resize);
    let pair = Arc::new(AlignedPair {
        left: fit(&left, plan.left_target, cancel)?,
        right: fit(&right, plan.right_target, cancel)?,
        right_offset: options.offset.tuple(),
    });
    cancel.check()?;

    let canvas = pair.canvas();
    let (overlay, stats, backend) = match try_gpu(gpu.as_ref(), &policy, &pair, &options) {
        Some(o) => (
            RgbaImage::from_raw(canvas.0, canvas.1, o.overlay)
                .ok_or_else(|| ImageComputeError::too_large(canvas.0, canvas.1))?,
            o.stats,
            BackendKind::Gpu,
        ),
        None => {
            let (overlay, stats) = compare_cpu(&pair, &options, cancel)?;
            (overlay, stats, BackendKind::Cpu)
        }
    };

    let verdict = Verdict::from_stats(&stats, pair.full_overlap());
    let overlap = pair.overlap();
    let right_size = pair.right.dimensions();
    Ok(CompareDone {
        pair,
        output: ImageCompareOutput { overlay, stats, verdict, backend, overlap, right_size, options },
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use openbc_core::image_compare::DiffStats;

    struct Failing;
    impl GpuImageExecutor for Failing {
        fn compare(&self, _: &GpuImageJob<'_>) -> Result<GpuImageOutput, String> {
            Err("no adapter".into())
        }
    }

    fn solid(w: u32, h: u32, v: u8) -> Arc<RgbaImage> {
        Arc::new(RgbaImage::from_raw(w, h, vec![v; (w * h * 4) as usize]).unwrap())
    }

    #[test]
    fn gpu_failure_falls_back_to_cpu() {
        let policy = ImageComputePolicy { gpu_min_bytes: 0, gpu_memory_budget: u64::MAX };
        // 1024x300 RGBA = 1.2 MiB, above the hard floor, so the GPU is tried and fails.
        let (l, r) = (solid(1024, 300, 5), solid(1024, 300, 5));
        let done = compare_blocking(Some(Arc::new(Failing)), policy, l, r, Default::default(), &CancelToken::new()).unwrap();
        assert_eq!(done.output.backend, BackendKind::Cpu);
        assert_eq!(done.output.verdict, Verdict::PixelsSame);
        assert_eq!(done.output.stats, DiffStats { compared_pixels: 1024 * 300, identical_pixels: 1024 * 300, ..Default::default() });
    }

    #[test]
    fn auto_resize_scales_right_to_left() {
        let (l, r) = (solid(40, 20, 9), solid(20, 10, 9));
        let done = compare_blocking(None, Default::default(), l, r, Default::default(), &CancelToken::new()).unwrap();
        assert_eq!(done.pair.right.dimensions(), (40, 20));
        assert_eq!(done.output.verdict, Verdict::PixelsSame);
    }

    #[test]
    fn cancelled_before_alignment_reports_cancelled() {
        let c = CancelToken::new();
        c.cancel();
        let err = compare_blocking(None, Default::default(), solid(4, 4, 1), solid(2, 2, 1), Default::default(), &c)
            .err()
            .expect("cancelled");
        assert_eq!(err, ImageComputeError::Cancelled);
    }
}
