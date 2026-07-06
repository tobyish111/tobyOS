# MISSION: tobyOS browser — the road to a real engine (goal: everything, incl. Gmail)

The goal is Chrome-parity: render *everything*, including JavaScript web
apps like Gmail. Be honest with yourself and the user about what that
means — **this is building a browser engine**, not adding features. There
are ~3 independent engines in the world (Blink, Gecko, WebKit), each
hundreds of engineer-years. Treat this as a multi-phase, multi-month
(realistically multi-year) effort with real intermediate milestones, not
a sprint to Gmail. Ship value at every phase.

Read `[[browser-omnibox-tls]]` first — it has the entire browser history
(stages 1–6), the architecture, and every test/build gotcha.

## Where the browser is today (stages 1–6, all merged to `main`)
`/bin/gui_browser` (`programs/user_gui_browser/main.c`, ~2800 lines) is a
TobyTK app that already does a lot:
- **Layout engine** (hand-rolled): HTML → styled **spans** → **blocks**
  → positioned **runs**, painted via the kernel TTF rasterizer.
  Proportional fonts, headings, bold, inline clickable links, bullets,
  hr, pre/code, pixel scroll, find-in-page, WM title = page title.
- **Forms** (GET), **images** (PNG/JPEG/GIF/BMP via libtoby stb_image,
  cooperative/incremental non-blocking load), **tabs** (per-tab bundle,
  Ctrl+T/W/N/P), **cookies + gzip** (kernel-side in `src/http.c`,
  `src/puff.c`), Chrome-style omnibox, redirects, DDG→Mojeek search,
  kernel **TLS 1.3** (proven on real HW: HP EliteDesk).

## THE HARD TRUTH: the current render model is a dead end for JS/CSS
The flat `span/block/run` model was right for a tag-stripping reader. It
is the **wrong foundation** for CSS and JavaScript and must be replaced,
not extended. A real engine is a pipeline of **retained trees**:

```
  HTML  →  DOM tree        (nodes: elements, text, attributes)
  CSS   →  CSSOM + cascade (computed style per node)
          →  Layout tree   (boxes: block/inline/flex/grid, positions)
          →  Paint         (display list → framebuffer)
  JS    ↻  mutates DOM/CSSOM → invalidates → re-style → re-layout → re-paint
```

The single most important early decision is to **build the DOM tree**
(Phase 8) — a real parent/child node tree with elements, text nodes, and
attributes — because CSS selectors, JS (`document.querySelector`,
`element.style`, event targets), and incremental reflow all address
*nodes*. The current parser throws structure away into a flat run list;
that has to become a tree the rest of the engine hangs off of. Expect to
**rewrite the rendering core**, reusing the TTF/blit painting primitives
and the networking/tabs/omnibox shell (those stay).

## PHASED ROADMAP (each phase is its own feature branch off `main`)

### Phase 7 — CSS engine (NO JavaScript). Biggest visual win.
**STATUS: v1 LANDED with Phase 8 on branch `browser-dom-css`** (see
`browser-stage7-dom-css.md`): tokenizer/parser for `<style>`/`style=`/
fetched `<link>` sheets, type/class/id/attr/descendant/child selectors,
cascade+specificity+inheritance, box model (margin/border/padding/
width/max-width/margin-auto), colors/backgrounds/fonts/text-align/
line-height, `@media`, block+inline flow. NOT yet: floats, position,
real flexbox/grid (degrade to block), table columns.
Static sites look "plain" today because we ignore CSS. A CSS engine is
the highest-leverage no-JS step and makes content sites (news, blogs,
docs, Wikipedia) actually look designed. Scope, in order:
- **Tokenizer + parser** for CSS (`<style>`, inline `style=`, and linked
  `<link rel=stylesheet>` — fetch those too). Selectors: type, class,
  id, descendant, `*`, basic combinators.
- **Cascade + specificity + inheritance** → a computed style per node
  (needs the DOM tree; if you do CSS before Phase 8, at least build a
  minimal element tree first — realistically do Phase 8 *with* or
  *before* 7).
- **Box model**: margin/border/padding/content, `display` (block/inline/
  inline-block/none), width/height, colors, backgrounds, font
  family/size/weight/style, `line-height`, `text-align`.
- **Layout modes**: normal flow first, then **flexbox** (the one modern
  layout primitive that unlocks the most real sites), then floats/`grid`
  as reach. `position: absolute/fixed/relative` for overlays/sidebars.
Milestone: a real news article or Wikipedia page renders with its actual
column/sidebar layout, fonts, and colors — recognizably itself.

### Phase 8 — DOM tree (the pivot). Prerequisite for all dynamic behavior.
**STATUS: v1 LANDED with Phase 7 on branch `browser-dom-css`**: real
parent/child node tree (`struct dnode`, index-linked, per-tab heap
engine), HTML5-subset tokenizer + tree constructor (void elements,
implied end tags, raw-text elements), attribute pool, computed style
per node; layout + paint + find + forms + images all hang off the tree.
The flat span/block/run model is gone.
Replace the flat span/block model with a retained node tree:
`struct dom_node { type (element/text/comment); tag; attrs[]; children[];
parent; computed_style; layout_box; }`. Rebuild the HTML parser to emit
this tree (a proper tokenizer + tree constructor — even a subset of the
HTML5 parsing algorithm). Layout consumes the tree + computed styles to
produce a layout/box tree; paint walks the box tree. Do this alongside
Phase 7 — CSS and DOM are co-dependent.

### Phase 9 — JavaScript engine. Port, don't write.
**STATUS: v1 LANDED on branch `browser-js`** (see `browser-stage9-js.md`):
QuickJS 2025-04-26 vendored (`third_party/quickjs`), compiled
freestanding against libtoby (3 tiny guarded patches; libtoby grew
inttypes/fenv/sys-time/gettimeofday/hyperbolic-math/int128-division).
DOM bindings: `__dom` C primitives + JS prelude = document/Element/
style-Proxy/innerHTML(real fragment parse)/createElement/appendChild/
querySelector(engine's own selector matcher). Scripts (inline + src=)
run once at load, then collect->cascade->layout. Milestone met: a
`<script>` page mutates the DOM on screen. NOT yet (phase 10): event
loop, listeners, timers, fetch — the runtime is per-load.
Writing a JS engine is folly; **port an embeddable one**. Best fit for a
freestanding, no-JIT, small-footprint target:
- **QuickJS** (Bellard) — ES2020, small, clean C, no OS deps beyond
  malloc/memcpy — recommended. Alternatives: **mujs** (smallest, ES5,
  easiest port) or **Duktape** (ES5/6, embeddable). This OS already runs
  off-the-shelf C in userspace (TinyCC, CPython, SQLite, per the memory)
  — the engine *port* is the tractable ~30%.
- Wire it to the DOM: bind `document`, `window`, `Element`,
  `getElementById/querySelector`, `element.style`, `innerHTML`,
  `addEventListener`, `createElement/appendChild`. **This binding layer +
  the Web APIs (below) are the hard ~70%,** larger than the engine port.
Milestone: a page with `<script>` that mutates the DOM (a counter, a
toggling menu) actually updates on screen.

### Phase 10 — Event loop + Web APIs + reactive reflow. Where apps start working.
**STATUS: v1 LANDED on branch `browser-events`** (see
`browser-stage10-events.md`): persistent per-tab runtime, real
setTimeout/setInterval + Promise microtask draining on the cooperative
main loop, click/input dispatch with bubbling + preventDefault +
onclick attrs (display items carry their DOM node), DOMContentLoaded/
load, element.value, fetch/XHR (Promise-shaped over the SYNC kernel
HTTP — async ABI still the known blocker), and reactive
mutate→recollect(light)→cascade→layout→repaint. NOT yet: keydown/
keyup, location/history, storage, JS-added stylesheets.
- **Event loop**: microtasks/macrotasks, `setTimeout/setInterval`,
  Promises (QuickJS has Promises built-in — wire them to the loop).
- **Events**: real dispatch (click/input/load/DOMContentLoaded) with
  capture/bubble to JS listeners; feed the browser's existing mouse/key
  events into DOM event dispatch.
- **Network APIs**: `XMLHttpRequest` and `fetch` (both over the kernel
  HTTP/TLS stack — but see the async note below; SPAs assume async).
- **Reactive reflow**: DOM/style mutation → invalidate → re-style →
  re-layout → re-paint. This closes the loop that makes JS visible.
Milestone: a small single-page app (a client-rendered demo, then a real
lightweight SPA) loads and is interactive.

### Phase 11 — the long tail (parallelizable, ongoing)
- Image formats: **SVG** (Wikipedia's icons — a vector renderer, its own
  mini-project), **WebP/AVIF** (add libs to libtoby).
- **Web fonts** (`@font-face`, WOFF2 → the TTF rasterizer already exists).
- Networking depth: **HTTP/1.1 keep-alive + chunked transfer** (chunked
  is currently *rejected* in `http.c` — many sites need it),
  **HTTP/2** (there's a stub `src/http2.c`), **WebSockets** (Gmail-class
  apps need them), **cert validation** (TLS currently accepts all certs —
  a real browser must verify), cache, `localStorage`/IndexedDB.
- **Canvas / video / audio**, accessibility, more CSS.

## Gmail specifically — set expectations
Gmail is one of the most demanding web apps in existence: heavy JS,
WebSockets, service workers, IndexedDB, modern CSS, and it actively
detects unsupported browsers. It is the **last** thing to work, not a
near-term target. Use a ladder of honest proof-milestones instead of
aiming straight at it:
1. A CSS-styled static news site / Wikipedia renders correctly (Ph 7–8).
2. A `<script>` DOM-mutation demo updates on screen (Ph 9).
3. `fetch` + a small hand-written SPA works (Ph 10).
4. A real lightweight SPA (e.g. a simple React/Preact app) renders.
5. Progressively harder real sites… Gmail is the summit, likely requiring
   most of Phase 11 too.

## PLATFORM CONSTRAINTS (do not fight these blind)
- **Freestanding userspace, no full libc**: the browser links **libtoby**
  (has `malloc`/`free` via an **sbrk bump allocator — NOT thread-safe and
  never frees to the OS**; stb_image; TTF). A big engine will stress this;
  you may need a real allocator (dlmalloc-style) in libtoby, and to grow
  the process heap. Check `libtoby/src/stdlib.c` + the ELF load / user
  heap map in `src/elf.c` (BSS is eagerly mapped; `USER_HALF_MAX` is the
  only ceiling).
- **Kernel HTTP is synchronous** (`SYS_HTTP_FETCH` blocks). SPAs assume
  async `fetch`. Either add an async/non-blocking kernel HTTP ABI, or run
  fetches on a worker — but libtoby malloc is not thread-safe, so real
  threads need a locking allocator first. This is a real blocker for
  Phase 10; plan for it.
- **Memory**: tabs already cost ~1 MiB each (fixed array, 6 tabs ≈ 6 MiB
  BSS). A DOM+CSSOM+JS heap per tab is far larger; revisit the inline
  fixed-tab design (move to heap-allocated per-tab engines).
- Rendering primitives to reuse: `tk_draw_text` (TTF), `tk_draw_fill`,
  `tk_draw_blit[_blend]`, `tk_text_width` — the paint layer stays.

## BUILD & TEST (unchanged; the gotchas that cost hours)
Build from `/c/CustomOS/tobyOS` (MSYS bash):
```
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
make "CC=TMP='C:\t' clang" "HOST_CC=TMP='C:\t' gcc" \
     EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT" iso
```
- **EXTRA_CFLAGS staleness trap**: toggling `-DTKAPP_BOOT`/`-DTKAPP_BROWSER`
  does NOT recompile `kernel.c`. `touch src/kernel.c` first, then verify
  `python -c "print(b'[TKAPP] launching' in open('tobyos.bin','rb').read())"`.
  A TKAPP-less kernel boots to login and `holding` never appears.
- **Flaky early-boot stall**: the TKAPP harness intermittently faults/
  stalls right after app spawn — retry-boot 2–4× before blaming your code
  (`[[gui-line-vertical-hang]]`).
- **Struct/ABI layout changes need `make clean`** (no header dep tracking).
- QEMU SLIRP = real DNS + internet; local server at `http://10.0.2.2:8077/`
  (**host port 8000 is taken by Epic Games — use 8077**). QMP relative-
  mouse targeting is unreliable — prefer keyboard test hooks. Drive with
  the scratchpad `browser_drive.py` + QMP `send-key`/`screendump` pattern.

## WORKFLOW (the user is strict)
- **One feature branch per phase, off `main`.** Commit there; **do NOT
  merge to `main` without asking** — the user reviews + directs the merge,
  then it fast-forwards. After merge the branch is deleted (commits live
  on `main`; origin keeps a pushed copy).
- Commit trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`.
- `make` from `/c/CustomOS/tobyOS`; `git` from `/c/CustomOS`. Never commit
  `*.log`, `*.img`, or `.claude/`.
- Verify every phase in QEMU with screenshots; a real-HW EliteDesk pass
  (serial COM4 @ 38400, AMT off) is the gold standard.

## READ FIRST (memory)
- `[[browser-omnibox-tls]]` — full stage 1–6 history + architecture + all
  test/build gotchas. **Start here.**
- `[[tobyos-build-env]]` — toolchain, TMP prefix, clean-rebuild rule.
- `[[file-explorer-tk-menu]]` — TobyTK app patterns, QMP screenshot recipe.
- `[[gui-line-vertical-hang]]` — the flaky TKAPP boot.
- `[[real-hardware-elitedesk-bringup]]` — real-HW networking + serial triage.
- `[[linux-abi-compat-b1]]` — how off-the-shelf C (TinyCC/CPython/SQLite)
  already runs in-VM; the same muscle ports a JS engine.
