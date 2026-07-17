# Media-query evaluation: units, calc(), level-4 range syntax

Punch-list item #3 (columns/headers stack, content below the fold), first
slice. The old evaluator parsed feature values with a bare-integer scan
and scanned to the *first* `)` — so real mobile-first sheets failed in
both directions:

- `@media (width < calc(1rem * 2 + 15rem + 2rem + 31rem))` (MDN's
  *mobile* layout gate, = width < 800px): the first `)` belongs to
  calc(), the feature name never matched, the query evaluated FALSE —
  so at a 719px viewport we applied MDN's **desktop** two-column grid
  (which we then stack vertically) instead of the single-column mobile
  layout Edge shows. The article body ended up below the fold; only the
  sidebar was visible.
- `@media (max-width: calc(640px - 1px))` (wikipedia's mobile rules):
  integer scan read px=0 → always false. (Not visible at our 708px
  width, but wrong at narrow ones.)
- em/rem lengths (`min-width: 64em`) would have read as 64px and
  matched everything.

## Fix

`media_matches` now captures each feature to its **matching** paren and
evaluates through:

- `med_len_px`: full length expression — float values, units
  (px default, em/rem = 16px in media-query context, vw/vh), and
  `calc()` with `+ - * /`, nested parens, two-level precedence.
  Unparseable → feature fails (still fail-closed).
- `media_feature`: `name: value` form (min/max-width, width, new
  min/max-height, height, orientation, prefers-color-scheme) plus
  **level-4 range comparisons** with width/height on either side
  (`width < L`, `L <= width`, …). Double ranges (`A < width < B`)
  remain unsupported (fail closed).

## Verification

- mdn composite: article content appears (single-column mobile layout,
  like Edge at 719px); grid-diff drops further from 17.5.
- wikipedia + github composites: no regression (their gates are
  integer-px min-widths that already evaluated correctly at this
  viewport).
- -DCSS_VERIFY 0 mismatches; home page; ESM 3×.
