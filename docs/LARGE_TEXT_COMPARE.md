# Large text compare architecture

The large-file text compare path now starts in `openbc-core` with a bounded-memory read layer instead of fully materializing the document before diffing.

## Components

- `LargeTextSource`: chunked file reads with a bounded cache and metadata change detection.
- `LineIndex`: newline boundary lookup for large byte streams.
- `hash_line` / `normalize_line_bytes`: stable per-line hashing and normalization helpers used by the diff pipeline.

## Design goals

- Keep memory proportional to the visible window and bounded cache instead of the whole file.
- Preserve a small, deterministic amount of state while the file is open.
- Detect file truncation / replacement through metadata verification and present a `SourceChanged` signal.

## Validation

Run the core tests with:

```bash
cargo test -p openbc-core --lib
```
