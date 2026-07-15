# Large-page fetch + youtube.com rendering assessment

## What changed

Modern sites routinely ship > 512 KiB of HTML with hundreds of KiB of
*inline* `<script>` (SPAs inline their bootstrap data + config). The old
fetch/parse caps truncated these mid-`<script>`, producing a cascade of
spurious `SyntaxError`s and a blank page. Raised the caps so heavy pages
load whole:

| cap            | old     | new     | storage            |
|----------------|---------|---------|--------------------|
| `RAW_CAP`      | 512 KiB | 1.5 MiB | `struct tab` (BSS, x6) |
| `RENDER_CAP`   | 576 KiB | 1.6 MiB | `struct eng` (heap, per tab) |
| `TPOOL_CAP`    | 768 KiB | 2 MiB   | `struct eng` (heap) |
| `JS_SRC_CAP`   | 384 KiB | 4 MiB   | `malloc` per external `<script src>` |
| `JS_FETCH_CAP` | 256 KiB | 4 MiB   | `malloc` per `fetch()`/XHR |

Net static-BSS growth ~6 MiB (raw[] x6 tabs); heap growth ~2.3 MiB per
*open* tab. Harmless on 512 MiB. Default home page + normal browsing
verified unchanged (no regression, no OOM/panic).

Also added a global **`Image`** constructor (`new Image(w,h)`): a detached
`<img>` shim whose `src` setter fires `load` synchronously so preloaders,
tracking pixels and lazy-loaders progress instead of throwing
`ReferenceError: 'Image' is not defined`. This is universal, not
YT-specific.

## youtube.com: what renders, and the wall

Gated behind `-DYT_TEST` (auto-navigates to `https://www.youtube.com/`
at startup, mirroring the other `*_TEST` build flags).

**Before** the cap bump: blank white page, `Done - 0 links` — the 777 KiB
page truncated at 512 KiB, chopping inline scripts.

**After**: the server-rendered **masthead skeleton paints** — the search
box + the three top-right icon buttons — and `Done - 15 links`. This is
YouTube's pre-hydration shell, and it renders correctly.

The **content grid stays blank**, and this is an architectural ceiling,
not a fixable bug:

- YouTube's visible UI is built at runtime by its client application
  (`desktop_polymer.js` / `base.js`, ~2-3 MiB of minified Polymer 2 +
  custom-element definitions) which reads `ytInitialData` and constructs
  the entire `<ytd-app>` DOM in JavaScript. The static HTML contains
  almost no visible content (15 `<a>` in 777 KiB).
- Those framework bundles are shipped as **ES modules** (`import`/`export`,
  generators). Our script runner evals scripts as classic global scripts
  (`JS_EVAL_TYPE_GLOBAL`), so they fail to even parse — the residual
  `SyntaxError: unexpected token '*'` / `unexpected character` are
  `import * as …` / `export`. Module support would need a full loader
  (specifier resolution + fetch + link), and even then…
- …the app depends on a Chromium-class JS engine (perf) plus MediaSource
  Extensions, WebGL, IntersectionObserver/ResizeObserver, Trusted Types,
  `scheduler.postTask`, a fetch-intercepting service worker, and dozens
  more APIs. Hydrating the video grid is the same class of effort as
  "reimplement a production browser + JS VM," not a handful of shims.

**Verdict:** the realistic ceiling for youtube.com on a hand-rolled
engine + QuickJS interpreter is the server-rendered shell, which now
renders. Full app hydration is out of reach — the same wall as the
video-playback signature-cipher arms race (parked).
