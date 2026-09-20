# OpenBC Copilot Instructions

Read `ARCHITECTURE.md` and `docs/CODING_GUIDELINES.md` before adding modules, traits, dependencies, or cross-crate APIs.

## Boundary rules

- `openbc-core` contains domain types and algorithms only. It must never depend on Qt, `openbc-ui-qt`, Tokio, Rayon, `wgpu`, or a concrete VFS.
- `openbc-vfs` owns filesystem and remote protocol adapters. It may depend on `openbc-core`, but never on UI crates.
- `openbc-compute` owns CPU/GPU execution. It must never import Qt C++ bindings or `openbc-ui-qt`.
- `openbc-engine` owns asynchronous orchestration and may depend on core, VFS, and compute.
- `openbc-ui-qt` is the only crate allowed to contain Qt or CXX-Qt bindings.
- `xtask` is a build driver, not a runtime dependency of library crates.

## Concurrency rules

- Use Tokio for I/O-bound orchestration and channel coordination.
- Use Rayon for CPU-bound hashing, diffing, and parallel traversal work.
- Use `openbc-compute` GPU backends for large-array processing only when the threshold policy permits it.
- Async work must communicate through `tokio::sync::mpsc`; never mutate Qt model state from a Tokio task.
- Keep blocking filesystem, SSH, SMB, and GPU mapping operations off Tokio worker threads.

## Change discipline

- Preserve lazy, level-by-level tree loading. Never add an upfront recursive scan.
- Prefer existing traits and types over new parallel abstractions.
- Add tests for boundary behavior and run `cargo check --workspace` plus focused tests.
- Keep optional integrations behind features when they require platform SDKs or external services.
