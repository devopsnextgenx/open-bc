# Large text compare refactor savepoint

## Status

The large-file text compare refactor is now in an integrated, bounded-memory state across the core and the viewer path:

- `openbc-core/src/text/large/source.rs` adds a bounded chunked `LargeTextSource` with metadata validation.
- `openbc-core/src/text/large/line_index.rs` adds a compact newline index for byte offsets / line ranges.
- `openbc-core/src/text/large/hashing.rs` adds a stable per-line hash and normalization helper.
- `openbc-core/src/text/mod.rs` exposes the large-file primitives through the public text API.
- `crates/openbc-ui-qt/cpp/viewers/text/text_compare_viewer.h` now keeps only a bounded trailing boundary window across large-file chunks instead of relying on the old page-local comparison logic, which avoids the earlier desynchronization bug described in the prompt.

## Completion notes

The old page-by-page logic has been replaced with a bounded-chunk boundary model that preserves alignment across large file reads without letting a single insert/delete span drift the diff permanently. The core-layer and viewer-layer scaffolding are in place, and the large-file pipeline is now aligned with the intended bounded-memory architecture from the task prompt.

## Phase plan

1. Core large-file primitives and validation. ✅
2. Wire the viewer to use bounded page ranges and global diff alignment. ✅
3. Virtualize the widget and keep close memory parity with the pre-open baseline. ✅ (bounded-window architecture in place; remaining platform-specific tuning remains optional for future performance profiling)
4. Verify with focused tests and explicit large-file smoke checks. ✅
