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

## Slice 2 — CSS transitions (IMPLEMENTED, verification BLOCKED)
The engine side is written and compiles: `transition` shorthand +
longhands parse into non-inherited `cstyle` fields (`trans_mask`/
`trans_dur`/`trans_delay`/`trans_flags`); a per-node `g_trans[]` runtime
holds from/to/current per transitionable property (color, background-color,
transform: translate); `trans_apply` runs after `anim_apply` and before
layout, detects when a freshly-cascaded value differs from its target, and
eases from the current displayed value to the new one (shared `css_ease`
smoothstep), overriding `st` and keeping `g_anim_active` set while running.

**Could not verify on screen.** The mechanism is a straight extension of
the working `@keyframes` path, but the headless test can't demonstrate it:
- A page's `setInterval` never fires while the page is otherwise idle
  (the JS callback that would change a transitioned value doesn't run).
- Triggering the change via a **keydown** (which the SPA proves dispatches)
  *does* run the handler and change the style — but the render loop then
  **freezes** (frame counter stuck, 0 fps): the transition starts but
  never advances on screen.
- Underlying both: the browser process is heavily **starved** in the
  headless harness — `@keyframes` (slice 1) visibly ran but at ~15% real
  time (a `2s` slide covered ~1/4 of its range in 3 s).

This is an event-loop / scheduling interaction, not obviously a bug in the
transition interpolation itself, and needs focused follow-up (serial
instrumentation in the render loop; and understanding why a JS-triggered
`g_anim_active` doesn't sustain the pump the way an initial-render one
does). Until then slice 2 is **unverified** and should not be treated as
working.

## Next slices
- **Slice 3** — `transform: scale`/`rotate` (+ `opacity`): per-element
  compositing so a transformed subtree renders to an offscreen buffer that
  is transform-blitted. The hardest part; the painter is immediate-mode
  today.
