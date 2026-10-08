//! CPU/GPU execution policy for content hashing.

use std::sync::Arc;
use std::time::Instant;

use rayon::{ThreadPool, ThreadPoolBuilder};
use sha2::{Digest, Sha256};
use thiserror::Error;

use crate::gpu_policy;

/// Files larger than this are eligible for GPU dispatch.
pub const GPU_MIN_BYTES: usize = gpu_policy::GPU_DEFAULT_MIN_BYTES as usize;
/// Files below this size always remain on the CPU.
pub const GPU_HARD_FLOOR_BYTES: usize = gpu_policy::GPU_FLOOR_BYTES as usize;

/// Backend initialization and execution failures.
#[derive(Debug, Error)]
pub enum ComputeError {
    /// Rayon could not construct its dedicated worker pool.
    #[error("failed to build CPU compute pool: {0}")]
    CpuPool(#[from] rayon::ThreadPoolBuildError),
    /// GPU initialization failed but CPU fallback remains available.
    #[cfg(feature = "gpu")]
    #[error("GPU initialization failed: {0}")]
    Gpu(String),
}

/// Which execution backend handled an operation.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum BackendKind {
    /// Dedicated Rayon workers.
    Cpu,
    /// A compatible `wgpu` adapter.
    Gpu,
}

impl BackendKind {
    /// Short label used in observability records.
    pub const fn label(self) -> &'static str {
        match self {
            Self::Cpu => "CPU",
            Self::Gpu => "GPU",
        }
    }
}

/// Result of a content hashing operation.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct HashResult {
    /// SHA-256 digest bytes.
    pub digest: [u8; 32],
    /// Backend selected by the policy.
    pub backend: BackendKind,
}

/// Compute HAL with an always-available CPU path and optional GPU path.
pub struct ComputeDispatcher {
    cpu_pool: Arc<ThreadPool>,
    #[cfg(feature = "gpu")]
    gpu: Option<GpuBackend>,
}

impl ComputeDispatcher {
    /// Construct a dispatcher with `worker_count` Rayon workers.
    pub fn new(worker_count: usize) -> Result<Self, ComputeError> {
        let cpu_pool = ThreadPoolBuilder::new()
            .num_threads(worker_count.max(1))
            .build()?;
        Ok(Self {
            cpu_pool: Arc::new(cpu_pool),
            #[cfg(feature = "gpu")]
            gpu: None,
        })
    }

    /// Initialize the optional GPU backend; failure leaves CPU fallback intact.
    #[cfg(feature = "gpu")]
    pub async fn initialize_gpu(&mut self) {
        self.gpu = GpuBackend::initialize().await.ok();
    }

    /// Hash bytes using the CPU pool, or mark large eligible inputs for a GPU backend.
    ///
    /// Note: the digest is always computed on the CPU today; `HashResult::backend` reports
    /// the backend the *policy* selected so callers can already branch on it once a GPU
    /// hash kernel exists.
    pub fn hash_bytes(&self, bytes: &[u8]) -> HashResult {
        let started = Instant::now();
        let backend = self.select_backend(bytes.len());
        let digest = self.cpu_pool.install(|| {
            let mut hasher = Sha256::new();
            hasher.update(bytes);
            hasher.finalize().into()
        });
        openbc_observability::record(
            "openbc-compute",
            "hash_bytes",
            started.elapsed(),
            Some((bytes.len(), 0)),
            None,
            None,
            None,
            Some(backend.label().to_owned()),
        );
        HashResult { digest, backend }
    }

    fn select_backend(&self, len: usize) -> BackendKind {
        let eligible = gpu_policy::gpu_eligible(len as u64, gpu_policy::GPU_DEFAULT_MIN_BYTES);
        if eligible && self.gpu_available() {
            BackendKind::Gpu
        } else {
            BackendKind::Cpu
        }
    }

    #[cfg(feature = "gpu")]
    fn gpu_available(&self) -> bool {
        self.gpu.is_some()
    }

    #[cfg(not(feature = "gpu"))]
    fn gpu_available(&self) -> bool {
        false
    }
}

#[cfg(feature = "gpu")]
struct GpuBackend {
    _device: wgpu::Device,
    _queue: wgpu::Queue,
}

#[cfg(feature = "gpu")]
impl GpuBackend {
    async fn initialize() -> Result<Self, ComputeError> {
        let instance = wgpu::Instance::default();
        let adapter = instance
            .request_adapter(&wgpu::RequestAdapterOptions::default())
            .await
            .ok_or_else(|| ComputeError::Gpu("no compatible adapter found".to_owned()))?;
        let (device, queue) = adapter
            .request_device(&wgpu::DeviceDescriptor::default(), None)
            .await
            .map_err(|error| ComputeError::Gpu(error.to_string()))?;
        Ok(Self {
            _device: device,
            _queue: queue,
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sha256_of_abc_matches_the_reference_vector() {
        let d = ComputeDispatcher::new(2).unwrap();
        let r = d.hash_bytes(b"abc");
        assert_eq!(r.backend, BackendKind::Cpu);
        assert_eq!(
            r.digest[..4],
            [0xba, 0x78, 0x16, 0xbf],
            "SHA-256(\"abc\") starts with ba7816bf"
        );
    }

    #[test]
    fn zero_workers_is_clamped_to_one() {
        assert!(ComputeDispatcher::new(0).is_ok());
    }

    #[test]
    fn backend_labels() {
        assert_eq!(BackendKind::Cpu.label(), "CPU");
        assert_eq!(BackendKind::Gpu.label(), "GPU");
    }
}
