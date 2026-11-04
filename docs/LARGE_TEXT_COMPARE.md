# Large text compare

## Status

The large-file replacement is **not implemented yet**. The viewer still routes large local files
through the legacy `QPlainTextEdit`/`QTextDocument` page loader. The existing
`openbc-core/src/text/large/` code is scaffolding and is not part of the viewer data path.

Work is gated slice-by-slice. S0 adds repeatable data generation and an offscreen legacy-path
measurement tool; no replacement behavior starts until the baseline results below are recorded.

## S0 measurement setup

Generate a deterministic pair (the requested size is the upper bound per file; output includes a
BOM, mixed CRLF/LF/lone-CR endings, an invalid UTF-8 sequence for UTF-8 output, one 70 KiB line,
and no trailing newline):

```sh
cargo run -p openbc-engine --example gen_large_text -- \
  --size 100MiB --shape log --diff-rate 0.001 --block-moves 20 --seed 1 \
  --out /tmp/left.txt /tmp/right.txt
```

Available shapes are `log`, `json`, `code`, `minified`, and `random`. `--encoding utf16le` and
`--encoding utf16be` produce the corresponding BOM and code units; the default is UTF-8. The
generator prints the selected shape and parameters; keep that output with the measurement log.

Configure and build the opt-in measurement executable. The `mem-stats` feature is enabled only for
this build; it counts Rust allocator bytes separately from process private memory.

```sh
cmake -S . -B build/lin -DOPENBC_BUILD_TEXT_MEMORY_TEST=ON
cmake --build build/lin --target text_memory_test
QT_QPA_PLATFORM=offscreen ./build/lin/text_memory_test /tmp/left.txt /tmp/right.txt 5
```

The tool reports first paint, completion, peak and steady private bytes, Rust live/peak allocations,
private bytes after close and trim, per-cycle private-byte delta, visible syntax-format spans,
diff-background selections, and saturated screenshot-pixel counts at 5 and 30 seconds. It prints
progress and private bytes every five seconds. On Windows, the private-memory metric is `PROCESS_MEMORY_COUNTERS_EX::PrivateUsage`;
on macOS it is `phys_footprint`; on Linux it is the sum of `Private_Clean`, `Private_Dirty`, and
`Private_Hugetlb` from `/proc/self/smaps_rollup`. RSS is not used.

## S0 gate — legacy baseline

Machine/OS and shape are recorded with every run. The first 100 MiB run completed once using the
initial harness revision; visual pixel counts were recorded, but syntax and diff formats were not
yet distinguished.

| Metric | Target | Measured | Machine / OS | File shapes |
|---|---|---|---|---|
| 100 MiB pair: first paint | Record baseline | 56 ms (one cycle) | Host `victus`, AMD Ryzen 5 5600H, Ubuntu Linux 7.0.0-42 x86_64, Qt 6.10.2 | `log`, seed 1, 0.1% changed, 20 block moves |
| 100 MiB pair: fully loaded | Record baseline | 802,276 ms (one cycle) | Host `victus`, AMD Ryzen 5 5600H, Ubuntu Linux 7.0.0-42 x86_64, Qt 6.10.2 | `log`, seed 1, 0.1% changed, 20 block moves |
| 100 MiB pair: peak / steady private bytes | Record baseline | Sampled peak 9,655,349,248 B; post-scroll steady 9,712,898,048 B, so actual peak was at least 9,712,898,048 B. Initial harness did not sample during scrolling. Rust live after close: 4,209,376 B; Rust peak: 4,981,483 B. | Host `victus`, AMD Ryzen 5 5600H, Ubuntu Linux 7.0.0-42 x86_64, Qt 6.10.2 | `log`, seed 1, 0.1% changed, 20 block moves |
| 100 MiB pair: after-close private bytes, 5 cycles | Record baseline | 48,345,088 B after one cycle, versus 24,035,328 B before open (+24,309,760 B); 5-cycle result pending. | Host `victus`, AMD Ryzen 5 5600H, Ubuntu Linux 7.0.0-42 x86_64, Qt 6.10.2 | `log`, seed 1, 0.1% changed, 20 block moves |
| 100 MiB pair: syntax / diff colors at 5 s and 30 s | Record baseline | 648,502 / 639,241 saturated screenshot pixels; this run did not separately record syntax spans or diff backgrounds. | Host `victus`, AMD Ryzen 5 5600H, Ubuntu Linux 7.0.0-42 x86_64, Qt 6.10.2 | `log`, seed 1, 0.1% changed, 20 block moves |
| 500 MiB pair: first paint | Record baseline | Not run | Host `victus`, AMD Ryzen 5 5600H, Ubuntu Linux 7.0.0-42 x86_64; not attempted | `log`, seed 1, 0.1% changed, 20 block moves |
| 500 MiB pair: fully loaded | Record baseline | Not run | Host `victus`, AMD Ryzen 5 5600H, Ubuntu Linux 7.0.0-42 x86_64; not attempted | `log`, seed 1, 0.1% changed, 20 block moves |
| 500 MiB pair: peak / steady private bytes | Record baseline | Not run | Host `victus`, AMD Ryzen 5 5600H, Ubuntu Linux 7.0.0-42 x86_64; not attempted | `log`, seed 1, 0.1% changed, 20 block moves |
| 500 MiB pair: after-close private bytes, 5 cycles | Record baseline | Not run | Host `victus`, AMD Ryzen 5 5600H, Ubuntu Linux 7.0.0-42 x86_64; not attempted | `log`, seed 1, 0.1% changed, 20 block moves |
| 500 MiB pair: syntax / diff colors at 5 s and 30 s | Record baseline | Not run | Host `victus`, AMD Ryzen 5 5600H, Ubuntu Linux 7.0.0-42 x86_64; not attempted | `log`, seed 1, 0.1% changed, 20 block moves |

**S0 is blocked.** One 100 MiB/side pair took 802 s to load and reached at least 9.71 GB private
bytes after scrolling; no Windows host is available here, and the 500 MiB or five-cycle baseline was
not attempted because the measured 100 MiB run already consumed substantial private memory and took
over 13 minutes. The available evidence supports the memory and load-time diagnosis but does not
complete the required S0 baseline. Do not start S1 until the remaining baseline is measured.

### Requested autosync-log backend diagnostic

Run the opt-in Rust backend test with:

```sh
cargo test -p openbc-engine --features mem-stats --test large_text_progress -- \
  --ignored --nocapture
```

The test intentionally fails its progressive-output assertion on the current backend. Its metrics
are printed before that assertion. This exercises `AsyncTextCompareTask` and core highlighting; it
does not instantiate Qt and cannot verify minimap painting or Qt syntax/diff styling. Allocator
figures are Rust global-allocator bytes, not process private bytes.

| Metric | Target | Measured | Machine / OS | File shapes |
|---|---|---|---|---|
| First diff chunk versus completion of both input reads | Emit before both complete reads | Failed: both reads completed at 37,920 us; first chunk at 2,654,894 us | Host `victus`, AMD Ryzen 5 5600H, Ubuntu Linux 7.0.0-42 x86_64 | `autosync.left.log` 40,473,183 B; `autosync.right.log` 40,488,552 B |
| Backend diff rows | Record row classifications | 1,152,620 total: 1,152,090 unchanged, 524 added, 6 deleted, 0 modified; 4 changed groups | Host `victus`, AMD Ryzen 5 5600H, Ubuntu Linux 7.0.0-42 x86_64 | Same autosync log pair |
| Syntax spans for `.log` rows | Record whether spans are available | 0 spans over the first 256 diff rows; `.log` routes to plain text | Host `victus`, AMD Ryzen 5 5600H, Ubuntu Linux 7.0.0-42 x86_64 | Same autosync log pair |
| Rust allocator peak | Record; no backend memory budget is set | 635,427,895 B process peak; 90,264 B live after comparison | Host `victus`, AMD Ryzen 5 5600H, Ubuntu Linux 7.0.0-42 x86_64 | Same autosync log pair |
| Qt minimap and style painting | Verify progressive display | Not observable from this Rust backend test; the viewer/minimap still require a Qt harness | Not measured | Same autosync log pair |

This confirms the existing engine compare path is not progressive: it reads both files, builds the
complete diff, and only then emits chunks. It also materializes over 1.15 million diff rows and
recorded a 635 MB Rust allocator peak for this 80 MB pair; the test consumes each emitted chunk
without retaining all rows. The zero syntax spans reflect the `.log` plain-text language route, not
a Qt rendering result. The minimap's progressive UI behavior is verified separately below.

### Progressive minimap fix

The large-file viewer now clears the overview when the first page starts and appends compact
per-row metadata to `DifferenceOverview` as each aligned page is appended to the editors. The
overview incrementally extends contiguous diff blocks rather than regrouping all rows on every
page. Its drawing state retains only changed/whitespace flags and whether each side has a line;
it does not duplicate large row text. The existing final `setRows(rows_)` rebuild at load completion
has been removed for this path.

Run the offscreen Qt marker test with:

```sh
QT_QPA_PLATFORM=offscreen ./build/lin/text_memory_test --minimap-progress-test
```

This tests the overview with synthetic aligned rows in two appends. Result on host `victus`, Ubuntu
Linux 7.0.0-42, Qt 6.10.2: passed. The first page painted 160 matched-diff pixels and 126
unmatched-line pixels; after a second page it painted 64 whitespace-diff pixels.

The actual large-file viewer path can be checked against the requested fixtures with:

```sh
QT_QPA_PLATFORM=offscreen ./build/lin/text_memory_test \
  --large-file-minimap-test test/autosync.left.log test/autosync.right.log
```

Result on the same host: passed in 313 ms. Unmatched markers first appeared after 518 aligned rows,
then the overview grew to 1,030 rows while the full comparison was still loading. It painted 192
unmatched-marker pixels at that first visible diff page. Together, the two Qt checks verify that the
viewer sends new page rows to the minimap before load completion and that matched-diff, unmatched,
and whitespace-diff markers render distinctly. Neither run measures full-load latency or private
memory; S0 remains blocked as described above.

On the Windows Git Bash build host, generate and run the missing five-cycle baseline with:

```sh
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:/c/msys64/opt/wix:$PATH"
export OPENSSL_DIR=/c/msys64/ucrt64
mkdir -p /c/tmp
cargo run -p openbc-engine --example gen_large_text -- --size 500MiB --shape log \
  --diff-rate 0.001 --block-moves 20 --seed 1 --out /c/tmp/left.txt /c/tmp/right.txt
cmake -S . -B build/win -G Ninja -DOPENBC_BUILD_TEXT_MEMORY_TEST=ON
cmake --build build/win --target text_memory_test
QT_QPA_PLATFORM=offscreen ./build/win/text_memory_test.exe /c/tmp/left.txt /c/tmp/right.txt 5
```

## Later slice gates

The S1–S6 acceptance targets and designs are maintained in the task specification. Do not start a
later slice before the current slice's measured gate is written above or in its corresponding gate
table.
