//! Backend selection for the deviation query (the dominant cost): GPU when eligible and
//! available, otherwise Rayon. No wgpu type leaks out of this module.
use super::bvh::Bvh;
use super::ComputeError;
use openbc_core::model3d::{Backend, CancelToken, V3};
use rayon::prelude::*;
use std::sync::Arc;

/// Architecture rule: files below 1 MiB are never sent to a GPU, default threshold 4 MiB.
pub const GPU_FLOOR_BYTES: u64 = 1 << 20;
pub const GPU_DEFAULT_BYTES: u64 = 4 << 20;

#[derive(Debug)]
pub enum GpuError {
    Unavailable(String),
    TooLarge(String),
    Device(String),
    Cancelled,
}

/// Executes signed-distance queries against a BVH. Implemented by the wgpu backend
/// (`gpu.rs`); tests and embedders can plug their own.
pub trait DeviationExecutor: Send + Sync {
    fn name(&self) -> String;
    fn signed_distances(&self, bvh: &Bvh, queries: &[V3], cancel: &CancelToken) -> Result<Vec<f32>, GpuError>;
}

#[derive(Clone, Copy, Debug)]
pub struct GpuPolicy { pub min_file_bytes: u64 }
impl Default for GpuPolicy { fn default() -> Self { Self { min_file_bytes: GPU_DEFAULT_BYTES } } }
impl GpuPolicy {
    pub fn eligible(&self, largest_file_bytes: u64) -> bool { largest_file_bytes >= self.min_file_bytes.max(GPU_FLOOR_BYTES) }
}

#[derive(Clone)]
pub struct Backends { gpu: Option<Arc<dyn DeviationExecutor>>, policy: GpuPolicy }

impl Backends {
    pub fn cpu_only() -> Self { Self { gpu: None, policy: GpuPolicy::default() } }
    pub fn with_gpu(gpu: Arc<dyn DeviationExecutor>, policy: GpuPolicy) -> Self { Self { gpu: Some(gpu), policy } }

    /// Tries to bring up the wgpu backend; any failure falls back to CPU-only.
    pub fn detect() -> Self {
        #[cfg(feature = "gpu")]
        if let Ok(g) = super::gpu::WgpuDeviation::try_new() { return Self::with_gpu(Arc::new(g), GpuPolicy::default()); }
        Self::cpu_only()
    }

    pub fn gpu_name(&self) -> Option<String> { self.gpu.as_ref().map(|g| g.name()) }

    /// GPU first (if preferred, present and eligible), CPU otherwise. A GPU failure is never
    /// fatal: the pass silently continues on the CPU.
    pub fn signed_distances(&self, bvh: &Bvh, queries: &[V3], largest_file_bytes: u64, prefer_gpu: bool, cancel: &CancelToken)
        -> Result<(Vec<f32>, Backend), ComputeError>
    {
        if let (true, Some(g), true) = (prefer_gpu, &self.gpu, self.policy.eligible(largest_file_bytes)) {
            match g.signed_distances(bvh, queries, cancel) {
                Ok(v) => return Ok((v, Backend::Gpu)),
                Err(GpuError::Cancelled) => return Err(ComputeError::Cancelled),
                Err(_) => {}
            }
        }
        Ok((cpu_signed_distances(bvh, queries, cancel)?, Backend::Cpu))
    }
}

pub fn cpu_signed_distances(bvh: &Bvh, queries: &[V3], cancel: &CancelToken) -> Result<Vec<f32>, ComputeError> {
    let parts: Vec<Vec<f32>> = queries.par_chunks(2048).map(|c| {
        if cancel.is_cancelled() { return Err(ComputeError::Cancelled); }
        Ok(c.iter().map(|&p| bvh.closest(p).map(|h| h.signed).unwrap_or(f32::INFINITY)).collect::<Vec<f32>>())
    }).collect::<Result<_, _>>()?;
    Ok(parts.into_iter().flatten().collect())
}
