use crate::gpu_policy;

/// Buffers below this are never sent to a GPU (ARCHITECTURE.md performance policy).
pub const GPU_HARD_FLOOR_BYTES: u64 = gpu_policy::GPU_FLOOR_BYTES;

#[derive(Clone, Copy, Debug)]
pub struct ImageComputePolicy {
    /// Canvas size (bytes) from which the GPU is eligible. Clamped up to the hard floor.
    pub gpu_min_bytes: u64,
    /// Upper bound on bytes resident on the GPU (left + right + overlay).
    pub gpu_memory_budget: u64,
}

impl Default for ImageComputePolicy {
    fn default() -> Self {
        Self { gpu_min_bytes: gpu_policy::GPU_DEFAULT_MIN_BYTES, gpu_memory_budget: 256 << 20 }
    }
}

impl ImageComputePolicy {
    pub fn gpu_eligible(&self, canvas_bytes: u64, right_bytes: u64) -> bool {
        gpu_policy::gpu_eligible(canvas_bytes, self.gpu_min_bytes)
            && canvas_bytes.saturating_mul(2).saturating_add(right_bytes) <= self.gpu_memory_budget
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn gpu_policy_floor_and_budget() {
        let p = ImageComputePolicy { gpu_min_bytes: 1024, gpu_memory_budget: 64 << 20 };
        assert!(!p.gpu_eligible(512 << 10, 0)); // below 1 MiB hard floor
        assert!(p.gpu_eligible(4 << 20, 4 << 20));
        assert!(!p.gpu_eligible(40 << 20, 0)); // 2x canvas exceeds budget
    }

    #[test]
    fn default_policy_uses_the_shared_threshold() {
        let p = ImageComputePolicy::default();
        assert_eq!(p.gpu_min_bytes, gpu_policy::GPU_DEFAULT_MIN_BYTES);
        assert!(!p.gpu_eligible(gpu_policy::GPU_DEFAULT_MIN_BYTES - 1, 0));
        assert!(p.gpu_eligible(gpu_policy::GPU_DEFAULT_MIN_BYTES, 0));
    }
}
