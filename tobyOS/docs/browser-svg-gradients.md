# SVG gradient paint servers + the transparent-rounded-rect black boxes

Two black-box defects that looked like one. Both fixed here.

## Defect A: SVG logos rendered as black blobs

**Symptom:** gradient-filled logos (Wikipedia's enwiki-25.svg puzzle globe:
38 gradients, 155 stops, 38 `fill="url(#…)"` refs) decoded to ~69%-opaque
**black** silhouettes.

**Root causes:**
1. `svg_render` had no paint servers: `fill="url(#id)"` failed
   `css_color_tok` and fell through to the inherited default black.
2. The path-data buffer was a fixed 8 KiB; the logo's longest
   `<path d="…">` is 19,230 chars → truncated mid-path.
3. `SVG_MAX_PTS` (1536) clipped the flattened outlines of paths that size.

**Fix (deliberately the cheap tier):**
- Pre-pass `svg_collect_gradients()` over the whole document: collect
  `<linearGradient>/<radialGradient>` ids, average their `<stop>` colors
  weighted by `stop-opacity` (attribute or `style="stop-color:…"` forms);
  `href`/`xlink:href` stop-inheritance chains resolve post-pass (≤4 hops).
- `svg_url_fill()`: `fill="url(#id)"` / `style="fill:url(#id)"` paint the
  average as a solid. Unresolved refs honor the spec'd inline fallback
  (`fill="url(#x) #abc"`), else paint **nothing** — per SVG a broken
  paint-server ref is not rendered (the old fallthrough was the black).
- `dbuf` sized from the document (min 8 KiB, cap 256 KiB); `SVG_MAX_PTS`
  1536 → 8192, `SVG_MAX_SUBS` 48 → 256 (~70 KiB transient, heap).

**Non-goal of this tier:** per-pixel gradient evaluation
(`gradientTransform`/`gradientUnits`/radial falloff/stop positions).
Average-stop solid fills give the right shapes and hues.

**Measured:** `-DIMG_PROF` (re-added, kept) — enwiki-25.svg opaque-pixel
average went from black to `avg=167,177,187` (the globe's real silvery
gray; host-side python cross-check of the stop averages agrees).

## Defect B: the header "black boxes" were never SVGs

After the compound-selector fix removed the salmon backgrounds, the same
four header buttons went **black**. A new gated `-DDL_TRACE` (kept: dumps
header-region display items with node classes) identified them as the
buttons' own `DI_RECT`s with **fg=0x00000000 — transparent backgrounds**.

**Root cause:** a rect item is (correctly) emitted for an element with
`border-radius`/decoration even when its background is transparent (the
item may carry a gradient or shadow). But `paint_deco_rect` forced
`0xFF000000 | color` — painting a *transparent* rounded rect as **opaque
black**. Codex buttons are exactly `background: transparent` +
`border-radius: 2px`; while they wrongly painted salmon the bug was
invisible, and every rounded-transparent element (quiet buttons, pills)
hit it.

**Fix:** honor the solid's alpha in `paint_deco_rect` (`st->bg` is
opaque-or-zero by construction); skip the fill entirely when there is no
gradient and alpha is 0. The blend row already multiplies coverage by
alpha, so gradient/`r4` cases are unchanged.

## Verification

- Local test page (real enwiki-25.svg + wordmark over HTTP): globe renders
  as the recognizable silver puzzle ball at 100px and 50px; wordmark
  unchanged.
- Wikipedia header: black boxes gone (quiet buttons; icons themselves are
  the separate mask-image gap — spans are zero-sized, so empty space, not
  boxes). The `mw-logo` icon is `display:none` at our 708px viewport
  (mobile-first layout), same as Chrome at that width.
- Regressions: data:-URI SVG test, home page (rounded/gradient hero
  unchanged — opaque backgrounds unaffected), ESM self-test 3×.

## Gated diagnostics kept

`-DIMG_PROF` — per-image decode outcome, dims, bytes, opaque %, average
opaque color, gradient count, src. `-DDL_TRACE` — header-region display
items with geometry, color, and source-node class.
