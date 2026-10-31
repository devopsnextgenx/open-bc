# OpenBC

## Support the Project

OpenBC is built with passion as a free, open-source alternative to commercial comparison tools. Maintaining the codebase, improving performance, adding new filesystem adapters, and polishing the Qt interface takes a lot of time and dedication. 

If OpenBC has saved you time, helped you review code, or streamlined your workflows, please consider supporting its continued development! Your contributions help keep the project alive and growing.

<p align="left">
  <a href="https://paypal.me/AmitKshirsagar13" target="_blank">
    <img src="https://img.shields.io/badge/Donate-PayPal-00457C?style=for-the-badge&logo=paypal&logoColor=white" alt="Donate with PayPal" />
  </a>
  &nbsp;&nbsp;
  <a href="https://ko-fi.com/devopsnextgenx" target="_blank">
    <img src="https://img.shields.io/badge/Support-Ko--fi-FF5E5B?style=for-the-badge&logo=kofi&logoColor=white" alt="Support on Ko-fi" />
  </a>
</p>

*Thank you for helping keep OpenBC open, fast, and free for everyone!*

## OpenBC image compare

Picture Compare tab (Tol / Range / Blend, offset drag, zoom, 9x9 pixel details, auto-resize)
backed by core -> compute -> engine -> ui-qt, per ARCHITECTURE.md.

Start with `cargo/module-registration.md`, then the `cargo/*.snippet` files, then `patches/main_window.patch`.

NOT COMPILED: this package was written without rustc or Qt available. Run
`cargo test -p openbc-core -p openbc-compute -p openbc-engine -p openbc-ui-qt` first; expect to fix a
few small compile errors (crate-version API drift in `image`, imports).

## OpenBC file/folder compare

OpenBC is an open-source, native file and folder comparison tool. It aims to become a capable, transparent alternative to Welknown `[Compare Software]`, built in the open with a modern Rust core and a Qt desktop interface.

The project is actively evolving. The goal is not only to reproduce familiar comparison workflows, but to build a fast, extensible tool that the community can inspect, improve, and adapt.

## Product preview

These project assets show the visual direction for the desktop experience and its file-oriented workflows:

| Workspace | File operations | Comparison context |
| --- | --- | --- |
| ![OpenBC workspace](docs/imgs/x-home.png) | ![OpenBC folder workflow](docs/imgs/x-folder.png) | ![OpenBC context menu](docs/imgs/x-context-menu.png) |

Video showing demo as below.

![open-bc-long-demo.gif](./docs/imgs/open-bc-long-demo.gif)


Additional UI and activity imagery is available in [`docs/imgs`](docs/imgs/).

## What is being built

- Native desktop comparison workflows for files and directories
- Lazy, level-by-level tree loading so large folders do not require an upfront recursive scan
- Metadata-first comparison followed by content work when needed
- Local and remote filesystem adapters through the VFS layer
- CPU-backed processing with a path toward capability-aware GPU acceleration
- A Qt model and UI that remain responsive while asynchronous work progresses
- An open architecture that keeps domain logic, filesystem access, compute, orchestration, and UI independently testable

The current architecture is documented in [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Project structure

| Crate | Responsibility |
| --- | --- |
| `openbc-core` | Domain types, comparison policy, and diff algorithms |
| `openbc-vfs` | Filesystem and remote protocol adapters |
| `openbc-compute` | CPU/GPU execution backends |
| `openbc-engine` | Async orchestration and progress pipelines |
| `openbc-ui-qt` | Qt integration and virtualized model state |
| `xtask` | Build and development tasks |

## Getting started

### Prerequisites

- Rust with Cargo
- Qt development libraries and a C++17 toolchain
- CMake and a supported build tool such as Ninja or Make

### Check the workspace

```sh
cargo check --workspace
cargo test --workspace
```

For the Qt application, use the repository's platform setup and build scripts as described in [`build-launch.sh`](build-launch.sh) and [`setup.sh`](setup.sh).

### File manager integration

The Windows MSI adds Explorer actions for selecting a left file or folder, comparing the next matching item to it, and comparing two selected items directly. Debian packages install a Nautilus context-menu extension (provided by `python3-nautilus`); restart Nautilus after installation to load it. Select one left item and then choose its matching `Compare to Left` action, or select exactly two files or two folders and choose `Compare`. The `openbc-install-nautilus` command remains available to add equivalent actions to Nautilus' Scripts menu on systems without the extension API.

## Contributing

OpenBC will be better with people who care about reliable, fast, and user-friendly comparison tools. Connect with the project by opening an issue, starting a discussion, or sending a pull request with a focused change.

Useful ways to contribute include:

- Build and run the project, then report reproducible bugs and UX friction
- Improve file and folder comparison behavior in `openbc-core`
- Add or harden local and remote adapters in `openbc-vfs`
- Improve hashing, diffing, scheduling, and performance in `openbc-compute` and `openbc-engine`
- Shape the Qt experience in `openbc-ui-qt`
- Add tests, benchmarks, platform support, and documentation
- Share workflows that Byond Compare handles well and that an open alternative should support

Before making a larger change, read [`docs/CODING_GUIDELINES.md`](docs/CODING_GUIDELINES.md) and [`ARCHITECTURE.md`](ARCHITECTURE.md). Small, focused pull requests with tests and a clear explanation of the user problem are especially welcome.

## Community goal

A strong open-source alternative to Byond Compare should be shaped by the people who use comparison tools every day. If you have experience with code review, release engineering, data migration, backups, binary files, remote systems, or accessibility, your perspective can help set the direction.

Connect, contribute, and help us build OpenBC into a dependable comparison tool that belongs to its users.

## License

OpenBC is dual-licensed under either of:

- [MIT License](https://opensource.org/licenses/MIT)
- [Apache License, Version 2.0](https://www.apache.org/licenses/LICENSE-2.0)
