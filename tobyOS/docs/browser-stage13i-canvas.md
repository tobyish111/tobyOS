# Browser stage 13I — Canvas 2D

Branch `browser-canvas`, stacked on `browser-brotli-h2` (13G). Adds
`<canvas>` + a 2D context, the drawing surface a whole class of pages
needs (charts, editors, games, visualisations) that render nothing
without it. **Entirely browser-side** (`programs/user_gui_browser/
main.c`) — no kernel changes.

## What shipped
- **`<canvas>`** is now a laid-out replaced element. It reuses the
  existing image machinery: `collect_node` gives a `<canvas>` an ARGB8888
  backing store (`struct img` with `pixels`, `w`/`h` from the width/height
  attributes, default 300×150), so it lays out and paints as an image via
  the existing display-list `DI_IMG` path. The UA stylesheet rule that
  hid canvas (`display:none`, from when it was unsupported) is replaced
  with `canvas{display:inline-block}`.
- **`canvas.getContext('2d')`** returns a `CanvasRenderingContext2D` with:
  - `fillStyle` / `strokeStyle` / `lineWidth` / `font` state, and a CSS
    colour parser (`#rgb`, `#rrggbb`, `rgb()/rgba()`, ~16 named colours).
  - `fillRect` / `strokeRect` / `clearRect`.
  - Paths: `beginPath` / `moveTo` / `lineTo` / `closePath` / `rect` /
    `arc` (polyline-approximated), then `stroke()` or `fill()`.
  - `fillText` / `strokeText` / `measureText` (an embedded public-domain
    8×8 bitmap font, scaled to the requested px — the kernel TTF
    rasterizer only draws into the window, not an arbitrary buffer).
  - `drawImage(img, …)` (blit a loaded page `<img>` into the canvas).
  - `canvas.width` / `canvas.height` reflect the backing size.
  - No-op `save`/`restore`/`translate`/`scale`/`rotate`/`setTransform`/
    `clip`/`setLineDash`, and a stub `createLinearGradient`.
- The drawing ops are **pure userspace pixel writes** into `im->pixels`
  (source-over composite), exposed as small `__dom.cv*` C primitives
  (`cvFillRect`, `cvClearRect`, `cvLine` — thick Bresenham, `cvFillPoly` —
  even-odd scanline fill, `cvText`, `cvDrawImg`, `cvSize`); the prelude
  builds the JS context on top. A `g_canvas_dirty` flag triggers a repaint
  (not a relayout) after canvas draws in timers / rAF (`js_pump_all`) and
  event handlers (`js_dispatch_key`); load-time draws show on the first
  paint.

## Verified (QEMU, `/canvas` test page)
- Canvas 1 (shapes): blue `fillRect`, red `strokeRect`, green triangle
  (`moveTo`/`lineTo`/`fill`), orange circle (`arc`+`fill`), purple
  diagonal (`moveTo`/`lineTo`/`stroke`), and "Hello Canvas 0123" text —
  all render correctly.
- Canvas 2 (bar chart): six colour-coded `fillRect` bars with `fillText`
  value labels and a stroked x-axis — a real charting use.
- Canvas 3 (animation): a `setInterval` loop `clearRect`s and redraws a
  moving box + frame counter; the screenshot caught "frame 52" with the
  box advanced, proving the repaint hook fires per tick.
- No JS errors (`CANVAS-1/2/3-DRAWN` markers). Regressions clean:
  `/observers` (getComputedStyle + all three observers) renders
  identically — the `Element.prototype` `width`/`height`/`getContext`
  additions don't disturb other pages.

## v1 limits
- No transforms (translate/scale/rotate are no-ops); draw in device px.
- `fillText` uses a fixed 8×8 bitmap font (monospaced advance), not the
  page's TTF; good enough for labels, not typographic text.
- `arc` is polyline-approximated (fine at page scale); no bezier curves
  (`quadraticCurveTo`/`bezierCurveTo`), no `clip()`, no gradients/patterns
  beyond the stub, no `globalAlpha`/composite modes, no `getImageData`/
  `putImageData`, no even-odd/nonzero fill-rule distinction.
- Canvas backing size is fixed at element-creation; setting
  `canvas.width`/`height` from script updates the attribute but does not
  reallocate the store (v1).
