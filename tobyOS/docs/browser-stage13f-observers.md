# Browser stage 13F — observers + getComputedStyle

Branch `browser-observers`, stacked on `browser-webfonts` (13E). Adds the
JS APIs that gate a whole class of modern pages: lazy images, infinite
scroll, responsive-JS, and anything that measures the laid DOM. Nothing
in the transport or CSS engine changed — this is the JS/DOM-geometry
layer built on top of the QuickJS integration (phases 9/10, stage 12D).

## What shipped
- **`getComputedStyle(el)`** → an object of used/computed values keyed by
  kebab-case CSS name, with camelCase aliases and `getPropertyValue()`.
  Covers `color`, `background-color`, `font-size`, `font-weight`,
  `text-align`, `display`, `position`, `width`, `height`, and the four
  `margin-*` / `padding-*`. Widths/heights are the **used** content-box
  values pulled from the laid border box when a layout is fresh, else the
  specified value.
- **`getBoundingClientRect()`** + `getClientRects()`, and the geometry
  props `offsetWidth/Height/Left/Top`, `offsetParent`, `clientWidth/
  Height`, `scrollWidth/Height`, `scrollTop/Left`. Rects are viewport
  coords (doc coords minus scroll; fixed elements ignore scroll).
- **`window.scrollY` / `pageYOffset` / `innerHeight` / `innerWidth`**
  (live getters over the viewport), plus no-op `scrollTo`/`scrollBy`.
- **`MutationObserver`** — `observe(el, {childList, attributes,
  characterData, subtree})`, `disconnect`, `takeRecords`. Delivered from
  the pump as a batch; records carry `{type, target}` (childList /
  attributes / characterData). subtree matching walks parents.
- **`IntersectionObserver`** — `observe`/`unobserve`/`disconnect`. Fires
  when a target's box enters or leaves the viewport band, with
  `isIntersecting`, `intersectionRatio`, `boundingClientRect`,
  `intersectionRect`, `rootBounds`. The initial `observe()` delivers the
  current state (kicked via `__dom.obsKick`), and scrolling re-checks.
- **`ResizeObserver`** — `observe`/`unobserve`/`disconnect`. Fires when a
  target's laid size changes across relayouts, with `contentRect` and
  `border/contentBoxSize`.

## How it works
The load-bearing new primitive is the **laid-rect journal** (13F).
getComputedStyle/rects/Intersection/Resize all need each element's final
box, but the display list is flat items, not per-element boxes. So:

- `struct dnode` gained `lx/ly/lw/lh` + `lgen` (valid only when `lgen ==
  eng->lay_gen`, bumped per `layout()`).
- Blocks stamp their border box into a journal (`g_nst[]`) as they lay
  (`nstamp_add` in `lay_block`, plus table cells). Every post-lay item
  shift (floats, cell valign, flex align, grid row placement, relative,
  transform, absolute placement) mirrors onto the journal range it moves
  (`nstamp_shift`), so the stamped coords track the painted items.
- `layout()` commits the journal into the nodes at the end (last write
  wins, so provisional/measure passes are harmless — they're gated off by
  `g_flt_freeze` anyway), then inline elements take a union of their
  painted items' rects.

The observer machinery lives in the JS prelude. A single C-registered
`obsCheck()` callback runs from the pump when the DOM mutated
(MutationObserver), the layout changed or the page scrolled
(Intersection/Resize), or a fresh `observe()` kicked it — the pump
compares `obs_gen`/`obs_scroll` to the live `lay_gen`/`scroll_y`. C
exposes: `computed(node)`, `rect(node)` (→ `[x,y,w,h,fixed]` or null),
`viewport()` (→ `[scrollY, height, width]`), `takeMutations()` (drains
the per-tab mutation log filled by the DOM primitives), `setObsCheck`,
`obsKick`.

**Pipeline reorder (the getComputedStyle fix):** `render_html` now runs
the initial style + `layout()` **before** `run_scripts()`, so synchronous
`getComputedStyle`/`getBoundingClientRect` at load time read real
computed values (browsers force this flush). Scripts that mutate the DOM
set `g_js_dirty` → one `js_rerender()` folds the change in — the same
idempotent light-collect path the pump already uses for every mutation.

## Verified (QEMU, `/observers` test page)
- `J0-observers-top.png`: getComputedStyle prints
  `color=rgb(26, 115, 232) font-size=18px width=200px display=block
  pad-left=6px`; MutationObserver "FIRED x2" (characterData + attributes);
  ResizeObserver "FIRED w=338 h=36" (150→320px width); IntersectionObserver
  above-fold "isIntersecting=true".
- `J1-observers-lazy.png`: after scrolling, the below-fold lazy box loads
  its content ("LAZY LOADED ON SCROLL", turns green) — IntersectionObserver
  scroll delivery.
- Serial markers: `OBS-GCS rgb(26, 115, 232) 18px 200px block`, `OBS-MUT`,
  `OBS-RESIZE 168→338`, `OBS-ABOVE true→false`, `OBS-LAZY false→true`.
- Regressions clean: `/spa` (Preact), `/grid`, `/css` torture all render
  as before (the reorder didn't disturb them).

## v1 limits
- MutationObserver records carry `{type, target}` only — no populated
  `addedNodes`/`removedNodes`/`attributeName`/`oldValue`.
- IntersectionObserver thresholds are stored but delivery is binary
  (enter/leave); ratio is reported but not used to gate callbacks.
  `rootMargin` is parsed and ignored; root is always the viewport band.
- ResizeObserver reports the border-box size for both content and border
  box sizes.
- No `getBoundingClientRect` sub-pixel; integer px throughout.
- getComputedStyle covers the common properties above, not the full set.
