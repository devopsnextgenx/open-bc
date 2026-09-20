# OpenBC Coding Guidelines

## Language and toolchain

- Rust Edition 2021 for all Rust crates.
- C++17 only where CXX-Qt generated or handwritten bridge code requires it.
- WGSL for `wgpu` compute shaders.

## Concurrency

- Tokio handles asynchronous I/O and orchestration.
- Rayon handles CPU-bound work; use dedicated pools or `spawn_blocking` boundaries when required by the embedding application.
- GPU dispatches run through `openbc-compute` and never block the Qt event loop.
- Do not perform blocking operations on the Qt main thread or Tokio worker threads.
- Use bounded `tokio::sync::mpsc` channels for scan progress and cancellation.

## Thresholds and I/O

- GPU offload is eligible only above 4 MiB, after backend capability checks. Files below 1 MiB always use CPU execution.
- Prefer streaming and bounded buffers. Memory-map only local, regular files when the mapping is smaller than the configured memory budget.

## Errors

- Library crates (`openbc-core`, `openbc-vfs`, `openbc-compute`) use `thiserror` for typed errors.
- Execution drivers (`openbc-ui-qt`, `xtask`) use `anyhow` at their binary/application boundaries.
- Preserve path and operation context in propagated errors.

## Naming and documentation

- `snake_case` for modules, functions, and variables.
- `PascalCase` for structs, traits, and enums.
- `SCREAMING_SNAKE_CASE` for constants.
- Public structs, traits, and functions require `///` documentation with a short usage example where practical.
- Keep comments focused on invariants and non-obvious performance decisions.
