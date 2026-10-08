//! Single source of truth for the "may this input go to the GPU?" size policy.
//!
//! ARCHITECTURE.md performance policy: inputs below 1 MiB never use a GPU, whatever the
//! caller configures; the default eligibility threshold is 4 MiB. Every domain (hashing,
//! hex, image, model3d) derives its own constants and checks from here so the numbers can
//! only change in one place.

/// Inputs smaller than this never use the GPU, regardless of the configured threshold.
pub const GPU_FLOOR_BYTES: u64 = 1 << 20;

/// Default input size from which the GPU becomes eligible.
pub const GPU_DEFAULT_MIN_BYTES: u64 = 4 << 20;

/// The effective threshold for a caller-configured minimum (clamped up to the floor).
pub const fn gpu_threshold(configured_min: u64) -> u64 {
    if configured_min > GPU_FLOOR_BYTES {
        configured_min
    } else {
        GPU_FLOOR_BYTES
    }
}

/// Whether an input of `bytes` is large enough for the GPU under `configured_min`.
pub const fn gpu_eligible(bytes: u64, configured_min: u64) -> bool {
    bytes >= gpu_threshold(configured_min)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn floor_always_wins_over_a_lower_configured_minimum() {
        assert_eq!(gpu_threshold(0), GPU_FLOOR_BYTES);
        assert_eq!(gpu_threshold(GPU_FLOOR_BYTES - 1), GPU_FLOOR_BYTES);
        assert_eq!(gpu_threshold(GPU_DEFAULT_MIN_BYTES), GPU_DEFAULT_MIN_BYTES);
    }

    #[test]
    fn eligibility_is_inclusive_at_the_threshold() {
        assert!(!gpu_eligible(GPU_FLOOR_BYTES - 1, 0));
        assert!(gpu_eligible(GPU_FLOOR_BYTES, 0));
        assert!(!gpu_eligible(GPU_DEFAULT_MIN_BYTES - 1, GPU_DEFAULT_MIN_BYTES));
        assert!(gpu_eligible(GPU_DEFAULT_MIN_BYTES, GPU_DEFAULT_MIN_BYTES));
    }
}
