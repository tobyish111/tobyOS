# Browser stage 12C: flexbox v1

Branch `browser-flexbox` (stage-12 scope item C, stacked on
`browser-tables`). The roadmap's "one modern primitive that unlocks
the most real sites": before this, `display:flex` degraded to plain
block stacking, so every modern nav bar / card row rendered vertically.

## What landed (programs/user_gui_browser/main.c)

- `display:flex` → new `D_FLEX`; `inline-flex` still degrades to
  inline-block. New cstyle fields: `fdir`, `fwrap`, `fjust`, `falign`,
  `fgrow`, `fshrink` (default 1), `gap`, `fbasis` (px, -1 auto).
- Properties: `flex-direction` (row/column; -reverse maps to the
  plain direction), `flex-wrap`, `flex-flow`, `justify-content`
  (start/end/center/space-between; around/evenly ≈ between),
  `align-items` (stretch/start/center/end; baseline ≈ start),
  `flex-grow`, `flex-shrink`, `flex-basis` (auto/px lengths), the
  `flex` shorthand (none / auto / N [M] [len] — `flex:N` sets basis 0
  per spec), `gap` (+ column-gap/row-gap treated as gap).
- `lay_flex` (row axis): items are element children (bare text
  skipped); hypothetical main size = basis+padding+borders, else %
  width of the container, else the preferred measure from the frozen
  provisional-layout pass (same machinery as table cells — explicit px
  widths short-circuit); min floor = min-content measure. Greedy line
  fill under `flex-wrap`; positive free space distributes by integer
  grow factors; negative shrinks proportionally to shrink×hyp floored
  at min-content. `justify-content` shifts the line (or spreads the
  gaps for space-between); items lay via `lay_block` with the resolved
  width imposed through a save/restore of the computed style;
  `align-items` center/end shift each item's display-items down within
  the line (stretch lays top-aligned, height-stretch not implemented).
  Lines advance by max item height + gap.
- Column axis: block stack with `gap`; `align-items` start/center/end
  shift narrower items horizontally.
- Measure-pass discipline: grow and justify offsets are suppressed
  under `g_flt_freeze` so a flex container measures as the sum of its
  items (same rule as the table/text-align fix).

## v1 limits
row-reverse/column-reverse order, `order:`, `align-self`,
`align-content`, auto-margin free-space absorption, %-basis, and
height-stretch for `align-items: stretch` are not implemented; flex
containers with meaningful bare-text children skip that text.

## Verified in QEMU (screenshots in the session scratchpad)
Local /flex page — 11 cases matching Chrome's layout:
plain row, justify center / flex-end / space-between, flex:1 equal
thirds, 2/1/1 grow split, fixed 120px basis + grower, align-items
center against a tall item, flex-wrap with gap (wraps 3+2 at this
width, item outer = basis+padding exactly as Chrome), column with gap
+ center, and the real-world nav-bar pattern (brand | centered links |
button via space-between + align-items center).
Real internet: mojeek.com's flex header lays out horizontally
(logo/search/nav row). Regressions: tables page, floats page, CSS
torture page all unchanged.
