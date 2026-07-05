# Browser stage 7 — DOM tree + CSS engine (the rendering-core pivot)

Branch: `browser-dom-css`. This is Phase 8 + Phase 7 of
`browser-engine-roadmap.md`: the flat span/block/run model is **gone**,
replaced by the retained-tree pipeline every real engine uses:

```
  g_raw (HTML)  -> DOM tree      (dom_build: tokenizer + tree constructor)
  CSS           -> rule pools    (css_parse_sheet: UA sheet, <style>,
                                  style="", fetched <link rel=stylesheet>)
                -> computed style per node   (style_node: cascade)
                -> display list  (lay_block/flush_inline: block + inline flow)
                -> paint         (paint_all walks ditems; TTF/blit unchanged)
```

Everything else — tabs, omnibox, history, forms, images, find-in-page,
cookies/gzip/TLS networking — survives on top of the new core.

## What the engine supports (v1)

**DOM** (`struct dnode`, index-linked; per-tab heap `struct eng` ~2.9 MiB):
- HTML5-subset tokenizer + tree constructor: void elements, implied end
  tags (`p`/`li`/`dt`/`dd`/`td`/`th`/`tr`/`option`), raw-text elements
  (`script`/`style`/`title`/`textarea`), comments/doctype, one
  `html`/`head`/`body` each, attribute pool with entity-decoded values.
- Entities + UTF-8 transliterated to ASCII (kernel TTF path is ASCII).
- Text nodes coalesce; whitespace collapses at layout per `white-space`.

**CSS**:
- Sources in cascade order: built-in UA sheet (light theme: white page,
  dark text, blue links — pages design for this), `<style>` blocks,
  `<link rel=stylesheet>` (fetched synchronously, ≤3 sheets ≤160 KiB
  each, gzip handled kernel-side), inline `style=` attributes.
- Selectors: type/`*`/`.class`/`#id`/`[attr]`/`[attr=v]`, compound,
  descendant + child (`>`) combinators, `:link`/`:visited`; unsupported
  selectors (`:hover`, `::before`, `+`/`~`, `:not(...)`) invalidate only
  their own selector, not the whole rule group.
- Cascade: origin (UA < author < inline < `!important`), specificity
  (id=100/class=10/type=1), source order; inheritance of color/font/
  text properties; `em`/`rem`/`pt`/`%`/`vw`/`vh` units; hex/rgb()/rgba()/
  ~50 named colors; `@media` (min/max-width, screen/print, orientation,
  prefers-color-scheme) evaluated against the live viewport at parse
  time — a resize reflows but does not re-evaluate media queries until
  the next navigation/reload.
- Properties: display (block/inline/inline-block/list-item/none; flex/
  grid/table degrade to block), color, background(-color), font-size
  (+keywords), font-weight, font-family (mono detection), text-align,
  text-decoration, line-height, white-space, list-style(-type), margin/
  padding (+shorthands, `margin:auto` centering), border (+per-side
  shorthands, width/color), width (px/%), height, max-width, visibility.

**Layout** (`lay_block` + `flush_inline` -> `struct ditem` display list):
- Block flow with the real box model: margin (sibling collapsing),
  border, padding, width/max-width resolution, `margin: 0 auto`.
- Inline formatting contexts: word wrap, per-word style runs coalesced
  into items, bottom-of-line alignment for mixed font sizes, text-align
  shifting, `<br>`, pre wrapping, atomic inline boxes (images, form
  controls) participating in line layout.
- Backgrounds/borders emitted as rect items (bg before children, height
  patched after) — paint is a flat walk in emission order.
- Page background = body/html bg promoted to the whole viewport.

## What is intentionally NOT here yet (known limits)
- No floats, no `position: absolute/fixed/relative` (laid out in-flow),
  no real flexbox/grid (block fallback), no table column layout
  (`td`/`th` flow inline within a block `tr`).
- Non-replaced `inline-block` degrades to inline; block-in-inline
  degrades to inline flow.
- rgba alpha is thresholded (>=50% opaque, else transparent) — no
  compositing; `visibility:hidden` behaves as `display:none`.
- Media queries are snapshot at parse time (reload after resize).
- Source order of sheets: `@import` is skipped; ≤3 linked sheets.
- Cascade is O(nodes x rules) with a rightmost-tag quick reject — fine
  in practice; add rule hash buckets if a page ever feels slow.

## Architecture notes / gotchas
- `struct eng` is heap-allocated per tab (`tab_reset` mallocs, `tab_close`
  frees the closing tab's engine BEFORE the shift; the vacated slot is
  zeroed without freeing because pointers were duplicated by the struct
  copy — same rule as image pixels).
- All tree links are **indices**, not pointers, so tab-shift copies stay
  safe; `E` is `cur->eng` (the active tab's engine — background-tab image
  relayout flips `g_active` exactly like before).
- Find-in-page still works over a flat text buffer: inline layout writes
  the visible text into `eng->render` as it lays words, and DI_TEXT items
  carry (off,len) slices — the old find/hit-test code shape survives.
- Advance-width cache is now keyed by (px,bold) with 14 round-robin slots
  (CSS font-size is continuous) — `adv_for()` replaces the fixed 5-face
  table; fills lazily via `tk_text_width` one char at a time.
- Source/plain view synthesize `doc > body > pre > #text` and run the
  same pipeline (no second render path).
- The old libtoby `html_parser.c`/`css_parser.c`/`layout.c` from commit
  4f2924b are dead pre-stage-1 scaffolding (6 KiB/node DOM, 2048-node
  cap, different font path) — not used, not touched.

## Test rig (scratchpad)
`websrv.py` (port **8077**; 8000 is Epic Games) serves `/css` (selector/
box/cascade torture page), `/ext.css`, `/form`, `/found`, `/img`,
`/pic.png`. `browser_drive.py local` boots the `-DTKAPP_BOOT
-DTKAPP_BROWSER` ISO headless and screendumps per stage (home, css page,
source view, new tab + form submit, tab switch, maximize reflow, inline
image); `browser_drive.py net` does example.com / Wikipedia / Mojeek over
real SLIRP internet. All the stage-1..6 build gotchas apply (touch
`src/kernel.c` when toggling TKAPP flags; retry flaky TKAPP boots 2-3x).
