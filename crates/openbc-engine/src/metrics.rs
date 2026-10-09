//! Observability helpers shared by the folder-level modules.

use openbc_core::EntryPath;

/// Records one timed engine operation under the `openbc-engine` component.
///
/// A macro (not a function) so each call site keeps inferring the exact argument types that
/// `openbc_observability::record` expects.
macro_rules! record_op {
    ($op:expr, $started:expr, io: $io:expr, bytes: $bytes:expr, depth: $depth:expr, note: $note:expr $(,)?) => {
        openbc_observability::record(
            "openbc-engine",
            $op,
            $started.elapsed(),
            $io,
            None,
            $bytes,
            $depth,
            Some(($note).to_owned()),
        )
    };
}

/// Number of path components, used as the "depth" metric.
pub(crate) fn depth_of(path: &EntryPath) -> usize {
    path.0.components().count()
}
