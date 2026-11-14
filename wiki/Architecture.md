# Architecture Overview

OpenBC is organized as a **dependency-directed Cargo workspace**. UI concerns live at the edge; domain logic, filesystem access, compute, and orchestration layers remain independently testable and have strictly enforced dependency boundaries.

---

## Crate Dependency Graph

```
openbc-core   (no upstream deps)
      │
      ├──► openbc-vfs       (filesystem & remote adapters)
      │
      ├──► openbc-compute   (CPU / GPU execution)
      │
      ├──► openbc-engine    (async orchestration)  ◄── openbc-vfs, openbc-compute
      │
      └──► openbc-ui-qt     (Qt bridge & models)   ◄── openbc-engine
```

> **Rule**: No crate may depend on a crate "above" it in this hierarchy. `openbc-core` has zero runtime dependencies on Qt, Tokio, Rayon, or wgpu.

---

## Data Flow

```mermaid
flowchart LR
    VFS[openbc-vfs\nAsyncVfs adapters] -->|directory entries and file bytes| E[openbc-engine\nTokio orchestration]
    E --> C[openbc-compute\nHAL]
    C --> CPU[Rayon CPU backend]
    C --> GPU[wgpu GPU backend]
    C -->|comparison results| E
    E -->|tokio::sync::mpsc progress| Q[openbc-ui-qt\nQt bridge]
    Q --> M[QAbstractItemModel]
    M --> T[QTreeView]
    D[openbc-core\ndomain and diff types] -. shared contracts .-> VFS
    D -. shared contracts .-> C
    D -. shared contracts .-> E
```

---

## Crate Responsibilities

### `openbc-core`
**Domain types, comparison policy, and pure diff algorithms.**

- No I/O, no threads, no async, no external runtime dependencies
- Contains: folder entry types, **file-matching policy**, diff algorithms, image compare options & verdicts, text diff, hex diff, table diff, 3D model diff, registry compare, metadata comparison, **binary compare model, diff and result tree**
- All backends must produce results bit-exact with the reference implementations here

Key modules:

| Module | Contents |
|--------|----------|
| `folder` | `DirectoryEntry`, `EntryMetadata`, `FolderComparison`, one-level name alignment |
| `text` | Line diff, log normalization, inline diff, syntax highlighting |
| `image_compare` | `RgbaImage`, `ImageCompareOptions`, `ImageCompareOutput`, `PixelProbe` |
| `hex_compare` | Byte-level difference runs |
| `hash_policy` | `HashAlgorithm` (MD5 default, SHA-1, SHA-256, SHA-512), `MatchMethod` (size/timestamp or hash), `MatchPolicy`: per-file-type rules and their one-line serialized form. Classifies only; never hashes |
| `binary_compare` | Format-neutral `BinaryModel` (metadata, sections/segments, symbols, functions with SHA-256 + SimHash, strings), `diff_binaries` (Unchanged / Modified / Removed / Added, matched by identity not address), `build_tree` / `flatten` for the collapsible result tree. Contains **no parser** |
| `meta_compare` | Generic property-table comparison (media metadata) |
| `table_compare` | CSV / spreadsheet row alignment, cell diff, copy edits |
| `registry_compare` | Windows `.reg` parsing, diff, copy edits, and writing |
| `model3d` | 3D mesh options, results, loader extension point |

---

### `openbc-vfs`
**Filesystem and remote protocol adapters.**

- Exposes the `AsyncVfs` trait: metadata reads, directory streams, file byte streams
- Adapters: local filesystem, FTP (`ftp.rs`), SFTP (`sftp.rs`), network utilities (`network.rs`)
- Lazy directory levels: each adapter must expose one level at a time, never recurse eagerly
- May depend on `openbc-core`; must never depend on UI crates

---

### `openbc-compute`
**CPU and GPU execution backends — the Hardware Abstraction Layer.**

- Selects CPU or GPU without leaking backend types to callers
- **CPU path**: Rayon-parallelized hashing, diffing, image decoding, audio decoding, spectral FFT
- **GPU path**: wgpu compute shaders (WGSL) for eligible payloads
- GPU eligibility policy: ≥ 4 MiB; files < 1 MiB always use CPU
- Automatic CPU fallback when no compatible GPU adapter is available
- Must never import Qt bindings or `openbc-ui-qt`

Key modules:

| Module | Contents |
|--------|----------|
| `dispatcher` | Backend capability checks and routing |
| `gpu_policy` | Threshold policy for GPU offload eligibility |
| `image/` | Bilinear resize, per-pixel compare, WGSL kernel |
| `audio_compare` | Decode to normalized PCM, channel power averaging |
| `spectral_diff` | Hann-windowed radix-2 STFT → RGB888 spectrogram |
| `media/` | Multiresolution peak pyramids for waveform visualization |
| `model3d/` | 3D mesh comparison execution |
| `hex` | Byte-level difference execution |
| `digest` | MD5 / SHA-1 / SHA-256 / SHA-512 digests for every `HashAlgorithm`; `ComputeDispatcher::hash_bytes_with` runs them on the Rayon pool |
| `binary/` | LIEF-backed extraction of PE / ELF / Mach-O into `BinaryModel` (feature `binary`, on by default): headers and metadata, sections and segments, exported / imported / local symbols, per-function SHA-256 and SimHash, strings. Read-only: no LIEF mutator or builder is ever called |
| `table` | CSV row alignment and cell diff execution |
| `shaders/` | WGSL compute shader sources |

---

### `openbc-engine`
**Async orchestration and bounded progress pipelines.**

- Owns the Tokio runtime boundary: all async I/O and channel coordination live here
- Requests directory levels progressively from VFS, compares metadata first, then schedules content work
- Folder compare strategy per file comes from `openbc_core::MatchPolicy` (`compare_folder_sides_with_policy`): a hash algorithm per file type, or size + timestamp, with the explicit "compare contents" / "ignore timestamps" toggles still overriding it
- Emits `ScanEvent` values over bounded `tokio::sync::mpsc` channels
- One actor per comparison tab (folder compare, image compare, media compare, **binary compare**…)
- Option changes (tolerance, mode, offset…) travel on a latest-wins `watch` channel — supersedes in-flight passes without cancellation races
- May depend on core, VFS, and compute; must never touch Qt state

---

### `openbc-ui-qt`
**Qt event-loop integration, virtualized model state, and C ABI bridges.**

- The only crate allowed to contain Qt or CXX-Qt bindings
- Consumes `ScanEvent` progress messages and never performs scans itself
- Qt model methods run only on the Qt GUI thread
- A C ABI layer (`image_compare_ffi.rs`) re-posts Rust worker-thread events to the GUI thread
- `QAbstractItemModel` + `QTreeView` for folder compare with virtual row loading
- `AudioSpectralDiffWidget` — copies RGB buffer into `QImage`, paints visible viewport, coordinates playhead
- `MediaCompareViewer` + `AudioDiffVisualizer` — peak pyramid display above metadata table
- `BinaryCompareViewer` + `BinaryTreeModel` — virtualized `QAbstractItemModel` / `QTreeView` over the backend's pre-order row list; read-only
- Tools > Options > **Folder Compare** page edits the file-matching policy (`options/match_policy.h`); the policy is pushed to the backend as one text line (`openbc_match_policy_set`)

---

### `openbc-observability`
**Tracing and telemetry.**

- Wraps `tracing`, `tracing-subscriber`, and `tracing-appender`
- Structured log output and optional file appender
- Initializes the global subscriber; telemetry hooks for performance instrumentation

---

### `xtask`
**Build and development automation.**

- A Cargo xtask binary — not a runtime library dependency
- Encapsulates platform-specific build steps, code generation, packaging, and CI tasks
- Run via `cargo xtask <command>`

---

## Thread Safety and Scheduling

| Thread type | Allowed work |
|-------------|-------------|
| Qt GUI thread | Qt model mutations, widget paint, event dispatch |
| Tokio worker threads | Async I/O, channel reads/writes, VFS metadata |
| Rayon thread pool | CPU-bound hashing, diffing, image/audio decode, FFT |
| GPU submission thread | wgpu command encoding, buffer mapping (off Tokio) |

**Key invariants:**
1. Qt model state is never mutated from a Tokio task
2. Tokio worker threads never perform blocking I/O or CPU-heavy work
3. Rayon pool size should be configured explicitly when embedding OpenBC
4. `ScanEvent` values are owned, serializable — safe to send across thread boundaries
5. Backpressure is intentional: a full progress channel pauses producers rather than allowing unbounded memory growth

---

## Failure Model

- VFS failures are attached to the affected path and emitted as scan events
- A single unreadable file must not terminate unrelated sibling work
- Fatal configuration errors and channel-closure errors terminate the scan with an explicit error event
- Memory bounds: image event channel holds ≤ 4 events; files above `max_file_bytes` are rejected per path

---

## Image Compare Detail

```
openbc-core::image_compare   →  options, verdicts, PixelProbe, reference PixelRenderer
openbc-compute::image        →  Rayon decode/resize/compare + wgpu kernel (above threshold)
openbc-engine::image_compare →  one actor per tab, ImageByteSource, bounded mpsc channel
openbc-ui-qt                 →  C ABI + callback → re-post to GUI thread; hover probe = sync read
```

## Audio Spectral Compare Detail

```
openbc-compute::audio_compare  →  PCM decode, channel power averaging
openbc-compute::spectral_diff  →  Hann-windowed STFT → RGB888 (neutral/magenta/cyan encoding)
openbc-ui-qt::AudioSpectralDiffWidget  →  QImage paint, playhead, viewport callbacks
```

## Binary Compare Detail

```
openbc-core::binary_compare     →  BinaryModel, diff_binaries, build_tree / flatten (pure, no parser)
openbc-compute::binary          →  LIEF extraction (PE / ELF / Mach-O), SHA-256 of file / sections / functions, SimHash
openbc-engine::binary_compare   →  one actor per tab; parse both files on Rayon, diff, publish rows; latest-wins ShowUnchanged
openbc-ui-qt                    →  C ABI (ffi/binary_compare.rs) → BackendChannel → BinaryTreeModel / QTreeView
```

- **Identity, not address.** Sections match by name, symbols by kind + name, functions by name, strings by text. A function that only moved is *Unchanged*; one whose bytes changed is *Modified* and reports a SimHash similarity percentage.
- **Hashing tiers:** file (SHA-256, streamed, never loaded whole), section (SHA-256 of content), function (SHA-256 of the raw bytes plus a 64-bit SimHash). Formats that record no function size (PE, Mach-O) use the distance to the next function start, flagged `size_estimated`.
- **Result tree:** Header & Metadata, Sections & Segments, Symbol Table / Functions (Exported, Imported, Removed, Added, Local, Functions), Strings. Group nodes carry the aggregate state.
- **Per-side failures:** each file is parsed independently; an unreadable or unsupported file is reported against its own path. Files larger than the configured limit are rejected per path.
- **Optional dependency:** without the `binary` feature the actor still exists and reports "built without binary compare support" per side, so the C ABI is identical in every build.
- **Local paths only:** LIEF parses from a path, so remote (VFS) files are downloaded to a local copy first (`openbc_remote::localCopy`).
- **Reach:** `.exe .dll .sys .ocx .so .dylib .elf ...` open here from Folder Compare, drag and drop, the Home tile, `openbc --compare version|binary-diff|exe|dll|elf`, and the folder context menu "Compare as Binary". Session kind: `version-compare`.

## Folder Compare File Matching Detail

```
openbc-core::hash_policy          →  MatchPolicy: extension → size/timestamp | MD5 | SHA-1 | SHA-256 | SHA-512
openbc-compute::digest            →  digests for every algorithm
openbc-engine::folder_compare     →  compare_folder_sides_with_policy: sizes first, then the policy's method
openbc-ui-qt (ffi/match_policy)   →  process-wide policy installed from preferences.ini, read by vfs_bridge
openbc-ui-qt (options/)           →  Tools > Options > Folder Compare edits the one-line policy
```

- **Default is MD5.** Built-in rules: binaries, archives and media by MD5; text and source code by size + timestamp; unknown extensions by MD5. All of it is user-editable (add types, change extensions, pick the algorithm) and **Reset to Defaults** restores it.
- **Order of decisions per same-named file pair:** different sizes → *Different* (no bytes read). Otherwise: toolbar *Compare contents* hashes every file (algorithm from the policy); toolbar *Ignore timestamps* compares sizes only; else the policy decides — `Hash(a)` compares `a`-digests and ignores timestamps, `SizeTimestamp` compares timestamps and reads no bytes.
- **Matching off** (Options) restores the previous behaviour driven only by the toolbar toggles.
- **Single source of truth:** the extension tables live only in `hash_policy.rs`; C++ asks the backend for the built-in line (`openbc_match_policy_builtin`).

## Media / Waveform Compare Detail

```
openbc-engine::media_compare  →  three peak pyramids (left, right, |diff|) on Rayon
openbc-ui-qt::MediaCompareViewer  →  copies pyramids into AudioDiffVisualizer; linked seek
```
