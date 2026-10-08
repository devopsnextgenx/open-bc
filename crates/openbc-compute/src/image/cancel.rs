use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use super::error::ImageComputeError;

/// Cooperative cancellation shared between the engine and Rayon jobs. Kernels
/// poll it once per row, so a superseded slider position stops within a row.
#[derive(Clone, Debug, Default)]
pub struct CancelToken(Arc<AtomicBool>);

impl CancelToken {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn cancel(&self) {
        self.0.store(true, Ordering::Relaxed);
    }

    #[inline]
    pub fn is_cancelled(&self) -> bool {
        self.0.load(Ordering::Relaxed)
    }

    /// `Err(Cancelled)` once cancelled; lets kernels write `cancel.check()?`.
    #[inline]
    pub fn check(&self) -> Result<(), ImageComputeError> {
        if self.is_cancelled() {
            Err(ImageComputeError::Cancelled)
        } else {
            Ok(())
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn clones_share_the_flag() {
        let a = CancelToken::new();
        let b = a.clone();
        assert!(a.check().is_ok());
        b.cancel();
        assert_eq!(a.check(), Err(ImageComputeError::Cancelled));
    }
}
