use std::fmt;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ImageComputeError {
    /// Decoder rejected the bytes (unsupported format, truncated, over limits).
    Decode(String),
    /// Buffer size would overflow `usize` or exceed configured limits.
    TooLarge { width: u32, height: u32 },
    Cancelled,
    /// The worker pool dropped the job before producing a result.
    WorkerDropped,
}

impl fmt::Display for ImageComputeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Decode(m) => write!(f, "cannot decode image: {m}"),
            Self::TooLarge { width, height } => write!(f, "image too large: {width}x{height}"),
            Self::Cancelled => f.write_str("cancelled"),
            Self::WorkerDropped => f.write_str("compute worker dropped the job"),
        }
    }
}

impl std::error::Error for ImageComputeError {}
