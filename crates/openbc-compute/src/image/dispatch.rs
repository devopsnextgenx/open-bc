use std::sync::Arc;

use openbc_core::image_compare::{
    plan_resize, AlignedPair, BackendKind, ImageCompareOptions, ImageCompareOutput, RgbaImage,
    Verdict,
};
use tokio::sync::oneshot;

use super::cancel::CancelToken;
use super::decode::{decode_image, DecodedImage};
use super::error::ImageComputeError;
use super::gpu::{GpuCompareParams, GpuImageExecutor, GpuImageJob};
use super::kernel::compare_cpu;
use super::resize::resize_bilinear;

/// Buffers below this are never sent to a GPU (ARCHITECTURE.md performance policy).
pub const GPU_HARD_FLOOR_BYTES: u64 = 1 << 20;

#[derive(Clone, Copy, Debug)]
pub struct ImageComputePolicy {
    /// Canvas size (bytes) from which the GPU is eligible. Clamped up to the hard floor.
    pub gpu_min_bytes: u64,
    /// Upper bound on bytes resident on the GPU (left + right + overlay).
    pub gpu_memory_budget: u64,
}

impl Default for ImageComputePolicy {
    fn default() -> Self {
        Self { gpu_min_bytes: 4 << 20, gpu_memory_budget: 256 << 20 }
    }
}

impl ImageComputePolicy {
    pub fn gpu_eligible(&self, canvas_bytes: u64, right_bytes: u64) -> bool {
        canvas_bytes >= self.gpu_min_bytes.max(GPU_HARD_FLOOR_BYTES)
            && canvas_bytes.saturating_mul(2).saturating_add(right_bytes) <= self.gpu_memory_budget
    }
}

/// Both sides decoded independently so one bad file does not hide the other.
pub struct PairDecode {
    pub left: Result<DecodedImage, ImageComputeError>,
    pub right: Result<DecodedImage, ImageComputeError>,
    /// Encoded bytes were identical (checked before decoding).
    pub bytes_identical: bool,
}

pub struct CompareDone {
    pub pair: Arc<AlignedPair>,
    pub output: ImageCompareOutput,
}

/// Single entry point for image compute. Cheap to share behind an `Arc`.
#[derive(Default)]
pub struct ImageCompute {
    pool: Option<Arc<rayon::ThreadPool>>,
    gpu: Option<Arc<dyn GpuImageExecutor>>,
    policy: ImageComputePolicy,
}

impl ImageCompute {
    pub fn new() -> Self {
        Self::default()
    }
    /// Run all work on an explicitly configured pool instead of Rayon's global one.
    pub fn with_pool(mut self, pool: Arc<rayon::ThreadPool>) -> Self {
        self.pool = Some(pool);
        self
    }
    pub fn with_gpu(mut self, gpu: Arc<dyn GpuImageExecutor>) -> Self {
        self.gpu = Some(gpu);
        self
    }
    pub fn with_policy(mut self, policy: ImageComputePolicy) -> Self {
        self.policy = policy;
        self
    }

    fn spawn<T: Send + 'static>(&self, f: impl FnOnce() -> T + Send + 'static) -> oneshot::Receiver<T> {
        let (tx, rx) = oneshot::channel();
        let job = move || {
            let _ = tx.send(f()); // receiver gone = superseded request
        };
        match &self.pool {
            Some(pool) => pool.spawn(job),
            None => rayon::spawn(job),
        }
        rx
    }

    /// Compare encoded bytes, then decode both sides in parallel.
    pub fn decode_pair_async(&self, left: Vec<u8>, right: Vec<u8>) -> oneshot::Receiver<PairDecode> {
        self.spawn(move || {
            let bytes_identical = left == right;
            let (l, r) = rayon::join(|| decode_image(&left), || decode_image(&right));
            PairDecode { left: l, right: r, bytes_identical }
        })
    }

    /// Align (resize + offset) and compare. Cancellation is cooperative.
    pub fn compare_async(
        &self,
        left: Arc<RgbaImage>,
        right: Arc<RgbaImage>,
        options: ImageCompareOptions,
        cancel: CancelToken,
    ) -> oneshot::Receiver<Result<CompareDone, ImageComputeError>> {
        let gpu = self.gpu.clone();
        let policy = self.policy;
        self.spawn(move || compare_blocking(gpu, policy, left, right, options, &cancel))
    }
}

fn fit(img: &Arc<RgbaImage>, target: (u32, u32), cancel: &CancelToken) -> Result<Arc<RgbaImage>, ImageComputeError> {
    if img.dimensions() == target {
        Ok(img.clone())
    } else {
        resize_bilinear(img, target.0, target.1, cancel).map(Arc::new)
    }
}

fn compare_blocking(
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
    if cancel.is_cancelled() {
        return Err(ImageComputeError::Cancelled);
    }

    let canvas = pair.canvas();
    let canvas_bytes = RgbaImage::byte_len(canvas.0, canvas.1).unwrap_or(usize::MAX) as u64;
    let right_bytes = pair.right.as_bytes().len() as u64;

    let mut gpu_result = None;
    if let Some(gpu) = gpu.filter(|_| policy.gpu_eligible(canvas_bytes, right_bytes)) {
        let job = GpuImageJob {
            left: pair.left.as_bytes(),
            right: pair.right.as_bytes(),
            params: GpuCompareParams::new(canvas, pair.right.dimensions(), &options),
        };
        gpu_result = gpu
            .compare(&job)
            .ok()
            .and_then(|o| (o.overlay.len() as u64 == canvas_bytes).then_some(o));
    }

    let (overlay, stats, backend) = match gpu_result {
        Some(o) => (
            RgbaImage::from_raw(canvas.0, canvas.1, o.overlay)
                .ok_or(ImageComputeError::TooLarge { width: canvas.0, height: canvas.1 })?,
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
    use super::super::gpu::GpuImageOutput;
    use openbc_core::image_compare::DiffStats;

    #[test]
    fn gpu_policy_floor_and_budget() {
        let p = ImageComputePolicy { gpu_min_bytes: 1024, gpu_memory_budget: 64 << 20 };
        assert!(!p.gpu_eligible(512 << 10, 0)); // below 1 MiB hard floor
        assert!(p.gpu_eligible(4 << 20, 4 << 20));
        assert!(!p.gpu_eligible(40 << 20, 0)); // 2x canvas exceeds budget
    }

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
}
