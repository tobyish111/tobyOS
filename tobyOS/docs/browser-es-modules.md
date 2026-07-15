# ES modules + the two bugs found chasing them

## ES modules (`<script type="module">`, import/export, dynamic import)

QuickJS resolves a module graph through two hooks, both now registered
per-tab in `js_ensure` via `JS_SetModuleLoaderFunc`:

- **normalize** (`js_module_normalize`): specifier + importer's name →
  absolute name. Delegates to `resolve_relative_url`, so modules are
  keyed by absolute URL.
- **load** (`js_module_loader`): name → compiled `JSModuleDef`. Fetches
  synchronously (`sys_http_fetch`, mirroring the classic external-script
  path) and compiles with `JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY`.

QuickJS caches modules by name, so each URL loads at most once per page
and import cycles terminate. `run_scripts` detects `type="module"` and
routes to `js_eval_module`; a module's name is its own absolute URL (an
inline module's is the document URL) so its relative imports resolve
correctly. `import.meta.url` is set via `JS_GetImportMeta` before
evaluation. Dynamic `import()` rides the same loader for free.

Module bodies may top-level-await, so evaluation returns a **promise** —
it can still be pending when the script loop ends and reject later. The
promises are collected and re-checked after `js_drain_jobs`, so a late
rejection surfaces instead of vanishing.

**Verified on-screen** (`BROWSER_EXTRA=-DESM_TEST`, host serves
`esm/*.mjs` from 10.0.2.2:8099), 11/11 checks, zero JS errors: named /
default / namespace (`import * as`) imports, `./` and `../` specifiers,
`import.meta.url` on both entry and dependency, module identity/caching,
live bindings, re-export, and dynamic `import()`.

**Limits:** bare specifiers ("react") have no import-map support and
resolve as relative paths (honest 404 rather than a hang); the loader
fetch is synchronous (blocks the UI, same as classic external scripts);
no `<link rel=modulepreload>`.

## URL resolution fixes (needed before any of it worked)

`resolve_relative_url` did **no dot-segment collapsing**, so `./x.js`
became `/dir/./x.js` and `../` broke outright — fatal for modules, whose
specifiers are almost always `./` or `../`. Added `url_norm_dots` (RFC
3986 remove_dot_segments; query/fragment ride along untouched). Also
fixed **protocol-relative** `//host/path`, which was treated as
root-relative and produced `https://host//other/path` — real sites emit
these constantly.

> **Gotcha:** `url_norm_dots` must be told the caller's capacity. Several
> callers pass 512-byte `src[]` fields, and copying back `URL_MAX+1`
> (1025) smashed their buffer — this showed up as a *kernel* page fault
> with `cr2 ≈ base + 4 GiB`. Pass `out_max`, never `URL_MAX`.

## Bug 1: a remote font could panic the kernel (`src/kfont.c`)

`stbtt_GetFontOffsetForIndex` returns **-1** for data it doesn't
recognize. Both call sites passed that straight into `stbtt_InitFont`,
which used it as an unsigned offset → the kernel dereferenced
`data + 0xffffffff` (data + 4 GiB) → EXCEPTION 14 in ring 0 → panic.

The web-font path takes **attacker-supplied bytes**: any site serving a
truncated/non-font body could halt the OS. Both sites now gate on a
valid in-bounds offset (`off >= 0 && off < size`) and reject cleanly.

## Bug 2: HTTP/3 never decompressed response bodies (`src/quic_conn.c`)

The big one. We advertise `Accept-Encoding: gzip, br` and auto-upgrade to
h3 on Alt-Svc — but `http3_fetch_core` only *parsed* `content-encoding`
(setting `HTTP_ENC_GZIP`/`HTTP_ENC_BR`) and returned the still-compressed
body. The h1 and h2 paths each decompressed; h3 did not.

So **every resource fetched over HTTP/3 arrived compressed**. Invisible
for `<img>` (decoders sniff their own format) but fatal for `<script>`
and CSS: QuickJS was handed brotli bytes and reported
`SyntaxError: unexpected character`. On youtube.com this was 9 syntax
errors across 22 h3 fetches, and the "malformed" web fonts the kernel
rejected were simply *gzipped* fonts.

Factored the decompression out of `http.c` into a shared
`http_body_decompress(out, &body, &len, max_out, tag)` and called it from
the h3 path. Any transport advertising `Accept-Encoding` must call it.

**Result on youtube.com:** 20 bodies now decompress on h3; the JS errors
went from 9 parse failures to 3 *runtime* errors (`TypeError: not a
function`, `cannot read property 'prototype' of undefined`) — YouTube's
scripts genuinely parse and execute now, and the remaining errors are
missing browser APIs, not corrupted bytes.

## Note: YouTube ships no ES modules

Probed directly: all 43 `<script>` tags on youtube.com have **no `type`
attribute** and there is zero `nomodule`. The main bundle URL is
`…kevlar_base.en_US.…**es5**.O` — YouTube transpiles down to **ES5** for
our UA. The original "unexpected token '*'" was never `import * as`; it
was brotli. ES modules are still the right investment for the modern web
at large — just not what youtube.com needed.

## Next lever for heavy sites

YouTube's CSS is **3.4 MB** decompressed in one bundle (total 3.45 MB),
but `SHEET_FETCH_CAP` is 256 KiB, so it truncates on any transport.
Raising it needs `CSSPOOL_CAP` (320 KiB) and `RULE_MAX`/`DECL_MAX`/
`PART_MAX` scaled with it, plus parser validation at ~40k rules.
