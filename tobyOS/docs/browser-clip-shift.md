# Provisional-lay clip rects + sibling combinators (wikipedia's icons)

Punch-list closeout, item 2 — the previously-parked masked-icon defect.

## Root cause — found in two probe cycles, and it was NOT the parked theory

The parked analysis blamed sticky-pass culling and measure-pass
survival. A fresh gated `-DMASKDBG` (kept: per-masked-item paint
outcome with geometry, clip index+rect, mask state) showed all four
icons reaching the mask blit with decoded masks — and every one
carrying a clip rect at **y ≈ 1,048,604 = the 1<<20 provisional lay
origin**:

    [maskdbg] ri=4 xy=11,71 wh=19x19 clip=1[1,1048604 63x16777216] mstate=1 paint

`.cdx-button { overflow:hidden; display:inline-flex }`: inline-blocks
(and floats) lay their interior at a provisional origin and then shift
the items into place — but clips created inside were **never shifted**.
The button's own overflow clip stayed a megapixel below the page and
culled its icon at the blit's clip intersection. Every
overflow-inside-inline-block/float was affected, not just masks.

## Fix

- `clips_shift(c0, dx, dy, outer)`: clips created by the provisional
  interior lay shift with the items. The interior is laid with
  `g_cur_clip = 0` (an interior clip must not intersect the outer clip
  while still in provisional coordinates — the result is garbage), and
  each new clip re-intersects the outer clip after the shift, in real
  coordinates. Items that carried no interior clip re-attach the outer
  one. Nested inline-blocks compose (inner clips shift twice, landing
  in real coordinates).
- Applied in `ic_inline_block` and `lay_float`.

## Follow-on: sibling combinators (the "menu/Main" text overlap)

With the clip fixed, the icons painted — plus the buttons'
screen-reader label text, which Chrome hides via
`.cdx-button--icon-only span + span { position:absolute;
width:1px; overflow:hidden; clip:rect(1px,1px,1px,1px) }` — an
**adjacent-sibling** selector we used to drop (`+`/`~` invalidated the
whole selector). The sr-only pattern is ubiquitous, so:

- `+` (adjacent) and `~` (general) combinators now parse (comb 3/4)
  and match via a previous-element-sibling walk in `match_upward`.
- Ancestor-bloom safety: parts joined through a sibling combinator are
  NOT ancestors; right-to-left, once a sibling link appears the bloom
  stops demanding bits (stays false-positive-only). `-DCSS_VERIFY`
  proves the match set.

## Verification

- wikipedia header: hamburger / search / language / tools icons render
  as real glyphs; sr-only label text no longer overlaps.
- `-DCSS_VERIFY` 0 mismatches; wiki/hn/github composites; home; ESM 3×.
