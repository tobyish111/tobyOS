# Browser stage 13C: CSS Grid v1

Branch `browser-grid` (stacked on `browser-calc`). CSS Grid is the
dominant layout system for modern sites and the single biggest layout
gap — before this, `display:grid` degraded to a block stack, breaking
the structure of a large fraction of the web.

## What landed (programs/user_gui_browser/main.c)

- **Types**: `display:grid`/`inline-grid` → new `D_GRID`. `cstyle`
  gained `gtc_off`/`gtc_len` and `gtr_off`/`gtr_len` (grid-template
  column/row raw text in a stable `g_gridpool`, since the templates are
  variable-length and must survive the per-node csspool rollback), plus
  per-item `gcol_span`/`grow_span`.
- **Properties**: `grid-template-columns`, `grid-template-rows`,
  `grid-column`/`grid-row` (v1 reads the span: `span N`, or `A / B` →
  `B-A`), and `grid-gap`/`grid-column-gap`/`grid-row-gap` (→ the
  existing `gap`).
- **Track resolution** (`grid_resolve_cols`): parses a column template
  into pixel widths. Supports fixed px, `%`, `fr`, `auto` (≈ 1fr in
  v1), `repeat(N, <track>)`, and `repeat(auto-fill|auto-fit,
  minmax(min, max))` — for auto-repeat it computes the column count
  that fits `(W + gap) / (min + gap)` and fills that many `1fr` tracks
  (the dominant responsive-card pattern). Fixed px/% tracks claim their
  size first; `fr` tracks split the remainder; gaps subtracted.
  `minmax(a, b)` collapses to its max track.
- **`lay_grid`**: auto-places items row-major honoring `gcol_span`
  (wrapping to the next row when a span doesn't fit), lays each item
  via `lay_block` at its cell x with the spanned width imposed
  (computed-style save/restore, same technique as flex/table cells),
  measures each row's height as its tallest cell (rows are
  content-sized), then shifts each item's display items down to its
  row's y. Composes with 13A/13B — a template may use `var()`/`calc()`
  because those resolve on the value string before this runs.
- Grid is a new dispatch arm in `lay_block` (`else if D_GRID`), so
  block/inline/float/table/flex are structurally untouched.

## Verified in QEMU (screenshot in the session scratchpad)
The `/grid` page, all four dominant patterns correct:
1. `repeat(3, 1fr)` — three equal columns, six cells flowing into two
   rows.
2. `grid-template-columns: 200px 1fr` — a fixed 200px sidebar and a
   fluid main column filling the rest of the row.
3. `repeat(auto-fill, minmax(160px, 1fr))` — six responsive cards, the
   column count computed from the container width.
4. `grid-column: span 2` — the red cell spans exactly two of four
   columns, with auto-placement flowing the rest around it into two
   rows.

Flex and table regressions unaffected (grid is a separate dispatch
arm).

## v1 limits
- Named grid lines and `grid-template-areas` (a common but separable
  addition).
- Explicit line placement *start* numbers (`grid-column: 2 / 4`
  positions at line 2); v1 uses the *span* count and auto-placement for
  the column, not the explicit start line.
- `fr` in `grid-template-rows` (rows are always content-sized in v1).
- `auto` tracks are treated as `1fr` rather than content-sized (so
  `auto 1fr` splits evenly instead of sizing the auto track to its
  content); `dense` packing; `justify/align-items/content` beyond the
  default stretch.
- Floats/absolutely-positioned descendants inside a grid item are laid
  in the item's provisional position and not shifted with it (grid
  items rarely contain them).
