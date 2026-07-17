# Right floats poisoned shrink-to-fit measurement (wikipedia's exploded header)

Punch-list item #3, second slice (the wiki-specific remainder after the
media-eval slice).

## Root cause — found by measurement, not by reading CSS

A new gated `-DFLEXDBG` (kept) dumps every flex row's per-item
hypothetical/min widths. On Wikipedia:

    [flex] node=76 cw=664 wrap=1 class="vector-header mw-header"
           items: 78=h65/m16 203=h99999/m110

`.vector-header-end` measured a preferred width of **99999**. The chain:
shrink-to-fit measure passes (`tbl_measure_cell`, also used for flex
hypothetical sizes) lay content at a provisional width of 100000 and
read back `items_extent`. A **`float:right`** child (`.search-toggle`,
active at this viewport) places at `lx1 - mbw` — against the
provisional width that is x≈100000 — so the extent exploded, the
header-end wrapped onto its own flex line, and the whole header
stacked ~132px tall with everything below pushed down (~50% of the
viewport was header/whitespace).

Alignment-type inflation was already freeze-suppressed (`talign`,
`justify-content`, auto-margin centering); right floats were the one
missed case, and they poison *every* extent-based measurement — flex
bases, table cells, inline-blocks, float shrink-to-fit.

## Fix

One line in `lay_float` placement: during `g_flt_freeze` (measure
passes), right floats place at the **left** edge like left floats.
Intrinsic width is direction-agnostic — a float contributes its width,
not its position. Real layout passes are untouched.

After: `203=h110/m110` — one flex line, header ~150px, H1/tabs/body all
shifted up ~45px, more article visible above the fold.

## Verification

- wikipedia composite: header renders hamburger + Donate/Create
  account/Log in on ONE row like Edge (remaining header gaps are the
  parked mask-icon items and the wordmark image, not layout).
- github / hn / bbc composites: no regressions (hn exercises floats
  heavily).
- Home page renders; ESM self-test 3× ALL PASS.

## Diagnostics kept

`-DFLEXDBG` — per-flex-row container class + per-item hyp/min widths
(real layout passes only, measure passes excluded).
