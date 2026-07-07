# Browser stage 12E: reach items (position, SVG, storage, popstate, focus)

Branch `browser-reach` (stage-12 scope item E, off `main` after 12A–D
merged). The "only if the above land" list from the stage-12 brief:
five smaller features that each unlock a class of real pages.

## What landed (programs/user_gui_browser/main.c)

### position: relative / absolute / fixed (v1)
- New `cstyle.pos` + `po[4]` (top/right/bottom/left, M_AUTO sentinel);
  `position`/`top`/`right`/`bottom`/`left` parsed.
- `relative`: the element lays in normal flow, then its display items
  shift by the offsets — the flow slot is untouched (siblings advance
  as if unshifted), exactly per spec.
- `absolute`/`fixed`: queued out-of-flow during the flow pass (with a
  snapshot of the containing block), laid AFTER the whole in-flow pass
  so they paint on top. A positioned ancestor (`pos != static`) is the
  containing block for its absolute descendants (tracked via
  g_cb_x/y/w save/restore in lay_block); fixed uses the viewport.
  Shrink-to-fit width when `width:auto` (the frozen-measure trick).
- `fixed` items carry IF_FIXED: the painter and all three hit-tests
  (link/field, JS click, run_at) use viewport coords for them, so a
  fixed bar stays pinned while the page scrolls.
- v1 limits: `bottom` anchoring resolves only for fixed (viewport
  height is known; a positioned ancestor's is not), z-index ignored
  (paint order = queue order), no absolute-inside-inline baseline
  fixups beyond the static position.

### Minimal SVG renderer
- `svg_render()`: a from-scratch parser + even-odd scanline filler for
  `<rect> <circle> <ellipse> <polygon> <polyline> <path>` with solid
  fills (fill= attr, style="fill:", inherited `<g>` fill), viewBox
  scaling, and path data (M/L/H/V/C/S/Q/T/Z, A degrades to a line;
  cubics/quadratics flattened). defs/mask/gradient/clip/symbol subtrees
  are skipped. Renders to an ARGB canvas with a transparent
  background, 2x for small icons (crisper blit). Wired into the image
  pipeline as a fallback when stb_image can't decode and the bytes
  sniff as SVG — so Wikipedia's chrome icons (previously `[image
  failed]`) now render.
- No strokes, gradients, transforms, text, or filters (v1).

### Persistent localStorage
- One file per origin host under `/data/browser/<host>.ls`; the
  prelude's localStorage serializes on every mutation (setItem/
  removeItem/clear) and loads on JS-world startup via new
  `__dom.storeLoad/storeSave` (file syscalls open/read/write/close/
  mkdir). sessionStorage stays in-memory. Survives reload within a
  boot; survives reboot when `/data` is a persistent volume (the case
  on real hardware).

### popstate + back/forward for SPAs
- Per-tab `doc_gen` bumps on every real document render; history
  entries record the generation they belong to. `history_go(delta)`:
  moving between SAME-generation entries (pushState routes) fires a
  `popstate` event and updates the address bar with NO refetch;
  crossing a generation boundary refetches through the async nav path.
  Wired to the toolbar back/forward buttons, the `[`/`]` keys, and JS
  `history.back()/forward()/go()`.

### keyup / focus / blur events
- keyup is synthesized right after each keydown (the key ABI has no
  break codes) for both field-focused and page-level dispatch.
- `set_focus_field()` centralizes focus handoff and fires `blur` on
  the old field's node and `focus` on the new one; all focus
  assignment sites route through it.

## Verified in QEMU (screenshots in the session scratchpad)
- `/pos`: absolute badge pinned top-right of its relative container,
  absolute overlay painting on top of flow, a relative box shifted
  left:60px/top:8px with the flow slot reserved, and a fixed green
  bar that stays pinned through a page scroll.
- `/svgtest`: a tricolor rect+circle+rect icon, an orange Z-closed
  star path, and a gear (g-fill inherit + circle + polygon) all
  render.
- `/store`: visit counter reads 1, then 2 after reload (file written
  then re-read by a fresh JS runtime).
- `/popstate`: two pushState calls, then two history.back() calls fire
  "POPSTATE fired" twice and restore the URL to /popstate — no
  refetch.
- `/focus`: focus / keyup (per character) / blur all fire on the
  right input as it's focused, typed into, and unfocused.
- Regressions: tables and CSS torture pages unchanged.

## Bug found during bring-up
`g.history` had leftover no-op `back`/`forward` stubs AFTER the real
ones in the same object literal — later keys win, so history.back()
silently did nothing. Removed the stubs. (Also confirmed
`querySelector('input[name=a]')` — tag + attribute selector — resolves
correctly; an earlier "null" was the stale-test-server trap, not a
real bug.)
