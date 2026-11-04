# Copilot task: large-file text compare (memory, load time, styling parity, cleanup)

You are working in the `crates/` workspace (Rust + Qt/C++ hybrid). **Read `crates/openbc-ui-qt/AGENTS.md` and
`crates/openbc-ui-qt/cpp/options/options.md` first and follow them** (include roots start at `cpp/`, new C++ is `.h` + `.cpp`,
keep changes local to the feature folder, reuse shared widgets, update `docs/REFACTOR_SAVEPOINT.md`). Work in the phases at the
end, one commit per phase, each phase building and passing tests on its own.

## 1. Goal

Text compare must open very large local text files with bounded memory and acceptable load time, **with exactly the same
syntax highlighting and diff styling as small files**, and must return the process to its pre-open memory level when the
viewer closes.

| Requirement | Target |
|---|---|
| Supported size | 500 MiB per side minimum; 1 GiB per side must work (hard cap configurable, default 2 GiB/side) |
| Peak + steady memory, 500 MiB pair fully loaded | <= 1.5 GiB total process private bytes; design goal ~0.6 GiB |
| Memory ratio for any size | never > 100x file size; design goal <= 3x |
| 100 MiB pair: open -> fully loaded and aligned | < 60 s (design goal < 10 s on SSD) |
| 1 GiB pair: open -> fully loaded and aligned | < 8 min (design goal < 60 s on SSD) |
| First paint (any size) | <= 2 s, UI never blocks > 50 ms |
| Scrolling | <= 16 ms per frame; jump to any row <= 100 ms |
| After tab close | private bytes within +/- 20 MB of the value before open, on Windows, macOS, Linux; 5 open/close cycles show no growth |

Measure **private/committed bytes** (Linux `smaps_rollup` Private_*, macOS `phys_footprint`, Windows `PrivateUsage`), not RSS.

## 2. What is wrong today (verified in the code - fix these, do not rediscover)

`cpp/viewers/text/text_compare_viewer.h`
- Large mode (`shouldUseLargeFileMode`, threshold = 300 MiB / 8 = 37.5 MiB) still appends **every line into two `QPlainTextEdit`
  `QTextDocument`s** (UTF-16 + per-block overhead) via `appendLargeRows` / `QTextCursor::insertText` per line, plus `rows_`
  (`QVector<TextDiffLine>`), `leftInline_/rightInline_`, line-number vectors and highlighter caches.
- Paging is `kLargePageLineCount = 512` lines with a `QTimer::singleShot(100, ...)` between pages. The 4 MiB byte limit never
  binds (512 lines ~ 40 KiB). 100 MB ~ 2,400 pages ~ 4 min of pure sleep; 1 GB ~ 40 min.
- Alignment is **page-local**: `compareLargePage` diffs one 512-line page at a time, carrying up to 64 rows. A block insertion/
  deletion larger than a page can desynchronize the two files permanently. It must be a global diff.
- `releaseLargeFileResources()` (called from `app/main_window.h:576`) clears containers but never returns memory to the OS.
- The small path makes 6-8 full copies of each file (`QTextStream::readAll`, `split`, `leftOriginal_`, `rows_` strings,
  `join`, `setPlainText`, history snapshots).

`cpp/common/line_number_editor.h`
- Never calls `setUndoRedoEnabled(false)`, so every programmatic `insertText` is also stored in the document undo stack
  (a second copy of the text). `setLargeContentMode(true)` only disables the filler tracking.
- `resetEditTracking()` snapshots every block text into `snap_` (full extra copy); fine for small files only.
- Highlight/inline styling is done by `QSyntaxHighlighter` + `QTextEdit::ExtraSelection` + a `paintEvent` overlay that draws
  `.` / arrow glyphs for whitespace mismatches. This cannot be virtualized.

`cpp/common/diff_algorithms.h`
- `alignTextLines`: `left.join('\n').toUtf8()` (2 copies), one `QString` pair per row for **every** row, and
  `normalizedTextLine()` (two more `QString` copies + `remove`) for **every paired row even when unchanged**.

`cpp/viewers/text/syntax_highlighter.h`
- `refreshBackendSpans()` does `document()->toPlainText().toUtf8()` and, per span, `findBlockByNumber` + `block.text().toUtf8()`.
- Connected to `contentsChanged`, so each page append re-runs refresh/clears incremental caches.
- Incremental mode highlights one line per FFI call with no cross-line context (multi-line comments/strings are wrong).

Rust (`openbc-core/src/text/diff.rs`, `openbc-engine/src/text_runner.rs`)
- `Lines::new` allocates a `String` per line even with no normalization; `compare_buffers` returns owned `LineDiff` text for
  **every row including unchanged**; `AsyncTextCompareTask` then `chunk.to_vec()` clones again; whole files are read with
  `read_small_file` and `String::from_utf8` (fails on non-UTF-8). `TextDiff::from_slices` has no deadline.
- No allocator/trim handling anywhere. `AsyncTextCompareTask::new` builds a Rayon pool per task.

## 3. Reference behaviour (research summary - use as design input)

- Beyond Compare documents a 500 MB text-compare limit and a 4096-char default line limit (configurable to 65,536). Its own
  tuning guide says compare time is dominated by the alignment search window ("skew tolerance", default 2000 lines) and
  "align similar lines"; shrinking the window and disabling similarity pairing took a 500 MB vs 700 MB random-text test from
  > 2 h to ~20 min. => **bound every alignment/similarity search and never let it be unbounded.**
- Qt maintainers state `QTextDocument` is not meant for huge files and recommend a custom component that loads only what is
  displayed. => **virtualize the view.**
- `similar` supports deadline variants (`capture_diff_slices_deadline`, `TextDiffConfig` timeout); Myers degrades gracefully
  on timeout.

## 4. Target architecture

Three separate layers; **no layer keeps a copy of file text in memory except a bounded cache.**

```
LargeTextSource (chunked positional reads, LRU cache) ──► LineIndex (4 B/line)
        │                                                    │
        └──► LineHasher (parallel, normalized u64 -> u32 ids) ──► HunkDiff (anchors + bounded Myers + deadline)
                                                                       │
                                                  RowMap (compact; O(log n) row -> (left line, right line, kind))
                                                                       │
              Window API: text / inline diff / highlight spans for ONLY the visible rows  ──►  LargeDiffView (virtual Qt widget)
```

### 4.1 Rust core - new module `openbc-core/src/text/large/` (pure, no Qt, no tokio)

`source.rs` - `LargeTextSource`
- Open with `std::fs::File`; **positional reads** (`read_at` / `seek_read`) into an LRU of 1-4 MiB chunks (default total 64 MiB,
  configurable). **Do not use mmap** (Windows file locking, SIGBUS on truncate, uncontrollable RSS).
- Record `(len, modified)` at open; verify on each uncached read; if changed return `SourceChanged` (viewer shows the existing
  FileChangeController banner). Never panic or crash on truncation.
- Encoding: detect BOM. UTF-8 (BOM skipped) is the native path; invalid bytes decode lossily **only for the lines shown**.
  UTF-16 LE/BE: index in code units and decode per line on demand. Newlines: `\n`, `\r\n`, lone `\r`. Last line without
  newline and empty files are valid. Offsets are `u64`; line numbers in the public API are `u64` (do not use `int`).

`line_index.rs` - `LineIndex`
- Split the file into ~8 MiB segments scanned in parallel with `memchr` (SIMD); merge in order.
- Compressed storage: per 1024-line block one `u64` base offset + `u32` deltas (~4 B/line). API: `line_count()`,
  `line_range(n) -> (u64 start, u32 len)`, `line_for_offset`.
- Byte-level **common prefix/suffix** detection between the two files (memcmp-speed) *before* hashing: lines wholly inside
  the identical prefix/suffix are Equal without hashing. If both files are byte-identical, finish immediately.

`hashing.rs`
- Per line, in parallel (Rayon, chunked): apply the same normalization as `text::normalize::PreparedNormalizer` /
  `CompareOptions` (ignore whitespace, timestamps, container ids, ignore_samples) **streaming over `&[u8]`/`Cow`, allocating only
  when a line actually needs rewriting**; hash with xxh3-64 (`xxhash-rust`); intern to `u32` ids with a sharded hash map
  (collision-checked: on equal hash compare normalized bytes before assigning the same id).
- Lines longer than `max_display_line` (default 65,536 chars, same as today's 64 KiB cap) are hashed in full but displayed
  truncated with the existing `... [line truncated at 64 KiB]` marker.

`hunk_diff.rs`
- Input: two `Vec<u32>` id sequences (after prefix/suffix strip). Split into independent sub-problems at **unique-line anchors**
  (lines occurring exactly once in both files, patience/histogram style, longest increasing subsequence). Run Myers
  (`similar` with deadline, or own linear-space impl) on each sub-problem in parallel (Rayon, dedicated pool).
- Budgets (all in `DiffBudget`, defaults in `CompareOptions`-style struct, exposed in Options): `per_block_deadline` (default
  2 s), `total_deadline = max(20 s, 0.35 s per MiB of combined input)`, `skew_window` (default 2000 lines, like BC; limits how
  far Myers may search from the diagonal). On timeout/window exhaustion mark the sub-problem **Degraded** (positional
  delete+insert) and count it; the viewer shows a non-blocking status note "alignment simplified in N regions".
- Output: compact `Vec<Hunk>`; Equal regions are implicit/run-length. **Memory must be O(number of changed lines), not O(file).**
- Pairing of deleted/inserted lines into `Modified` rows must keep today's semantics
  (`Lines::flush_block` in `text/diff.rs`: positional pairing, `jaro_winkler >= fuzzy_threshold`, else Deleted+Added), but:
  compute on normalized text of at most the first 1,024 chars, cap pairing at 2,000 pairs per block (beyond that emit Deleted
  then Added), run blocks in parallel, and store decisions compactly (bitset per block).
- Per paired row also compute `whitespace_only` (same definition as `normalizedTextLine` in `diff_algorithms.h`: equal after
  removing spaces and tabs) **only for changed paired rows**, and store it in the run, so the "minor differences" toggle works
  without re-diffing.

`row_map.rs` - `RowMap`
- Runs of rows: `Equal{count}`, `Modified{count, ws_only}`, `Deleted{count}`, `Added{count}` with cumulative row prefix sums
  and left/right line starts (`u64`). `row_count()`, `row_at(row) -> RowInfo` in O(log n), `row_for_left_line`,
  `row_for_right_line`, `next_change(row, dir)`, `sections()` (maximal runs of changed rows sharing `whitespace_only`, i.e. the
  current `groupDiffRows` semantics), and `minimap_bins(n)` returning per-bin dominant kind in O(#runs + n).
- Property: expanding the RowMap for inputs < 2,000 lines **must equal** `TextCompareEngine::compare_buffers` row for row
  (kind, line numbers, similarity) when no deadline triggers - use the existing engine as the test oracle.

`window.rs` - window services (called only for visible rows, typically 30-120)
- `row_texts(first, count)` -> per row left/right text (UTF-8, truncated to the display cap) and line numbers.
- `inline_diff(row)` -> `Vec<CharDiff>` for Modified rows via existing `compute_inline_diff` (prefix/suffix), LRU-cached (<= 2,048 rows).
- `highlight_window(side, first_line, last_line, lead_in)` -> spans per line **already in UTF-16 offsets** (so C++ does no
  UTF-8 -> UTF-16 span conversion) using the existing `text::highlight` backend, language resolved exactly like
  `applyLanguageHighlighting()` (file name + user mappings), theme/style the same as `SyntaxHighlighter::Style`.
  Run the backend on one contiguous buffer: lines `[first - lead_in, last]` with `lead_in = 500` lines so multi-line
  comments/strings are right in the common case. If `highlight/mod.rs` uses syntect, additionally store a parse-state
  checkpoint every 4,096 lines computed lazily on a low-priority background task (cancellable) and resume from the nearest
  checkpoint when available. Lines longer than 4,096 chars skip syntax spans (still diff-styled). Cache the last 8 windows
  per side (invalidate on style/language/mapping change or file change).

### 4.2 Engine - `openbc-engine/src/text_large.rs`

`LargeTextSession`: owns the two sources, indexes, RowMap, caches, a **dedicated Rayon pool** (threads = min(cores, 8)) and a
cancel token. Phases reported through atomics for polling: `Opening -> Indexing -> Hashing -> Aligning -> Pairing -> Ready |
Failed | Cancelled`, with `done/total` counters. `close()` cancels, joins every task, drops the pool (thread exit releases
thread-local arenas) and all caches, then returns. `Drop` calls `close()`. Keep `AsyncTextCompareTask` for small files and
remote/virtual inputs; make its pool shared/static instead of per task.

Also add `openbc-core/src/mem.rs`: `trim_process_memory()` implemented per OS - Linux/glibc `malloc_trim(0)`; macOS
`malloc_zone_pressure_relief(NULL, 0)`; Windows `HeapCompact` on the process heap (+ `_heapmin`); no-op elsewhere - and
`configure_allocator()` called once at startup: on Linux/glibc `mallopt(M_MMAP_THRESHOLD, 1 MiB)` (also disables glibc's
dynamic threshold so large buffers are mmap-backed and unmapped on free) and `mallopt(M_ARENA_MAX, 4)`. Add `libc` /
`windows-sys` deps behind `cfg`. Also provide `memory_stats()` (private bytes) used by tests. Evaluate `mimalloc` for the Rust
side behind a cargo feature `mimalloc` with `mi_collect(true)` on close; keep the default only if it measurably beats trim.

### 4.3 FFI - `openbc-ui-qt/src/ffi/text_compare.rs` + `cpp/viewers/text/text_compare_api.h`

Follow the existing `openbc_*` C ABI conventions (opaque handles, `*_destroy`). Poll, do not call back across threads.
- `openbc_text_session_open(left_path, right_path, options*) -> *Session` ; `..._start`, `..._cancel`, `..._close`
- `..._state(&phase,&done,&total,&degraded_regions)`, `..._row_count() -> u64`, `..._side_info(side) -> {line_count, bytes, encoding, truncated_lines}`
- `..._rows(first, count) -> *RowWindow` with accessors for kind, left/right line (u64), whitespace_only, similarity, text ptr+len
  (UTF-8), inline segments (UTF-8 byte ranges; C++ converts only visible rows), and highlight spans (UTF-16 start/length, fg/bg RGBA, bold, italic).
- `..._next_change(row, dir)`, `..._sections_iter`, `..._row_for_line(side, line)`, `..._minimap_bins(n, *out)`
- `..._find(side_or_both, needle, regex, case, from_row, dir) -> row/col` (streaming over chunks with `memchr::memmem`/`regex`, cancellable)
- `..._copy_range(side, row_a, col_a, row_b, col_b, max_bytes) -> text`
- `..._set_style(style)`, `..._set_language_override(...)`, `..._memory_stats()`, `openbc_trim_memory()`

### 4.4 Qt - `cpp/viewers/text/large_diff_view.{h,cpp}` (+ `large_diff_pane.{h,cpp}` if useful)

A virtualized two-pane view that replaces the two `LineNumberEditor`s **only while in large mode**:
- `QAbstractScrollArea`-based panes sharing one vertical scroll position (rows are fixed height; use `qint64` row math and map
  to the 32-bit scrollbar proportionally when `rows * lineHeight` would overflow). Independent horizontal scrolling per pane.
- Paints only visible rows. Text is drawn with a cached `QTextLayout` per visible line (LRU <= 512) using `QTextLayout::setFormats`.
  Monospace + tab stop = 4 spaces (same as `LineNumberEditor`). Font from `TextFont::instance()`; re-layout when it changes.
- **Styling parity is mandatory.** Reuse the exact colors/rules from the existing small-file path:
  - Row backgrounds (full width), colors from `openbc::ui::appearance::background(...)` with the same defaults:
    `File Views/Different` (150,120,30,150) for paired changed rows; `File Views/Unimportant` (45,90,135,150) for
    whitespace-only rows; `File Views/Left only` / `File Views/Right only` (70,70,70,90) for the side that has the line, and the
    **hatched** brush `QBrush(QColor(0x50,0x50,0x50,130), Qt::BDiagPattern)` on the opposite (filler) side.
  - Current row overlay `QColor(255,255,255,22)`.
  - Inline diff: `Mismatch` -> foreground `#ff4d4d`, bold, no background; `WhitespaceMismatch` -> background
    `palette Highlight`, foreground `HighlightedText`; **plus** the whitespace marker overlay from `LineNumberEditor::paintEvent`
    (`·` for space, `→` for tab, color `#a9d4ff`, font 0.9x) drawn on whitespace-mismatch segments.
  - Syntax foreground/bold/italic from the backend through the same `styledForeground()` mapping for all four `Style`s
    (Vibrant, Classic, HighContrast, VsCodeDark). Factor that function out of `SyntaxHighlighter` into a shared header
    (e.g. `viewers/text/syntax_style.h`) used by both; do not duplicate it. Inline-diff format wins over syntax format on overlapping ranges, as in `highlightBlock`.
  - Gutters: line numbers right-aligned, color `File Views/Line numbers`, blank for filler rows (disabled-text color as today);
    gutter width from the digit count of the largest line number. Copy arrows are not shown (large mode is read-only, as today).
- Interaction: mouse/keyboard selection per pane with Ctrl+C (via `copy_range`, capped, with a status message when capped),
  arrow/PageUp/PageDown/Home/End/Ctrl+Home/Ctrl+End, wheel (sync), Ctrl+G go to line, drag scrollbar.
- Wire the existing shared widgets: `DifferenceOverview` (add a `setBins()`/summary overload; do not build a new control),
  `FindReplaceBar` (find only; replace disabled in large mode), `DiffLinePreview` (feeds from the current row, with inline diff),
  next/previous section actions, "minor differences" toggle (uses the stored `ws_only`), status bar (phase, lines, degraded
  regions, timings), progress bar with cancel. Show All is supported; "Show differences only"/context is a stretch goal
  implemented as a view-level row filter over `RowMap`. Disable the word-wrap action with a tooltip in large mode.
- React to the same changes the editors react to: font, appearance colors, highlight style, extension mapping changes, theme,
  file-changed (reload creates a new session and closes the old one).
- Threshold: use the virtual view for any **local regular file >= 8 MiB on either side** (option `TextCompare/LargeFileThresholdMiB`,
  default 8, documented in `options.md`). Below it keep the current editable `LineNumberEditor` path. Remote/archive-virtual files keep the
  legacy path (fixed as in phase 0) until a spill-to-temp-file adapter exists.

## 5. Cleanup contract (must be explicit and tested)

On tab close, viewer destruction, Reload and file swap, in this order: cancel -> `LargeTextSession::close()` (joins tasks, drops
pool/caches/indexes) -> destroy `LargeDiffView` and layout caches -> `deleteLater` processed -> `openbc_trim_memory()` via a
queued call. Use a **dedicated `QThreadPool`** for compare work (not `globalInstance()`), `waitForDone()` + delete it on close, and
make sure queued lambdas never hold large objects (move only small tokens/ids; check the generation before touching data).
`TextCompareViewer::releaseLargeFileResources()` must call this path and must also be invoked from the viewer destructor.
Provide `cpp/tools/text_memory_test/` (own `main`, offscreen) that: records private bytes, opens a generated 500 MiB pair, waits for
Ready, scrolls through 200 random positions, closes, trims, and asserts the contract in section 1, five times in a row.

## 6. Testing, benchmarks, acceptance

1. **Generator**: `cargo run -p openbc-engine --example gen_large_text -- --size 500MiB --shape log|json|code|minified|random --diff-rate 0.001 --block-moves 20 --seed 1 --out a.txt b.txt`. Include CRLF, no-trailing-newline, UTF-8 BOM, UTF-16, very long lines, invalid UTF-8 variants.
2. **Rust tests**: line index vs naive split (all newline styles/encodings); hashing normalization equals `PreparedNormalizer`; RowMap equals `compare_buffers` for < 2,000 lines (property test); every input line appears exactly once and in order for large synthetic cases; large block insertion (> 10,000 lines) does **not** desynchronize the remainder; deadline path returns Degraded blocks, never hangs; `SourceChanged` on truncate.
3. **Criterion benches**: index, hash, diff, window services; report MiB/s.
4. **Perf/memory gate** (CI job, ignored by default, run manually/nightly): the table in section 1 on 100 MiB / 500 MiB / 1 GiB pairs for each `shape`; fail on regression > 15%. Adversarial cases (500 MiB all-different random, 20% changed log) must still finish inside the targets by degrading alignment.
5. **Styling parity test** (`cpp/tools/text_style_parity/`, `QT_QPA_PLATFORM=offscreen`): render the same 300-line fixture pair (with every kind: equal, modified with inline mismatch, whitespace-only, left-only, right-only, multi-line comment, a long line) once through the legacy `TextCompareViewer` editor path and once through `LargeDiffView` (force with threshold 0), `grab()` both, and compare the text regions (excluding scrollbars) with a small per-pixel tolerance; also compare for all four highlight styles and light/dark themes. A failing parity test blocks the phase.
6. Update `options.md`, `docs/REFACTOR_SAVEPOINT.md`, and add a short `docs/LARGE_TEXT_COMPARE.md` describing the architecture, budgets and how to run the tests/benches.

Definition of done: all targets in section 1 met on the three OSes' CI/manual runs, parity test green, no desync on block insert,
no memory growth over repeated open/close, existing small-file behaviour unchanged (editing, undo, copy arrows, save).

## 7. Phases (one commit each)

**Phase 0 - safe quick wins in the legacy path (keep working while the rest lands)**
- `LineNumberEditor`: add `setUndoRedoEnabled(false)` in `setLargeContentMode(true)`; clear/skip `snap_` in large mode.
- `appendLargeRows`: build one joined `QString` per page and insert with a single `insertText`, `setUpdatesEnabled(false)` around it; page size = 8 MiB (byte-based), **remove the 100 ms timer** (chain next page on completion).
- `alignTextLines`: compute `normalizedTextLine` only when `changed && both numbers > 0`; avoid the `join().toUtf8()` double copy for big inputs.
- `SyntaxHighlighter::refreshBackendSpans`: no `toPlainText()` round trip, no per-span `block.text()`; build one line-start table.
- `releaseLargeFileResources`: after clearing, call a new `openbc_trim_memory()` (implement `mem.rs` now) and use a dedicated `QThreadPool`.
- Add the generator, `text_memory_test`, and record baseline numbers in `docs/LARGE_TEXT_COMPARE.md`.

**Phase 1 - Rust large engine** (4.1 + 4.2) with all Rust tests/benches and a CLI example that prints phase timings and memory.
**Phase 2 - FFI + virtual view** (4.3 + 4.4) behind the threshold option; legacy large mode removed once parity is green.
**Phase 3 - cleanup contract + memory regression test** (section 5), allocator tuning, cross-OS verification.
**Phase 4 - stretch**: syntect checkpoints, progressive first paint using the byte-identical prefix, "show differences only", spill-to-temp adapter for remote/archive inputs.

## 8. Constraints

- No new third-party crates beyond: `memchr`, `xxhash-rust`, `libc`, `windows-sys` (needed features only), optionally `mimalloc` behind a feature, optional small LRU crate (or hand-roll). Justify each in the commit message.
- No `unsafe` outside FFI and `mem.rs`; every `unsafe` block gets a safety comment. No panics across FFI (catch_unwind at the boundary).
- Never read a whole large file into one buffer; never build a per-line `String`/`QString` for the whole file; never store full-file text in Qt widgets in large mode.
- Keep telemetry (`openbc_core::telemetry`) for each phase.
- If something in this prompt conflicts with the code you find, prefer the code's existing conventions and note the deviation in `docs/LARGE_TEXT_COMPARE.md`.