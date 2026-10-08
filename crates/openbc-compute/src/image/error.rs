use std::fmt::Display;

#[derive(Clone, Debug, PartialEq, Eq, thiserror::Error)]
pub enum ImageComputeError {
    /// Decoder rejected the bytes (unsupported format, truncated, over limits).
    #[error("cannot decode image: {0}")]
    Decode(String),
    /// Buffer size would overflow `usize` or exceed configured limits.
    #[error("image too large: {width}x{height}")]
    TooLarge { width: u32, height: u32 },
    #[error("cancelled")]
    Cancelled,
    /// The worker pool dropped the job before producing a result.
    #[error("compute worker dropped the job")]
    WorkerDropped,
}

impl ImageComputeError {
    pub(crate) fn decode(cause: impl Display) -> Self {
        Self::Decode(cause.to_string())
    }

    pub(crate) fn too_large(width: u32, height: u32) -> Self {
        Self::TooLarge { width, height }
    }
}
