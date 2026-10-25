# Vision & Goals

## The Problem

Developers, engineers, and power users rely on file and folder comparison tools every day — for code review, release engineering, data migration, backup verification, and binary inspection. The dominant tool in this space, Welknown `[Compare Software]`, is proprietary and closed-source. Its users cannot inspect its behavior, fix its bugs, adapt it to their workflows, or trust that it works correctly on edge cases that matter to them.

There is no credible open-source native alternative with comparable breadth and quality.

## The Vision

**OpenBC** aims to become the definitive open-source comparison tool — one that professionals reach for with confidence, that the community trusts because they can read and verify every line, and that improves because many people care about it.

> _A strong open-source alternative to Compare should be shaped by the people who use comparison tools every day._

## Core Goals

### 1. Transparency
Every comparison algorithm, hashing strategy, and UI behavior is readable, auditable, and changeable. No black boxes.

### 2. Performance
- Lazy, level-by-level directory traversal — never an upfront recursive scan
- Metadata-first comparison before content work
- CPU-bound tasks parallelized with Rayon
- Optional GPU acceleration for large files (images, audio, video) via wgpu
- Bounded memory: no unbounded allocations or unmapped file growth

### 3. Extensibility
The project is designed so that each major concern — domain logic, filesystem access, compute execution, UI presentation — lives in its own crate with a clean, testable interface. New comparison modes, new remote protocols, new compute backends, and new UI frontends can be added without disrupting existing behavior.

### 4. Community Ownership
OpenBC belongs to its users. Contributions to core algorithms, adapters, UI polish, benchmarks, documentation, and platform support are all equally welcome. The project is governed by open issues, public discussions, and focused pull requests.

## Comparison Modes Planned / In Progress

| Mode | Status |
|------|--------|
| Text / Log | In progress |
| Folder / Directory | In progress |
| Image / Picture | In progress |
| Hex / Binary | In progress |
| Audio (waveform + spectral) | In progress |
| Video / Media | In progress |
| Table / CSV / Spreadsheet | In progress |
| 3D Model | In progress |
| Windows Registry | In progress |
| Remote (FTP / SFTP) | In progress |

## What OpenBC Is Not

- **Not a version control system** — OpenBC compares two points-in-time; it does not track history
- **Not a merge tool** (yet) — merge workflows are on the long-term roadmap but are not the first priority
- **Not a web application** — OpenBC is a native desktop tool; a web frontend is not planned for the initial versions

## Long-Term Direction

- Merge editor for text and structured data
- Plugin API for community-contributed comparison modes
- Accessibility improvements for screen readers and keyboard navigation
- CI-friendly headless comparison mode with structured output (JSON / SARIF)
- Cross-platform packages: AppImage, Flatpak, Homebrew, Winget, MSI
