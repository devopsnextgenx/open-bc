Act as a Principal Rust and Qt Systems Architect specializing in high-performance concurrent computing. I am starting an open-source, ultra-fast file and directory comparison tool named "OpenBC" (inspired by Beyond Compare).

===============================================================================
1. PROJECT GOAL, VISION & ROADMAP
===============================================================================
- Primary Goal: A native, blazingly fast folder and file comparison utility for Linux desktop, cross-compilable to Windows/macOS.
- Hardware Acceleration Vision: Maximize modern hardware capabilities by combining multi-core CPU parallelism and GPU compute acceleration to make folder and binary scans bottlenecked solely by disk I/O, not CPU/compute throughput.
- Core VFS Protocols: Local Filesystem, SSH/SFTP (ssh2-rs), and SMB/Samba (via VFS traits).
- Primary Features:
  1. Folder Comparison: Rule-based metadata comparison (Size, Timestamp) and Phase-2 async binary/fast-hash (xxHash/SHA256) matching.
  2. Text File Content Comparison: Side-by-side and unified diffs with syntax awareness.
  3. Lazy Loading Architecture: Folder trees load on-demand level-by-level sequentially. NO upfront full-tree recursive scanning.
- Acceleration Targets:
  - Multi-Core CPU: Dynamic work-stealing parallelism for tree traversal and small/medium file hashing.
  - GPU Compute (`wgpu`): Offload parallel fast-hashing (xxHash/Murmur3) and large-block byte array comparison to GPU compute shaders when compatible hardware (Vulkan/Metal/DirectX) is detected.
- Future Enhancements: GPU-accelerated image diffing, media metadata comparison.

===============================================================================
2. SYSTEM ARCHITECTURE & CRATE WORKSPACE
===============================================================================
Provide a Rust Cargo workspace split strictly into decoupled crates:
  - `openbc-core`: Core domain models, comparison engine interfaces, diff algorithms, and shared types. Zero UI dependencies.
  - `openbc-vfs`: Virtual Filesystem abstraction trait (`AsyncVfs`) and implementations (Local, SFTP, SMB).
  - `openbc-compute`: Hardware abstraction layer (HAL) for multi-core and GPU acceleration:
      * CPU Backend: Rayon work-stealing thread pools for CPU-bound hashing and diffing.
      * GPU Backend: `wgpu` compute shaders for parallel memory-mapped block hashing and large binary array diffing.
      * Auto-fallback mechanism: Gracefully falls back to multi-core CPU if GPU is unavailable or for small files (< 1MB) where PCIe transfer latency exceeds CPU execution speed.
  - `openbc-engine`: Tokio-driven orchestration pipeline managing VFS stream queues, feeding chunks into `openbc-compute`, and emitting non-blocking diff results.
  - `openbc-ui-qt`: Qt6 frontend using `cxx-qt` bridging Tokio async events to `QAbstractItemModel` / `QTreeView` for virtualized lazy tree rendering.
  - `xtask`: Rust-based build automation tool driver (`cargo xtask setup`, `cargo xtask build`).

===============================================================================
3. ONE-COMMAND SETUP & BOOTSTRAP AUTOMATION
===============================================================================
- Provide a single top-level `setup.sh` bootstrap script that:
  - Auto-detects Linux package managers (`apt`, `dnf`, `pacman`).
  - Installs required dependencies: Qt6 development headers, CMake, C++ build toolchain (GCC/Clang), `libssl-dev`, `libssh2-1-dev`, Vulkan SDK (`libvulkan-dev` / `vulkan-tools`).
  - Configures `cargo-xtask` for cross-platform task invocation (`cargo xtask setup`, `cargo xtask build`, `cargo xtask run`).

===============================================================================
4. COPILOT ARCHITECTURE PRESERVATION & GOVERNANCE FRAMEWORK
===============================================================================
To prevent AI hallucination, conflicting solutions, or breaking modular boundaries during future feature development, generate the following repo-level governance files:

A. `.github/copilot-instructions.md` (System Instructions for Copilot):
   - Mandate crate boundary isolation (e.g., `openbc-core` and `openbc-compute` must NEVER import `openbc-ui-qt` or Qt C++ bindings).
   - Enforce compute offloading rules: Use Tokio for I/O bound tasks, Rayon for multi-core CPU compute, and `openbc-compute::wgpu` for large array processing.
   - Require async operations to pass through `tokio::sync::mpsc` channels rather than mutating shared UI state directly.
   - Enforce reading `ARCHITECTURE.md` before adding new modules or traits.

B. `ARCHITECTURE.md` (Source of Truth Document):
   - High-level data flow diagram showing VFS -> Tokio Engine -> `openbc-compute` (Rayon/wgpu Shaders) -> Channel -> Qt Model -> TreeView.
   - Detailed thread safety rules, CPU core saturation strategies, memory-mapped I/O policies, GPU buffer upload thresholds, and fallbacks.

C. `docs/CODING_GUIDELINES.md` (Code Style & Engineering Conventions):
   - Language Standard: Rust Edition 2021 / C++17 (where CXX is required) / WGSL for GPU compute shaders.
   - Hardware Concurrency Rules: No blocking operations on main UI or Tokio worker threads. Use dedicated compute channels for Rayon/GPU dispatches.
   - Threshold Heuristics: Offload file blocks to GPU only when size > 4MB to ensure PCIe upload overhead is amortized by GPU execution speed.
   - Error Handling: `thiserror` for library crates (`openbc-core`, `openbc-vfs`, `openbc-compute`), `anyhow` for binary execution drivers (`openbc-ui-qt`, `xtask`).
   - Naming Conventions: `snake_case` for modules/functions, `PascalCase` for structs/traits/enums, `SCREAMING_SNAKE_CASE` for constants.
   - Documentation: All public structs, traits, and functions must contain `///` doc comments with usage examples.

===============================================================================
5. REQUIRED ARCHITECTURAL DELIVERABLES FOR THIS PROMPT
===============================================================================
Please generate the initial boilerplate files and implementations for:
1. The Cargo workspace `Cargo.toml` layout and crate directory structure.
2. The `.github/copilot-instructions.md`, `ARCHITECTURE.md`, and `docs/CODING_GUIDELINES.md` governance files.
3. The `setup.sh` bootstrap script and `xtask` Cargo runner setup.
4. The core `AsyncVfs` trait in `openbc-vfs` supporting async `read_dir`, `stat`, and `open_file`.
5. The `openbc-compute` HAL interface demonstrating CPU Rayon pool worker dispatch and `wgpu` compute shader initialization with auto-fallback.
6. The non-blocking two-phase Folder Scanning Engine skeleton in `openbc-engine` using Tokio channels.
7. The Qt6 bridge design (`cxx-qt`) mapping Tokio background scan progress into a virtualized `QAbstractItemModel`.