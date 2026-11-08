# Text compare: engine, two editors, Hybrid mode

Text comparison is split into a **Qt-independent engine** (Rust, `openbc-core/src/text/engine/`) and two
**presentation layers** that render the same `TextCompareModel`:

```
 TextSource (mmap | buffered)  ->  LineIndex (24-byte records, XXH3)  ->  DiffEngine (global)
                                                                              |
                         TextCompareModel  <-  DiffIndex (run-length segments, lazy inline diffs)
                                   |
        ----------------------------------------------------------
        |                                                        |
 Existing editor (QPlainTextEdit)                  VirtualTextEditor (QAbstractScrollArea)
 via PlainTextEditorAdapter                        implements ITextCompareEditor
```

Nothing in the engine depends on Qt, Qt Widgets or any GUI type, so a CLI or another UI can use it.

## Engine (`openbc-core/src/text/engine/`)

| file | responsibility |
|---|---|
| `source.rs` | `TextSource`; `MappedTextFile` (`memmap2`, buffered 1 MiB-block fallback, `verify_unchanged`); `MemorySource` |
| `encoding.rs` | UTF-8 (BOM or not), UTF-16 LE/BE (BOM or NUL heuristic); lazy lossy decode per line |
| `line_norm.rs` | streaming, allocation-free normalisation: ignore edge/all whitespace, ignore case, ignore EOL |
| `hasher.rs` | `LineHasher` trait; default XXH3-64 (`xxhash-rust`); injectable for collision tests |
| `line_index.rs` | `LineRecord {offset,hash,length,flags}` (24 B); RAM index that **spills to a memory-mapped temp file** past `ram_record_limit` (default 8 Mi records) |
| `diff.rs` | global diff; `DiffConfig`; cancellation + progress |
| `diff_index.rs` | run-length `Segment`s; row <-> left/right line (binary search); change blocks; overview strip |
| `model.rs` | `TextCompareModel`: lazy text, `line_window` for huge lines, bounded inline-diff cache, `find`, navigation, `memory_report` |
| `cache.rs` | `BoundedCache` (two-generation, hard entry limit, O(1)) |
| `text/highlight/stateful.rs` | `SyntaxEngine`/`HighlightSession`: carried lexer state, checkpoints, bounded caches |

### Diff algorithm
1. Common prefix/suffix are stripped first (sequential, byte-verified) - for "mostly similar" files this removes almost all work.
2. Remaining lines get **class ids**. Equal hashes are **verified on the real bytes** (after normalisation) before sharing a class, so a collision can never create a false match (tested with a hasher that returns a constant).
3. Alignment (explicit stack, no recursion): **unique-line anchors** (patience / LIS) -> **histogram** anchor (rarest shared line) -> **Myers** (`similar`) on what is left, with a time budget; regions above `myers_max_lines` become one coarse replace block and the result is flagged `degraded` (shown in the status bar).
4. Delete/insert runs between equal runs are paired positionally into `Replace` rows.

There are **no pages**: alignment cannot drift at page boundaries. Cancellation returns `Err(Cancelled)` and publishes nothing.

Working memory during a diff: ~4 B per differing line (ids) + ~40-60 B per *distinct* line of the differing middle region (class table, hash map, counters).

## Editors

* **Existing editor** (`TextCompareViewer`, unchanged UX). Fixes made (see "What was fixed"). Large-file mode now receives rows from the global engine alignment.
* **Virtual editor** (`VirtualTextEditor` + `VirtualTextCompareViewer`): read-only; renders only visible rows + `prefetchRows` (default 24); line/inline/syntax caches are bounded `QCache`s; `uint64` rows map to the scrollbar through `ScrollMapper` (exact ends, monotonic, identity while rows fit an `int`); lines > 16 KiB are shown as a horizontal *window* (plain text, no syntax/inline) so a 1 GB single line stays responsive; syntax is computed on a worker thread (first highlight loads the grammar set, ~2.4 s once per process) and rows paint plain until spans arrive.

Both are driven through `ITextCompareEditor` (`viewers/text/text_compare_editor.h`); `PlainTextEditorAdapter` adapts the existing editor. The same interface test runs against both.

Feature matrix (Virtual editor): line numbers, current-line highlight, changed/inserted/deleted rows, inline differences, syntax highlighting (multi-line aware), selection (mouse, keyboard, double-click word), copy (capped at 64 MB), Ctrl+A, find / next / previous (case option, wraps), go to line, next/previous difference, synchronised vertical + horizontal scroll, overview bar, file-change notices + explicit reload, swap sides, theme font/colours (`TextFont`, appearance keys `File Views/...`), context menu, **write mode, block copy, undo/redo, save, Rules** (below).

## Write mode (Virtual editor)

The tab opens **read-only**. A lock icon at the right of the status bar shows the state: **red lock = read-only, green open lock = editing**. Clicking it toggles write mode. Any attempt to edit while locked (typing, Backspace, Delete, Enter, Tab-in-write-mode, paste, cut, IME input, undo/redo, the copy arrow, the Copy button) leaves the text untouched and **flashes the lock red -> blue -> yellow -> red, 100 ms per colour, three rounds = 1.2 s**; every further attempt restarts the flash. (Tab keeps normal focus navigation while read-only.)

**How edits work without a `QTextDocument`.** Each side is an `EditableSource`: the memory-mapped file plus an append-only edit buffer, presented as one address space. Edited text is appended there, so untouched lines never move and the original mapping is never modified. The line index gets an *overlay* (a piece table of line records: `Base(range) | Edit(records)`), so a splice costs O(overlay segments), not O(file). The diff is kept consistent *locally*:

| edit | alignment update |
|---|---|
| one line replaced by one line (typing) | only that row is re-classified (Equal <-> Replace) |
| lines inserted / deleted / joined / split / pasted / block copied / undo / redo | the change block(s) around the edit are re-aligned with the normal diff engine and spliced into the run-length `DiffIndex` (`diff_region`, `splice_region`) |
| "Recompare" (`rediff`) | full global alignment again (restores the optimum after many local edits) |

Undo/redo are per side and store *records* (24 B per line), not text; consecutive single-character typing on one line is one undo step until the caret moves. Memory for edits is proportional to the edit (typing 500 characters in a 200 000-line file used < 20 KB). "Modified" is tracked per side by undo-stack identity, so undoing back to the saved state clears it. Edits survive Rules changes; undo survives saves.

**Copy a difference.** The gutter arrow (drawn at the first row of every change block, pointing at the other side) and the toolbar *Copy* button make the other side's lines of that block identical (insert, delete or overwrite) as one undo step. Dropped text is byte-exact when both files use the same encoding and transcoded otherwise.

**Save.** Save (Ctrl+S, active side), the per-side Save buttons, and Save As write a temporary file in the target's directory and atomically rename it over the original (symlinks are followed; permissions kept). UTF-8/UTF-16 encoding, BOM, every line's original ending (a replaced line keeps its ending, new lines use the file's dominant ending) and a missing final newline are preserved. A side without a file asks for a name. Our own write is re-baselined with the file-change controller so it does not raise a "changed on disk" notice.

**Unsaved changes.** Closing the tab or window, Swap, Reload, changing a path, dropping new files and "reload latest" after an external change all go through the same `confirmUnsavedChanges` dialog as the Existing editor (Save Left / Save Right / Continue / Cancel).

**Rules.** The toolbar *Rules* button opens a bar with the **same syntax as the Existing editor**: comma-separated samples (digit runs match any digits, other characters are literal, `,,` is a comma) plus `/regex/` entries. Matches are replaced by a token before hashing/comparing, so lines that differ only inside matches (or lines a rule covers entirely) count as matched. Implemented once in the engine (`engine/rules.rs`, reusing the existing sample-to-pattern code), applied to the index, to diff verification, to edited lines and to undo; an invalid regex is rejected without changing anything; rules persist across reloads of the tab. Lines over 1 MiB are matched without rules (the same on both sides).

## Tools > Options > Text Compare
`Existing Editor` | `Virtual Editor` | `Hybrid Mode (Recommended)`; threshold value + `KB/MB/GB` (binary units, 1 KB..64 GB, default **3 MB**), stored as `textCompare/editorMode|thresholdValue|thresholdUnit` in `preferences.ini`; invalid values fall back to defaults. Applies to comparisons opened afterwards; open tabs are never switched.

### How Hybrid chooses (decided in `addTextView`, before any content is read)
1. Explicit `Existing` / `Virtual` win.
2. Hybrid: if **either** file is `>=` threshold -> Virtual; if either cannot be sized (non-regular file) -> Virtual; else Existing. Missing/new files count as 0 bytes.
3. The Virtual editor needs local regular files. A side that is remote (`scheme://`) or in-memory/clipboard text forces the Existing editor and the status bar says so.

Logic: `options/text_editor_selection.h` (Qt-free), `viewers/text/text_editor_choice.h` (file facts).

## What was fixed in the existing editor
| # | problem | fix |
|---|---|---|
| 1 | paged large mode appended everything to one `QTextDocument` | **Not fixed inside this widget** (inherent to `QPlainTextEdit`). Mitigated: the existing 500 MiB cap is kept, and Hybrid mode sends large files to the Virtual editor, which never builds a document. |
| 2 | per-line / inline metadata accumulated | **Partly fixed**: the highlighter's cache is bounded (8192 lines). The viewer's own per-row vectors (`rows_`, inline segments) still grow with the rows loaded into the document, as before. |
| 3 | page-by-page diff drifted | replaced by one global engine alignment (`loadLargePage`/`appendNextLargeModelPage`) |
| 4 | unbounded highlighter caches | `QHash`+`QSet` replaced by `QCache` + stateful Rust session |
| 5 | `toPlainText().toUtf8()` | UTF-8 buffer built block-by-block (no full `QString` copy) |
| 6 | no lexer state across lines | incremental path uses `HighlightSession` (state carried + checkpoints) |
| 7 | one block per line | unavoidable in `QPlainTextEdit`; Virtual editor has none |
| + | `highlightBlock` scanned *all* spans for *every* block (quadratic) | binary search by line |
| + | Qt's own format notifications cleared the incremental cache | ignored via `applyingFormats_` |
| + | `text::large`: `DefaultHasher`, O(n) offset lookup, file reopened/copied per read | XXH3, binary search, no cache copy |

## Limitations (honest list)
* The Virtual editor has no replace, no word wrap and no "Minor" row filtering. Rules in the Existing editor's *large-file* mode are still not applied (its global alignment comes from the engine with default options).
* Editing limits: lines over 16 KiB are shown as a window and cannot be edited; line-ending / encoding conversion is not offered; drag-and-drop of selected text is not implemented; the whole undo history lives in memory (100 000 steps per side, records only).
* **Windows save is untested.** A memory-mapped file cannot be replaced on Windows, so the viewer reopens the files with buffered I/O the first time write mode is unlocked (the engine's buffered path is tested on Linux, including saving through it), after which the atomic rename should succeed. This was not run on Windows.
* A single *Save* holds the model's write lock while writing; typing waits until it finishes (sequential write, about 1-2 s for 500 MB).
* Replace-row pairing is positional inside a changed block (the Existing editor's small-file path still uses its fuzzy pairing).
* Find in the Virtual editor runs on the GUI thread (cancellable in the engine, not yet from the UI); a no-match search over a multi-GB file blocks until done.
* Syntax: tree-sitter languages (Rust/Python/JS) have no incremental line API, so sessions use the matching syntect grammar -> slightly different colours than the Existing editor for those languages in large-file mode. Far jumps into a file replay at most 50 000 lines and may start from a fresh state (`is_approximate`).
* Mapped files: truncation by another process while open can fault; `verify_unchanged` + the file watcher reload are the mitigation.
* Encodings: UTF-8, UTF-16 (LE/BE). Others decode as UTF-8 with U+FFFD. UTF-16 lines are buffered whole while indexing.
* Lines > 4 GiB are split into `CONTINUED` records; UTF-16 lines > 4 GiB are rejected.
* Existing editor memory in large mode stays O(lines): document blocks plus `rows_`/inline vectors (see fixes #1, #2). Use the Virtual editor for files that do not fit comfortably.
* Stale duplicates `cpp/app/app_options.h` and `cpp/app/options_dialog.h` (older copies of `cpp/options/`) were left untouched; nothing includes them.

## Tests
| suite | how to run |
|---|---|
| engine unit tests (157) + real-file tests (5) | `cargo test -p openbc-core` |
| C ABI shim | `cargo test` in a crate that includes `ffi/text_engine.rs` (needs no other workspace crate) |
| VirtualTextEditor / tab / loader / adapter / write mode / lock flash / save / unsaved dialog / rules | `cpp/tools/virtual_editor_test/build_and_run.sh <dir with Rust staticlib>` (Qt 6, offscreen; `backend_stubs.cpp` provides a polling stand-in for the engine's file watcher) |
| existing SyntaxHighlighter | `cpp/tools/syntax_highlighter_test/build_and_run.sh <dir>` |
| settings persistence + Hybrid decision with real files | `cpp/tools/text_settings_test/build_and_run.sh` |
| selection logic (Qt-free) | `g++ -std=c++17 -I cpp cpp/tools/text_editor_select_test/text_editor_select_test.cpp` |
| benchmarks | `cargo run --release -p openbc-core --example text_engine_bench -- --sizes 3,50,100,500` |

`cpp/tools/text_memory_test` (existing) compiles against the changed viewer but needs the full backend library; it was **not executed** in this environment.

## Benchmarks (Linux x86-64, release build, 4 cores, page cache warm)
Each side the stated size; "first rows" = open -> model built -> first 60 rows decoded. "heap" is anonymous memory after the model is built (mapped files and the spilled index are file-backed and excluded); RSS includes mapped pages the kernel can drop.

| scenario | size/side | lines | index ms | diff ms | first rows ms | scroll µs/row | index MB | heap MB | RSS MB | peak RSS MB |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| code, edits start/mid/end | 3 MB | 55 k | 5 | 18 | 21 | 0.3 | 2.5 | 4 | 21 | 26 |
| code, edits start/mid/end | 51 MB | 920 k | 93 | 495 | 557 | 0.3 | 42 | 50 | 260 | 375 |
| code, edits start/mid/end | 105 MB | 1.84 M | 173 | 1074 | 1096 | 0.3 | 84 | 126 | 550 | 749 |
| code, identical | 105 MB | 1.84 M | 171 | 63 | 233 | 0.3 | 84 | 126 | 550 | 550 |
| code, edits start/mid/end | 532 MB | 9.2 M | 1576 | 7028 | 9027 | 0.4 | 421 | 35* | 2588 | 3503 |
| code, identical | 532 MB | 9.2 M | 1132 | 405 | 1345 | 0.3 | 421 | 35* | 2589 | 2589 |
| 13 M short lines | 132 MB | 15.0 M | 1413 | 5789 | 7495 | 0.1 | 686 | 4* | 1224 | 2040 |
| 5 distinct lines (repetitive) | 50 MB | 3.7 M | 269 | 123 | 398 | 0.1 | 171 | 200 | 405 | 405 |
| 1 MB-long lines | 199 MB | 209 | 114 | 42 | 144 | 2.1 | 0 | 28 | 829 | 829 |

\* index spilled to its memory-mapped temp file (more than 8 Mi records per side).
Syntax: ~50-70 ms per 100 lines for a cold jump into a 50-500 MB file (500-line warm-up), plus a one-time ~2.4 s grammar load per process (off the GUI thread in the Virtual editor).
Qt test: scrolling a 40 MB single-line pair grew anonymous heap by 0.0-3.9 MB (ASan build 3.9 MB).

Targets derived from these runs (regression guards, not claims): index >= 300 MB/s; diff of a 100 MB pair with scattered edits < 2 s; identical-file diff < 100 ms per 100 MB; per-row fetch < 5 µs; heap after build < 2 B per source byte for ordinary text.
