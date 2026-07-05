# Browser stage 9 — JavaScript: QuickJS + DOM bindings

Branch: `browser-js` (off `browser-dom-css`). Phase 9 of
`browser-engine-roadmap.md`: port an engine, don't write one.

## The port
Vendored **QuickJS 2025-04-26** (Bellard) at `third_party/quickjs/`
(quickjs.c/h, libregexp, libunicode, cutils, dtoa, LICENSE). It
compiles **freestanding against libtoby** with three tiny vendored
patches, all `__TOBYOS__`/`QJS_NO_ATOMICS`-guarded:
- Atomics opt-out (single-threaded browser; no pthread/stdatomic dep).
- `js_def_malloc_usable_size` -> 0 (libtoby malloc has no usable-size).
- Timezone offset -> 0 (tobyOS is UTC-only; no `tm_gmtoff`).

libtoby grew real additions for it (useful beyond QuickJS):
- `include/inttypes.h` (PRI* macros), `include/fenv.h` (stub),
  `include/sys/time.h` + `gettimeofday()` in `src/time.c`.
- `src/math.c`: fmin/fmax/hypot/sinh/cosh/tanh/asinh/acosh/atanh/
  expm1/log1p/rint/nearbyint/lrint (the QuickJS `Math` table).
- `src/int128.c`: `__udivti3/__umodti3/__divti3/__modti3` (compiler-rt
  128-bit division helpers clang emits; no compiler-rt freestanding).

**SSE**: QuickJS objects and the browser's `main.c` build
`-msse -msse2` (they exchange doubles across the QuickJS ABI), matching
the established libtoby pattern (math/stdio/font/image are SSE too;
the kernel FXSAVEs user FP state). Makefile: `QJS_OBJS` + a dedicated
browser rule replacing the generic `LIBTOBY_PROGRAM_RULES`.

## Bindings architecture (the "hard 70%" begins)
- C exposes ~16 primitives on a `__dom` global operating on **node
  indices** (ints): byId, query (reuses the engine's real CSS selector
  parser + matcher), create, text, append, remove, setText/getText,
  setHTML (reuses `dom_parse` -> real fragment parsing), getAttr/
  setAttr (rebuilds the node's attr block contiguously), tag, parent,
  children, body, title. Plus `console.log/warn/error` -> fd 1 with a
  `[js]` prefix (greppable in the serial log).
- A JS prelude wraps them: `document` (getElementById, querySelector,
  createElement, createTextNode, body, title), `Element` prototype
  (textContent/innerText/innerHTML/id/className/tagName/parentNode/
  children/appendChild/removeChild/remove/get/setAttribute), and
  `element.style.xxx = ...` via a Proxy that serializes camelCase ->
  kebab-case into the `style` attribute — so the existing inline-style
  cascade picks mutations up with zero new style code.
- Stubs that make simple pages run: `window`, `alert`, `navigator`,
  `addEventListener` (no-op), `setTimeout`/`requestAnimationFrame`
  (run synchronously — honest placeholder until the phase-10 event
  loop), `setInterval` (no-op).

## Execution model (phase 9 scope)
`render_html()`: `dom_build` -> **`run_scripts()`** -> UA sheet ->
collect (styles/links/forms/images) -> cascade -> layout. Scripts run
once per load: inline `<script>` text (now captured raw by the parser
— stage 7 dropped it) and `src=` scripts (fetched like stylesheets,
192 KiB cap). `type=` values that aren't javascript/module are
skipped. Exceptions land in the status bar + `[js] ERROR:` on serial.
The runtime (32 MiB memory cap, 192 KiB JS stack, stack-overflow
check enabled) is created and freed per load — nothing persists,
because nothing can re-enter it yet. Phase 10 keeps it alive and adds
real events/timers/fetch.

## Deliberate limits
- No event loop: listeners are no-ops, timers run synchronously at
  load. No `fetch`/XHR. One runtime per load, then freed.
- `innerHTML` get returns "" (set is real); `document.title` get too.
- Scripts added by scripts (via innerHTML) do not execute (matches
  real browsers for innerHTML).
- DOM node arena: removed nodes leak their slots until the next
  navigation (arena resets per page).
