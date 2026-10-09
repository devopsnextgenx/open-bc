# Refactor savepoint (resume here)

Last updated: 2026-10-09. Nothing below has been compiled yet - the environment that did the refactor had
no Qt, CMake or Cargo. **First action on resume: build on Linux and Windows and fix compile errors.**

## Goals (verbatim intent) and status

| # | Goal | Status |
|---|------|--------|
| 1 | All compare tabs use PathSelector | DONE in code (text, folder, media already; picture, 3D, table, registry, hex converted). UNBUILT |
| 2 | LineNumberEditor / minimap used appropriately | AUDITED, no change needed: LineNumberEditor only fits the text editor; minimap already in text, table, registry, hex; picture/3D/media/folder have no row-diff list |
| 3 | Naming `<kind>_compare_viewer.<ext>` | DONE (files + classes: FolderCompareViewer, TextCompareViewer, PictureCompareViewer, Model3dCompareViewer; image_compare_* -> picture_compare_*) |
| 4 | Folder structure; .h vs .cpp; where .rs lives | Folders DONE (cpp/ by feature, Rust in src/ffi). Policy decided in AGENTS.md. The actual .h -> .cpp split of legacy viewers is NOT done (see next steps) |
| 5 | CMakeLists handles all headers | DONE (recursive glob, include root `cpp/`, openbc-depcheck target, install) |
| 6 | Consistent style/icons/layout | PARTIAL: one shared close-button helper, one path row. Not done: common viewer base class, table-driven menus/title code in main_window.h |
| 7 | Windows + Linux consistency | PARTIAL: console attach, Qt plugin preflight, USERNAME fix, platform-specific build dirs. Not audited: remaining hard-coded `/var/log` log-path logic in "Download Log" |
| 8 | Esc closes every tab except folder compare | DONE (main_window.h application shortcut; Home also exempt) |
| 9 | CLI: two optional paths, auto-detect, `--compare` | DONE (cli_options.*, main_window.h launch, main.rs forwards argv). UNBUILT |
| 10 | Instruction doc for Copilot/Claude | DONE (crate-root AGENTS.md). Point `.github/copilot-instructions.md` / `CLAUDE.md` at it |
| 11 | Missing-dependency messages + `--dependency-check` | DONE: Qt-free core tested on ELF + synthetic PE; `openbc-depcheck` tool; Windows platform-plugin preflight. PE path never run on real Windows |
| 12 | Unified build script `-c -p -s` | DONE: scripts/build.sh (dry-run tested). deb/rpm/msi logic ported/new; flatpak manifest is a scaffold |
| 13 | Savepoints to resume | This file |

## Next steps, in order

1. Build (Linux: `scripts/build.sh -p lin`; Windows MSYS2 UCRT64: `scripts/build.sh -p win`) and fix errors.
2. Smoke test: `openbc a.txt b.txt`, `openbc dirA dirB`, `openbc --compare hex x y`, `openbc --dependency-check`,
   Esc on each tab, path edit + Enter in table/registry/hex/picture/3D.
3. Introduce `common/compare_viewer_base.h` (virtual title/menus/leftText/rightText + callbacks) and collapse the
   dynamic_cast chains in main_window.h (rebuildMenus, updateWindowTitle, duplicateTab, closeTab).
4. Split viewers into `.h` + `.cpp`, one per change, smallest first: table, registry, hex, media, picture,
   model3d, text, folder (2.5k lines, last).
5. Split main_window.h (tab management vs menus vs launch).
6. Delete old scripts: win-build.sh, build-deb.sh, build-msi.sh, build-pkg.sh (superseded by scripts/build.sh).

## Known risks to check first
* `makeCommitSelector` lambdas capture `side` as `std::uint8_t`/`int`; registry casts explicitly.
* Hex/table/registry reload by `startSession()`; confirm models reset cleanly when paths change.
* Esc application shortcut vs. find bar / combo popups: behaviour was already this way for text tabs.
* build.rs compiles every `cpp/**/*.cpp` (not `cpp/tools`) into the staticlib; CMake compiles them again into
  the exe. Same as before for qt_app.cpp; watch for duplicate symbols at link time.
* Files from the archive use CRLF; new files use LF. Normalize with your usual `.gitattributes`.
