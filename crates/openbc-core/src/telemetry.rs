//! Thin wrapper over `openbc-observability` so call sites stay one-liners and the
//! component name lives in exactly one place.

use std::time::Instant;

/// Component name reported to the observability sink.
const COMPONENT: &str = "openbc-core";

/// Optional measurements attached to a timing sample.
///
/// Field order mirrors the positional parameters of `openbc_observability::record`.
#[derive(Default)]
pub(crate) struct Sample {
    /// Input sizes in bytes (left, right).
    pub sizes: Option<(usize, usize)>,
    /// Input sizes in lines (left, right).
    pub lines: Option<(usize, usize)>,
    /// Sum of entry sizes processed.
    pub total_size: Option<u64>,
    /// Number of items of interest processed.
    pub total_count: Option<usize>,
    /// Free-form description of the backend or input kind.
    pub detail: Option<String>,
}

/// Emits an INFO log line for `operation`.
pub(crate) fn info(operation: &'static str, message: &'static str) {
    openbc_observability::log(COMPONENT, operation, "INFO", message);
}

/// Records how long `operation` took since `started`, with optional measurements.
pub(crate) fn record(operation: &'static str, started: Instant, sample: Sample) {
    openbc_observability::record(
        COMPONENT,
        operation,
        started.elapsed(),
        sample.sizes,
        sample.lines,
        sample.total_size,
        sample.total_count,
        sample.detail,
    );
}
