# CSS animations in the browser (arc)

Branch `browser-css-anim`, off `main`. Adds CSS animation to the engine —
the last of the "feels modern" visual gaps. This arc is sliced:
1. **`@keyframes` animations** (this slice) — driving the *cheap*
   properties that work in the immediate-mode painter.
2. CSS transitions (change-detection on restyle).
3. `transform: scale`/`rotate` (needs compositing-layer work).

## Slice 1 — `@keyframes` animations (DONE)
An element with `animation:` runs autonomously on an animation clock; no
change-detection needed. Supported animatable properties (chosen because
they don't need new paint machinery): `transform: translate` (px), `color`,
`background-color`.

### How it works
- **`@keyframes` registry** (`g_kf[]`, parsed in `css_parse_sheet`): each
  named set holds up to 8 stops; each stop carries only the animated subset
  (translate px, color, bg). `from`/`to`/`N%` selectors, comma-shared
  stops, stops sorted by percent. Reset per page in `page_reset`.
- **`animation` shorthand + longhands** (`CP_ANIMATION`, `animation-name`/
  `-duration`/`-iteration-count`/`-delay`/`-direction`/`-timing-function`)
  parse into non-inherited `cstyle` fields (`anim_kf`/`anim_dur`/
  `anim_delay`/`anim_iter`/`anim_flags`). `<time>` accepts `s`/`ms`;
  `infinite` → 0 iterations; `alternate` and `ease*` set flags.
- **The clock** (`anim_apply`, run right after the style pass and before
  layout in both `render_html` and `js_rerender`): for each animating
  node, compute elapsed → cycle/fraction (with delay, iteration count,
  `alternate` reversal, and an integer smoothstep for `ease-in-out`),
  find the surrounding keyframe stops, interpolate, and overwrite the
  node's computed `transform`/`color`/`bg` **before layout bakes them**.
  Sets `g_anim_active` while anything is still running. Per-node start
  times live in `g_anim_t0[]`.
- **The pump** (`anim_pump`, in the main loop next to `media_pump`):
  while `g_anim_active`, a full restyle→relayout→repaint per frame
  (~30 fps), the same reflow path JS uses — necessary because
  transform/color are baked at layout, not read at paint.

### Verified
Built with the browser; a host page with
`animation: slide 2s infinite alternate ease-in-out` (translateX 0→210px)
and `animation: huepulse 3s infinite` (background-color through three
color stops) fetched over SLIRP. Screenshots ~3 s apart show the box at
different x positions and the pulse box at different interpolated colors,
with the GUI monitor counting frames — the clock drives continuous reflow.

### v1 limits
Translate/color/background only; scale/rotate and `opacity` need the
compositing work in slice 3. `animationend`/`transitionend` aren't fired
yet (the events are registered in the prelude; wiring the dispatch is a
follow-up). Per-frame full reflow is fine for a few animated elements but
not free — a display-list-only fast path is a later optimization.

## Next slices
- **Slice 2** — CSS transitions: snapshot computed values on restyle, diff
  the transitioned properties, interpolate on change; fire `transitionend`.
- **Slice 3** — `transform: scale`/`rotate` (+ `opacity`): per-element
  compositing so a transformed subtree renders to an offscreen buffer that
  is transform-blitted. The hardest part; the painter is immediate-mode
  today.
