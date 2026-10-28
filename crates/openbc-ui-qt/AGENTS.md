# openbc-ui-qt - rules for adding or changing UI (humans, Copilot, Claude)

Read this before touching the crate. Keep changes **local to the folder of the feature** and reuse the shared
widgets below; do not create new one-off controls when a shared one exists.

## 1. Layout (Rust + C++ hybrid)

```
crates/openbc-ui-qt/
  Cargo.toml  build.rs            cxx-qt + cc build; compiles every cpp/**/*.cpp (except cpp/tools)
  src/                            RUST ONLY (Cargo convention)
    lib.rs main.rs
    ffi/<viewer>_compare.rs       C-ABI shim for one viewer; pairs with cpp/viewers/<viewer>/
    ffi/common.rs                 helpers shared by the shims (input_text, handles ...)
    highlight_bridge.rs           cxx-qt QML bridge
  qml/                            QML files
  cpp/                            C++ ONLY; this directory is the include root
    app/        main_window.h, qt_app.cpp (main), cli_options.*, dependency_check.*
    common/     shared widgets/helpers: path_selector.h, line_number_editor.h, minimap.h,
                find_replace_bar.h, diff_*.h, file_kinds.h, compare_extensions.h
    style/      qt_style.h (palette, stylesheet, ALL icons: icons::Glyph)
    backend/    backend_api.h, backend_channel.h, compare_backend_api.h
    remote/     SFTP/FTP/SMB profiles, dialogs, vfs bridge
    home/       home_view.h, session_history.h
    viewers/<name>/   one folder per compare tab
    tools/      standalone executables (depcheck) - own main(), never linked into the GUI
  docs/         REFACTOR_SAVEPOINT.md (progress log), this file lives at the crate root
```

Includes always start at `cpp/`: `#include "common/path_selector.h"`. Never `../` and never a bare filename.
Rust symbol names (`openbc_*`) are the C++/Rust contract; moving a Rust file never changes them.

## 2. Naming

| Thing | Rule | Example |
|---|---|---|
| Viewer header | `<kind>_compare_viewer.h` in `viewers/<kind>/` | `viewers/table/table_compare_viewer.h` |
| Viewer class | `<Kind>CompareViewer` | `TableCompareViewer` |
| Rust shim | `src/ffi/<kind>_compare.rs` | `src/ffi/table_compare.rs` |
| C API header for the shim | `<kind>_compare_api.h` next to the viewer | `picture_compare_api.h` |
| Session-history kind | `"<kind>-compare"` | `"table-compare"` |

Kinds today: folder, text, picture, model3d, table, registry, hex, media.

## 3. Every compare viewer must

1. **Path rows**: use `PathSelector` (`common/path_selector.h`). Single-file-per-side viewers build rows with
   `makeCommitSelector(...)` and implement `commitPath(side, path)` (validate -> confirm discard if dirty ->
   store -> `startSession()`/`compare()`). No hand-made `QLineEdit` + "..." button, no read-only `QLabel` paths.
2. **Row-based diffs** show `DifferenceOverview` (`common/minimap.h`) on the left edge. **Text editors** use
   `LineNumberEditor`. Viewers with neither (picture, 3D, media) have nothing to add.
3. **Icons** only through `openbc::ui::icons::glyph(Glyph::X)` / helpers in `style/qt_style.h`. Add a new
   `Glyph` there instead of using `QStyle::standardIcon` or image files. Colours come from `openbc::ui::color`.
4. **Tab plumbing** (close button, title, tooltip, history) goes through `main_window.h`
   (`addViewerTab`, `attachCloseButton`). Do not build a second close button by hand.
5. **Esc closes the tab.** The application shortcut in `main_window.h` handles it for every viewer except Home and
   the folder compare viewer. Do not add per-viewer Esc handling; widgets that need Esc for themselves (find bar,
   popups) already take priority.
6. Expose the standard surface used by `main_window.h`: `title()`, `detailedTitle()`, `leftText()`, `rightText()`,
   `actionsMenuItems()/editMenuItems()/searchMenuItems()/viewMenuItems()` (as applicable),
   `onHomeRequested`, `onSessionsRequested`, `onTitleChanged`, `onComparisonRequested` (when it records history).
7. Be registered in four places: `file_kinds.h` (`ViewerKind`, `classifyFilePath`, `sessionKindForViewer`,
   `canonicalSessionKind`), `home_view.h` (`sessionTypes()`), `main_window.h` (`add<Kind>View`, `onNewSession`,
   `onOpenHistory`, `duplicateTab`, `rebuildMenus`, `updateWindowTitle`) and `cli_options.cpp` (`kindForType`).

## 4. How to add a new compare viewer (checklist)

1. `cpp/viewers/<kind>/<kind>_compare_viewer.h` (+ `.cpp` if more than a thin class, see 5) and, if it needs the
   Rust backend, `src/ffi/<kind>_compare.rs` registered in `src/ffi/mod.rs` and `<kind>_compare_api.h`.
2. Build the header row with `makeCommitSelector`; add `DifferenceOverview` if the diff is a list of rows.
3. Register in the four places in 3.7. 4. Update `docs/REFACTOR_SAVEPOINT.md` if you finish a tracked item.
No CMake or build.rs change is needed: headers and `.cpp` files under `cpp/` are globbed.

## 5. `.h` only vs `.h` + `.cpp`

Standard practice (and the target here): declarations in `.h`, definitions in `.cpp`, so one edit rebuilds one
translation unit. The legacy viewers are still header-only and compiled as a single unit through
`app/qt_app.cpp`. Rules:
* **New code**: `.h` + `.cpp` pair in the same folder. Header-only is allowed only for tiny inline helpers.
* **Existing viewers**: split opportunistically when you are already changing the file (one viewer per change;
  keep behaviour identical; move `std::function` callbacks and Qt wiring as-is).
* Anything with `Q_OBJECT` must be in a header under `cpp/` (AUTOMOC runs on the glob).

## 6. Platform rules (Windows + Linux)

* OS-specific code only behind `#ifdef _WIN32` / `Q_OS_WIN` / `Q_OS_LINUX`, kept in `app/` or a clearly named
  helper, never scattered through viewers. Use `QDir::homePath()`, `QStandardPaths`, `QDir::toNativeSeparators`
  for display; never hard-code `/` or `\\` or `$USER` (use `USERNAME` on Windows).
* The Windows exe is a GUI-subsystem program: anything meant for a console (`--help`, errors,
  `--dependency-check`) must print through `printToConsole()` (attaches the parent console).
* `app/dependency_check.*` and `tools/depcheck` must stay **Qt-free**: they run when Qt cannot load.

## 7. Command line

`openbc [--compare folder|text|picture|3d|table|registry|hex|media] [LEFT [RIGHT]]`, `--dependency-check`,
`--version`, `--help`. Auto-detect lives in `cli_options.cpp` (`resolveLaunchKind`) and reuses `file_kinds.h`;
add new kinds there, never in `main_window.h`.

## 8. Before you finish

* Every `#include` resolves from `cpp/`; no leftover old names (`CompareSession`, `*_compare_view.h`).
* Windows and Linux both considered; no new global state in headers (single-TU ODR still applies).
* Update the savepoint file when you complete a tracked item.

## 9. Drag-and-drop and reload

Viewers never parse drops or watch files themselves. A viewer exposes `applyDroppedPaths(left, right)` (empty = leave that side) and, for reload, creates a `FileChangeController` (`common/file_change_controller.h`) and calls `track()` right before loading. Routing policy: `common/drop_router.h`; details and limits: `docs/DROP_AND_RELOAD.md`. Unit tests for the policy: `cpp/tools/drop_router_test` (see its header for the g++ line).