# Design Principles

This page documents the foundational design decisions that shape OpenBC. Understanding these principles is essential before making architectural changes or adding new features.

---

## 1. Strict Layer Separation

OpenBC enforces hard boundaries between its concerns. Each crate has exactly one job and a clearly defined set of allowed dependencies:

```
openbc-core      ← no runtime deps (pure domain)
openbc-vfs       ← may use core only
openbc-compute   ← may use core only
openbc-engine    ← may use core + vfs + compute
openbc-ui-qt     ← may use engine (and transitively everything above)
```

**Why?** Each layer can be tested in isolation. A bug in a diff algorithm stays in `openbc-core` and can be reproduced with a unit test — no Qt, no filesystem, no async runtime needed. A bug in GPU scheduling stays in `openbc-compute`. This strict separation also means the Qt UI can be replaced with a different frontend without rewriting business logic.

---

## 2. Lazy, Level-by-Level Tree Loading

Directory traversal is performed one level at a time. When the engine opens a folder pair, it fetches only the immediate children. Expanding a subdirectory triggers a fetch of that subdirectory's immediate children — and so on recursively, under bounded concurrency.

**Why?** Repositories, backup drives, and network shares can contain hundreds of thousands of files. An upfront recursive scan would block the UI, consume unbounded memory, and make the tool unusable on large trees. Lazy loading keeps the UI responsive from the first result onward.

> **Rule:** Never add an upfront recursive scan. Expansion controls visibility, not whether descendants have been compared.

---

## 3. Metadata Before Content

For any pair of entries, OpenBC compares size and modification time before scheduling content work. If metadata matches, content comparison may be skipped entirely (configurable).

**Why?** Metadata reads are orders of magnitude cheaper than hashing file contents. On large folders with mostly-unchanged files, this optimization eliminates the majority of I/O.

---

## 4. Bounded Memory and Backpressure

All progress pipelines use bounded `tokio::sync::mpsc` channels. When a channel is full, the producer pauses — it does not allocate a larger buffer or drop events.

Specific bounds:
- Image compare event channel: ≤ 4 events (each may carry a full canvas overlay)
- Files above `ImageCompareConfig::max_file_bytes` are rejected per path without failing the other side
- GPU transfers are chunked; never map a file larger than the configured memory budget
- Memory mapping is used only for local regular files within the configured budget

**Why?** Comparison tools are commonly run on multi-gigabyte file trees. Any unbounded allocation is a denial-of-service risk. Backpressure makes the system self-regulating: throughput degrades gracefully rather than crashing.

---

## 5. CPU-First, GPU-Optional

Rayon is the primary compute backend. GPU acceleration through wgpu is an optional optimization, not a requirement for correctness.

GPU eligibility thresholds:
- Files **≥ 4 MiB** are eligible for GPU offload (after backend capability checks)
- Files **< 1 MiB** must always use CPU
- If no compatible GPU adapter is available, initialization falls back to CPU silently

All GPU backends must produce results that are bit-exact with the reference CPU implementation in `openbc-core`.

**Why?** GPU availability varies across machines. A tool that silently produces different results depending on whether a GPU is present is untrustworthy. CPU is always the source of truth; GPU is purely a performance path.

---

## 6. Qt Thread Isolation

The Qt event loop is single-threaded by design. OpenBC strictly enforces this:

- Qt model mutations happen **only** on the Qt GUI thread
- Rust worker-thread results are re-posted to the GUI thread via a C ABI callback
- No GPU object, Tokio task result, or Rayon output is allowed to directly mutate Qt state

**Why?** Violating Qt's threading model produces non-deterministic crashes. The re-post pattern is more verbose but safe and auditable.

---

## 7. Typed Errors in Libraries, `anyhow` at Boundaries

| Crate type | Error strategy |
|------------|----------------|
| Library crates (`openbc-core`, `openbc-vfs`, `openbc-compute`) | `thiserror` — typed, documented error variants |
| Execution drivers (`openbc-ui-qt`, `xtask`) | `anyhow` — rich context chains at application boundaries |

**Why?** Library callers deserve precise error types they can match on. Application boundaries care about human-readable messages with context chains. Mixing these strategies in the wrong crates leads to either opaque errors in libraries or over-engineered error handling in binaries.

---

## 8. Path-Scoped Failure Isolation

A single unreadable file must not terminate unrelated sibling work. VFS failures are attached to the affected path and emitted as `ScanEvent::Error` entries. The scan continues for all other paths.

The only exceptions are:
- Fatal configuration errors (e.g., invalid comparison policy)
- Channel closure (the UI has disconnected)

**Why?** In real-world usage, large folder trees frequently contain files the user lacks permission to read, files locked by other processes, or files on partially-disconnected network shares. Aborting the entire scan on the first error would make the tool unusable for exactly the scenarios where it is most needed.

---

## 9. Features for Optional Platform Dependencies

Any capability that requires a platform SDK, external service, or native library must be gated behind a Cargo feature flag. The crate must compile and pass tests without that feature enabled.

Examples:
- GPU backends: `wgpu` feature
- SSH/SFTP: `ssh` feature
- Windows registry compare: `windows-registry` feature

**Why?** Contributors on platforms that lack a given SDK should still be able to build and test the rest of the codebase. CI matrices can selectively enable features per platform.

---

## 10. Prefer Existing Traits Over New Parallel Abstractions

Before adding a new trait, adapter, or abstraction layer, check whether an existing one can be extended. OpenBC's internal APIs are designed to evolve at their boundaries, not to be forked.

**Why?** Parallel abstractions that diverge over time are worse than a single well-understood abstraction that accumulates some complexity. The cost of maintaining two separate code paths almost always exceeds the upfront simplicity of a new one.
