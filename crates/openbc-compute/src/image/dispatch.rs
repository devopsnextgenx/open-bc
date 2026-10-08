use std::sync::Arc;

use openbc_core::image_compare::{AlignedPair, ImageCompareOptions, ImageCompareOutput, RgbaImage};
use tokio::sync::oneshot;

use super::cancel::CancelToken;
use super::decode::{decode_image, DecodedImage};
use super::error::ImageComputeError;
use super::gpu::GpuImageExecutor;
use super::pipeline::compare_blocking;
use super::policy::ImageComputePolicy;

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
