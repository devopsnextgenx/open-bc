//! Instrumentation for the text engine: which UI tab an operation belongs to, and one record per
//! long operation (open, index, diff, rules, re-diff, save) in that tab's instrumentation report.
//!
//! The engine itself knows nothing about tabs. A caller that does (the Qt FFI) wraps the work of a
//! worker thread in [`enter_session`]; every record made on that thread then lands in that tab's
//! report instead of the shared "application" one. Records are per operation, never per painted
//! line, so a large file adds a handful of events, not thousands.

use std::time::Instant;

use super::model::{SideId, TextCompareModel};
use crate::telemetry::{self, Sample};

/// Keeps the calling thread attributed to a UI tab until dropped.
pub struct SessionGuard {
    _scope: Option<openbc_observability::SessionScope>,
}

/// Attribute instrumentation recorded on this thread to the tab `session_id`. An empty id leaves the
/// current attribution unchanged.
#[must_use]
pub fn enter_session(session_id: &str) -> SessionGuard {
    SessionGuard {
        _scope: (!session_id.is_empty()).then(|| openbc_observability::session_scope(session_id)),
    }
}

/// Records how long `operation` took on `model`: input sizes and line counts, and a one-line result
/// summary (rows, change blocks, whether the alignment degraded, index memory).
pub fn record_model_operation(
    operation: &'static str,
    started: Instant,
    model: &TextCompareModel,
    detail: &str,
) {
    let (left_lines, right_lines) = model.line_counts();
    let stats = model.stats();
    let memory = model.memory_report();
    let mut summary = String::new();
    if !detail.is_empty() {
        summary.push_str(detail);
        summary.push(' ');
    }
    summary.push_str(&format!(
        "rows={} blocks={} equal_rows={} inserted_rows={} deleted_rows={} replaced_rows={} degraded={} index_bytes={} index_resident_bytes={} diff_bytes={}",
        model.row_count(),
        stats.blocks,
        stats.equal_rows,
        stats.inserted_rows,
        stats.deleted_rows,
        stats.replaced_rows,
        model.is_degraded(),
        memory.index_bytes,
        memory.index_resident_bytes,
        memory.diff_bytes,
    ));
    telemetry::record(
        operation,
        started,
        Sample {
            sizes: Some((
                model.source_len(SideId::Left) as usize,
                model.source_len(SideId::Right) as usize,
            )),
            lines: Some((left_lines as usize, right_lines as usize)),
            detail: Some(summary),
            ..Sample::default()
        },
    );
}

/// Records an operation that produced no model: it failed or was cancelled. `outcome` is the reason.
pub fn record_failed_operation(operation: &'static str, started: Instant, outcome: &str) {
    telemetry::record(
        operation,
        started,
        Sample {
            detail: Some(format!("not completed: {outcome}")),
            ..Sample::default()
        },
    );
}
