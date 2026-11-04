# Large text compare refactor savepoint

## Status

The large-file replacement is not complete. The viewer still uses its legacy page loader and
`QTextDocument`-backed panes. Existing `openbc-core/src/text/large/` files are not wired into that
viewer path and are not a bounded-memory implementation.

## Completion notes

S0 is blocked: a deterministic large-text pair generator, opt-in Rust allocation counters, and an
offscreen legacy-path measurement tool have been added. Only one 100 MiB Linux cycle has been
measured; the 500 MiB baseline, five-cycle baseline, and Windows run remain outstanding. Do not start
S1 until the S0 gate is complete. See [`LARGE_TEXT_COMPARE.md`](./LARGE_TEXT_COMPARE.md) for results
and the exact Windows commands.

A targeted follow-up fix now initializes the large-file minimap at the start of page loading and
appends compact row metadata as each diff page arrives. This avoids regrouping the full accumulated
row list after every page. Offscreen Qt tests verify distinct matched-diff, unmatched-line, and
whitespace-diff markers and confirm the actual autosync viewer map grows from 518 to 1,030 rows
before the full comparison completes. This narrow fix does not complete S0 or claim the virtualized
replacement slice is unblocked.

## Phase plan

1. S0 — Measure the legacy path; do not start replacement slices until the baseline gate is complete.
2. S1 — Walking skeleton with a virtualized view; start only after S0 baseline results are recorded.
3. S2–S6 — Progressive alignment, global refine, highlight quality, cleanup, interaction, and docs;
   each slice is gated by measured results before proceeding.
