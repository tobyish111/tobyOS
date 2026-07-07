# Browser stage 13D: overflow clipping + transform + position-%

Branch `browser-overflow` (stacked on `browser-grid`). Two rendering
primitives that real pages assume everywhere: clipping content to a
box, and `transform: translate` (including the ubiquitous
`translate(-50%,-50%)` centering pattern).

## What landed (programs/user_gui_browser/main.c)

### overflow clipping
- `overflow`/`overflow-x`/`overflow-y`: `hidden`/`scroll`/`auto`/`clip`
  → clip; `visible` → no clip. (Interactive per-element scrolling is
  not implemented; scroll/auto clip like hidden for now.)
- A clip-rect table (`g_clips`) + a 1-byte `clip` index on every
  display item (reused the struct's pad byte). When `lay_block` enters
  an overflow element it pushes a clip = the element's padding box
  (intersected with any ancestor clip) as the current clip;
  `item_new` stamps it on every emitted item; the clip pops when the
  subtree is done. The painter translates each item's clip to screen
  coords and intersects the draw with it — precise rectangle clipping
  for `DI_RECT`, and cull-if-fully-outside for text/image/field/bullet.
- An overflow-clip box with an explicit `height` is now sized to
  *exactly* that height (content past it clipped), instead of growing
  to fit — the fix that makes vertical clipping real.

### transform: translate
- `transform: translate(x, y)`, `translateX`, `translateY` — px or `%`
  of the element's own border-box size. Applied after layout as a
  paint-only shift of the element's display items (like `position:
  relative`, the flow slot is unchanged). scale/rotate/matrix are
  parsed-and-ignored (no wrong result).

### position offsets in %
- `top`/`right`/`bottom`/`left` now accept `%` (a `po_pct` bitmask):
  L/R resolve against the containing block width, T/B against its
  height. The positioned containing block now tracks its height
  (`g_cb_h`, known when the ancestor has an explicit height; auto
  falls back to 0 for `%` top/bottom). `bottom` on a non-fixed
  absolute box now resolves against a known-height CB too.
- This is what makes `position:absolute; top:50%; left:50%;
  transform: translate(-50%,-50%)` — the single most common centering
  idiom — actually center.

## Verified in QEMU (screenshot in the session scratchpad)
The `/overflow` page:
1. `overflow:hidden; height:60px` — a 5-line block clipped to 60px
   (box sized exactly to 60px; content beyond hidden).
2. `overflow:hidden` crop — a 400×200 box clipped to a 120×60 window
   (only the top-left corner shows; both axes clip).
3. `position:absolute; top:50%; left:50%; transform:translate(-50%,
   -50%)` — a box correctly centered in its relative container.
4. `transform: translateY(20px)` — a box nudged down 20px.

Grid and CSS regressions unaffected (overflow/transform/position-% are
opt-in; grid cells don't set them, and the height-force + clip push are
gated on `overflow`).

## v1 limits
- No partial-glyph clipping: a text line straddling the clip edge draws
  whole (culled only when fully outside), so a boundary line can peek a
  few px past a clip. Rectangles and the crop case clip precisely.
- No interactive per-element scrollbars (`overflow:scroll` clips but
  doesn't scroll independently yet).
- Images that straddle a clip draw whole (culled only when fully
  outside).
- transform: only translate; no scale/rotate/skew/matrix, no transform
  on the containing-block chain for descendants.
