//! Errors, cancellation and progress reporting shared by the text engine.

use std::io;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use thiserror::Error;

/// Failures produced by the text comparison engine.
#[derive(Debug, Error)]
pub enum EngineError {
    /// The underlying file could not be opened, mapped or read.
    #[error("I/O error: {0}")]
    Io(#[from] io::Error),
    /// The operation was cancelled through its [`CancelToken`]. No partial state is published.
    #[error("operation cancelled")]
    Cancelled,
    /// One side has more lines than the engine can address (see `MAX_LINES`).
    #[error("too many lines ({0}); the limit is {1}")]
    TooManyLines(u64, u64),
    /// A single UTF-16 line is longer than a record can describe.
    #[error("a line is longer than {0} bytes")]
    LineTooLong(u64),
    /// The two line indexes were built with different normalisation or hashing settings.
    #[error("the two sides were indexed with different normalisation options")]
    OptionsMismatch,
    /// The file was modified (or truncated) after it was opened.
    #[error("source changed while it was open")]
    SourceChanged,
}

/// Cheap, cloneable cancellation flag. Cancelling never leaves shared state half-written because
/// every long-running operation builds its result privately and returns it only on success.
#[derive(Clone, Debug, Default)]
pub struct CancelToken(Arc<AtomicBool>);

impl CancelToken {
    /// A fresh, un-cancelled token.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Request cancellation; visible to every clone.
    pub fn cancel(&self) {
        self.0.store(true, Ordering::Relaxed);
    }

    /// Whether cancellation was requested.
    #[must_use]
    pub fn is_cancelled(&self) -> bool {
        self.0.load(Ordering::Relaxed)
    }

    /// `Err(Cancelled)` when cancellation was requested.
    pub fn check(&self) -> Result<(), EngineError> {
        if self.is_cancelled() {
            Err(EngineError::Cancelled)
        } else {
            Ok(())
        }
    }
}

/// Phase of a comparison, reported through [`Progress`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Phase {
    /// Scanning a file and building its line index (units: bytes).
    Indexing,
    /// Matching lines of both sides to shared classes (units: lines).
    Matching,
    /// Aligning the two sides (units: lines resolved).
    Aligning,
}

/// One progress notification. `done <= total`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Progress {
    /// Current phase.
    pub phase: Phase,
    /// Units completed.
    pub done: u64,
    /// Total units (0 when unknown).
    pub total: u64,
}

/// Receives progress notifications. Called on the computing thread; keep it cheap.
pub type ProgressSink<'a> = &'a mut dyn FnMut(Progress);

/// A no-op progress sink for callers that do not care.
pub fn no_progress(_: Progress) {}
