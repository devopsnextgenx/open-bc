# OpenBC Copilot Instructions


## Windows build environment (read before using any terminal)

On Windows, always use the **Git Bash** shell and set the MSYS2 toolchain PATH first, exactly as `win-build.sh` does. Without it `cargo` fails (e.g. `dlltool.exe: program not found`) and builds/tests are not valid.

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
export OPENSSL_DIR=/c/msys64/ucrt64
```

- Prefix every compile/test/run command with these exports (shell state does not persist between tool calls), e.g. `export PATH=... OPENSSL_DIR=... && cargo check --workspace`.
- Toolchain is `x86_64-pc-windows-gnu` (cargo/rustc/dlltool/perl/cmake/ninja come from `/c/msys64/ucrt64/bin`). Keep `win-build.sh` as the source of truth for these paths.

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
