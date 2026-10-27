# OpenBC Wiki

Welcome to the **OpenBC** project wiki — the central knowledge hub for contributors, users, and anyone curious about the project.

---

## 📚 Table of Contents

| Page | Description |
|------|-------------|
| [Vision & Goals](Vision.md) | Why OpenBC exists and where it is headed |
| [Architecture Overview](Architecture.md) | Crate layout, data flow, and design invariants |
| [Design Principles](Design-Principles.md) | Performance, safety, and extensibility decisions |
| [Getting Started](Getting-Started.md) | Prerequisites, build instructions, first run |
| [Contributing](Contributing.md) | How to contribute code, tests, and documentation |
| [Coding Guidelines](Coding-Guidelines.md) | Language standards, naming, error handling |
| [Crate Reference](Crate-Reference.md) | Per-crate responsibility and public API overview |

---

## What is OpenBC?

**OpenBC** is an open-source, native file and folder comparison tool built in Rust with a Qt desktop interface. It is designed to be a fast, transparent, and community-driven alternative to Welknown `[Compare Softwares]`.

OpenBC supports:
- **File & Folder Comparison** — Text, binary, image, audio, video, table, hex, 3D model, and Windows registry
- **Remote Filesystems** — FTP and SFTP adapters through a virtual filesystem layer
- **GPU Acceleration** — Optional wgpu-based compute for large assets
- **Platform Integration** — Nautilus (Linux) and Windows Explorer context menus

---

## License

OpenBC is dual-licensed under:

- [MIT License](../LICENSE-MIT.md)
- [Apache License, Version 2.0](../LICENSE-APACHE.md)

You may choose either license at your option.
