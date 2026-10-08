use openbc_core::model3d::CancelToken;

/// Failure of a model3d compute pass.
#[derive(Debug, Clone, thiserror::Error)]
pub enum ComputeError {
    #[error("cancelled")]
    Cancelled,
    #[error("both models must be loaded")]
    NotLoaded,
    #[error("{0}")]
    Message(String),
}

/// `Err(Cancelled)` once the token fires; the one cancellation check used by every stage.
#[inline]
pub fn check(cancel: &CancelToken) -> Result<(), ComputeError> {
    if cancel.is_cancelled() {
        Err(ComputeError::Cancelled)
    } else {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn display_strings_are_stable() {
        assert_eq!(ComputeError::Cancelled.to_string(), "cancelled");
        assert_eq!(ComputeError::NotLoaded.to_string(), "both models must be loaded");
        assert_eq!(ComputeError::Message("boom".into()).to_string(), "boom");
    }
}
