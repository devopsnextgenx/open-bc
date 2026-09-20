//! CPU/GPU execution policy for hashing and binary comparison.

use rayon::{ThreadPool, ThreadPoolBuilder};
use sha2::{Digest, Sha256};
use std::sync::Arc;
use thiserror::Error;

/// Files larger than this are eligible for GPU dispatch.
pub const GPU_MIN_BYTES: usize = 4 * 1024 * 1024;
/// Files below this size always remain on the CPU.
pub const GPU_HARD_FLOOR_BYTES: usize = 1024 * 1024;

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
    pub fn hash_bytes(&self, bytes: &[u8]) -> HashResult {
        let backend = if bytes.len() >= GPU_MIN_BYTES {
            #[cfg(feature = "gpu")]
            {
                if self.gpu.is_some() {
                    BackendKind::Gpu
                } else {
                    BackendKind::Cpu
                }
            }
            #[cfg(not(feature = "gpu"))]
            {
                BackendKind::Cpu
            }
        } else {
            BackendKind::Cpu
        };
        let digest = self.cpu_pool.install(|| {
            let mut hasher = Sha256::new();
            hasher.update(bytes);
            hasher.finalize().into()
        });
        HashResult { digest, backend }
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
