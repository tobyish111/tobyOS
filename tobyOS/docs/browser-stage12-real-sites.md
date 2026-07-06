# MISSION: tobyOS browser — Stage 12: progressively harder REAL sites (milestone 5)

The engine is real now: DOM + CSSOM + cascade + block/inline/float
layout, QuickJS with a live event loop, and an unmodified Preact app
runs (stages 7–11, all merged to `main` at `902e507`). The proof
ladder from `browser-engine-roadmap.md` stands at 4/5. This stage is
rung 5: **real websites, rendered correctly and used interactively.**
Not Gmail yet — Gmail is the summit and still needs most of Phase 11.
This stage is the base camp: transport depth + the two missing layout
primitives + a responsive UI while loading.

Read FIRST (memory): `[[browser-js-quickjs]]` (phases 9–11 history +
every JS/test gotcha), `[[browser-dom-css-engine]]` (engine
architecture + layout internals), `[[browser-omnibox-tls]]` (stages
1–6, networking), `[[tobyos-build-env]]`, `[[gui-line-vertical-hang]]`
(flaky TKAPP boot — retry 2–3x, never single-sample-bisect).

## Where things stand (verify against the tree, don't trust line numbers)
- `programs/user_gui_browser/main.c` (~5600 lines): the whole engine.
  Per-tab `struct eng` (~2.9 MiB heap): DOM nodes (index-linked),
  CSS rule pools, display list. Per-tab persistent QuickJS runtime.
- Layout: block flow + inline formatting contexts + floats. **No
  tables** (td/th flow inline in a block tr), **no flexbox** (degrades
  to block), **no position** (absolute/fixed lay out in-flow).
- Transport: kernel `SYS_HTTP_FETCH` is synchronous and truncates to
  caller caps. **Chunked transfer-encoding is REJECTED** in
  `src/http.c` (HTTPE -5) — many real sites fail at transport before
  the engine even sees them. No keep-alive (every asset = fresh
  TCP+TLS handshake). Page cap RAW_CAP = 96 KiB (truncates Wikipedia),
  ≤3 stylesheets, TLS accepts all certs.
- The browser UI FREEZES during every fetch (page, sheets, scripts);
  images load cooperatively one per idle pass. JS `fetch()` is a
  Promise over a blocking call.

## Target ladder (each = a screenshot-proof acceptance test)
1. **A chunked-encoding site renders.** Today it errors. Local chunked
   endpoint first, then a real one.
2. **news.ycombinator.com is readable and clickable.** Pure table
   layout — the acid test for real `<table>` support. Rows, numbers,
   titles, subtext all in their columns.
3. **A full Wikipedia article, untruncated**, with the infobox as a
   real right-floated TABLE (floats already work), thumbnails, and
   correct sections. Needs bigger caps + tables.
4. **A flexbox page lays out horizontally** — nav bar / card row test
   page, then a real site header (e.g. mojeek's).
5. **The UI never freezes**: scroll and switch tabs WHILE a slow page
   loads; JS timers keep ticking during `fetch()`.

## Scope, in priority order (one feature branch each, off `main`)

### A. Transport depth — branch `http-chunked-keepalive` (kernel-side)
- **Chunked transfer decoding** in `src/http.c` (dechunk into the
  caller buffer, honor HTTP_F_TRUNCATE semantics; gzip+chunked
  compose — dechunk BEFORE inflate).
- **HTTP/1.1 keep-alive**: small per-host connection cache (host,
  port, TLS session) with reuse + idle timeout; hit it for sheets/
  scripts/images. Measure: Wikipedia asset count vs handshakes.
- Raise caps: RAW_CAP 96K→512K, sheets 3→6 @ 256K, and scale the
  per-tab eng pools to match (NODE/ATTR/TPOOL/ITEM). Mind: 6 tabs ×
  bigger eng = heap growth; the libtoby malloc free-list handles it.
- GOTCHA: `struct http_response`/abi growth ⇒ touch every http.h
  includer (http/pkg/shell/kernel/syscall.c — no dep tracking).

### B. Table layout — branch `browser-tables`
Real CSS table algorithm, v1: fixed + auto layout (min/max content
width per column via measure passes — the shrink-to-fit machinery
from floats generalizes), colspan (rowspan can degrade), th/td
borders + backgrounds + padding, border-collapse basic, caption.
Table = new block-context type in `lay_block`; cells are block
containers. Acceptance: HN front page + a Wikipedia infobox.

### C. Flexbox — branch `browser-flexbox`
The roadmap's "one modern primitive that unlocks the most real
sites." v1: `display:flex` row/column, `flex-direction`, `justify-
content` (start/end/center/space-between), `align-items` (start/
center/end/stretch), `flex-grow/shrink/basis` (the `flex` shorthand),
`flex-wrap`, `gap`. Items are block containers measured via the
existing measure pass; distribute free space per grow/shrink.
Acceptance: a flex test page matching Chrome's layout for ~10 cases.

### D. Async fetch — branch `kernel-async-http` (the known blocker)
New kernel ABI: `SYS_HTTP_START` → handle, `SYS_HTTP_POLL(handle)` →
{state, bytes}, `SYS_HTTP_READ/FINISH`. Kernel drives the transfer
from its own context (the TCP/TLS stack is already there). Browser:
page/sheet/script/image loads become poll-driven from the main loop;
JS `fetch()` resolves its Promise from the pump (NOW truly async —
timers tick during fetches). Keep `SYS_HTTP_FETCH` for wget/pkg.
This is kernel work — budget a full session; `make clean` after any
ABI struct change.

### E. Reach (only if the above land): `position: absolute/relative/
fixed` v1 (nearest-positioned-ancestor containing block, fixed =
viewport, paint after in-flow), SVG placeholder→minimal renderer
(rect/circle/path-fill for icons), keyup/focus/blur events, popstate
+ back/forward for pushState SPAs, persistent localStorage under
`/data/browser/<host>`.

## Ground rules (unchanged, the user is strict)
- One feature branch per item, off `main`. Commit there; **do NOT
  merge to `main` without asking** — the user reviews and directs the
  merge (last round: branches stack fine, tip fast-forwards all).
- Trailer: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- `make` from `/c/CustomOS/tobyOS`; `git` from `/c/CustomOS`. Never
  commit `*.log`, `*.img`, `.claude/`, or scratchpad test assets
  (preact-standalone.js stays out of the repo).
- Verify every step in QEMU with screenshots; finish with an
  EliteDesk real-HW pass (serial COM4 @ 38400, AMT off) — stages 7–11
  have NOT had one yet; do that first if hardware is available.

## Build & test (the gotchas that cost hours — all still true)
```
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
make "CC=TMP='C:\t' clang" "HOST_CC=TMP='C:\t' gcc" \
     EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DTKAPP_BOOT -DTKAPP_BROWSER" iso
```
- EXTRA_CFLAGS staleness: `touch src/kernel.c` when toggling TKAPP
  flags; verify `[TKAPP] launching` is in tobyos.bin. Rebuild PLAIN
  (no TKAPP) before handing the ISO over.
- Struct/ABI layout changes ⇒ `make clean` (no header dep tracking).
- FP code (and anything exchanging doubles with QuickJS) builds
  `-msse -msse2` — see the QJS Makefile rules for the pattern.
- `JS_Eval` needs NUL-terminated buffers. Preact probes `'onclick' in
  dom` — keep the on* prototype stubs when touching the prelude.
- Test rig: scratchpad `websrv.py` (port **8077**; 8000 = Epic Games)
  + `browser_drive.py` (QMP send-key; boot-retries 3x; omnibox needs
  '/', Esc, '/' to clear before retyping). QMP mouse: the compositor
  scales relative deltas **exactly 3x** — send target/3 in ≤4px steps
  after a saturating reset to (0,0); prefer big click targets or
  synthetic `.click()`.
- TCG QEMU is ~3x slower than real time; never run builds while a
  QEMU verification boot is in flight.

## Definition of done
Targets 1–4 screenshot-verified in QEMU over the real internet,
target 5 demonstrated (scroll during load + ticking timers during
fetch), regressions clean (CSS torture page, /js2 events page, /spa
Preact page), docs per stage (`browser-stage12-*.md` + roadmap status
updates), memory updated, branches pushed and left unmerged for
review.
