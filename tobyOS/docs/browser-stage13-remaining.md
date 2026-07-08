# MISSION: tobyOS browser — Stage 13 remaining (Chrome-parity push)

You are continuing a multi-branch push to bring the tobyOS browser
(`programs/user_gui_browser/main.c`, ~8k lines) as close to Google
Chrome as a hobby OS realistically can. Stage 12 made it render real
sites (Hacker News, Wikipedia, mojeek) with async transport. Stage 13
is the honest gap-closing tier: the CSS/JS/transport features whose
absence most visibly breaks the modern web. Five are done; four remain,
plus a long tail.

Be honest about scope: Chrome is ~35M LOC and three engines exist in
the world. "Every page perfectly" is asymptotic. The job is to keep
landing the next highest-leverage feature, each verified on screen,
each its own reviewable branch.

## READ FIRST (memory + docs)
- Memory: `[[browser-stage13-chrome-parity]]` (the tier, the branch
  chain, every gotcha), `[[browser-stage12-transport]]` /
  `[[browser-stage12-tables]]` / `[[browser-stage12-async-http]]`
  (engine + transport architecture), `[[tobyos-build-env]]` (build on
  this Windows box), `[[gui-line-vertical-hang]]` (flaky TKAPP boot —
  retry, don't bisect).
- Stage-13 docs (what's already built, with the internals + v1 limits):
  `docs/browser-stage13a-css-vars.md`, `-13b-calc.md`, `-13c-grid.md`,
  `-13d-overflow-transform.md`, `-13e-webfonts.md`.

## WHERE THINGS STAND (verify against the tree, not line numbers)
Branch chain, all UNMERGED and pushed to origin, each stacked on the
previous (tips fast-forward cleanly):

    main  →  browser-reach (12E)
          →  browser-css-vars   (13A  CSS custom properties + var())
          →  browser-calc       (13B  calc())
          →  browser-grid       (13C  CSS Grid v1)
          →  browser-overflow   (13D  overflow clip + transform + pos-%)
          →  browser-webfonts   (13E  @font-face, TIP = 5ec02b6)

**Branch your next feature off `browser-webfonts`, NOT `main`** — main
lacks everything from 12E onward. The user reviews and merges the
branches themselves; do NOT merge to main without being asked.

## REMAINING WORK (priority order — one feature branch each)

### 13F — observers + getComputedStyle  (branch `browser-observers`)
Unblocks JS-driven content that never loads today (lazy images,
infinite scroll, responsive JS). Mostly browser/JS-side.
- `getComputedStyle(el)` returning a live-ish object of computed
  values (at least color/background/font-size/width/height/display/
  margin/padding) — add a `__dom.computed(node, prop)` C binding that
  reads the node's `struct cstyle`, wrap it in the JS prelude.
- `MutationObserver`: fire callbacks when the DOM mutates. The reactive
  rerender path already detects mutations (`g_js_dirty`); hook observer
  delivery into the pump (`js_pump_all`) as a microtask batch.
- `IntersectionObserver`: fire when an element's box enters/leaves the
  viewport — needs a scroll/layout hook comparing item rects to the
  visible band. Common for lazy-loading and "load more".
- `ResizeObserver`: fire when an observed element's laid size changes
  across relayouts.

### 13G — brotli + HTTP/2  (branch `browser-brotli-h2`, kernel-side)
- **Brotli** `Content-Encoding: br` decode in `src/http.c` (now more
  common than gzip on CDNs; today a `br` response is unusable). Port a
  small brotli decoder (or the reference) as `src/brotli.c`, wire it
  like the existing gzip/`puff.c` path (kernel-side, transparent to the
  browser). Advertise `Accept-Encoding: gzip, br`. **Bonus: this also
  unlocks WOFF2 web fonts (13E limit) — WOFF2 is brotli + glyph
  transforms.**
- **HTTP/2**: there is a 620-line stub `src/http2.c` — wire it in.
  Nearly every major site is h2. This is the harder half; brotli first.

### 13H — TLS certificate validation  (branch `kernel-tls-certs`, kernel-side)
**The one true must-have for a shippable browser** — `src/tls.c`
currently accepts ANY server cert (see the header comment). Needs: a CA
trust store (bundle a small root set in the initrd), X.509 chain
parsing + signature verification (RSA/ECDSA — the crypto primitives
exist in the kernel: `sec.h`, monocypher, `rng.c`), hostname matching
against SAN/CN, and expiry checks. Fail closed on invalid, with a
clear error page. Budget a full session; `make clean` after any ABI/
struct change.

### 13I — Canvas 2D  (branch `browser-canvas`)
`<canvas>` + a 2D context (fillRect/strokeRect/fillText/paths/
drawImage). A whole class of app (charts, editors, games) renders
nothing without it. The kernel has blit/fill primitives and the TTF
rasterizer to build on.

### Beyond the tier (only after the above, or if the user redirects)
Shadow DOM / web components, WebSocket, IndexedDB, WebP/AVIF images,
`<video>`/`<audio>` codecs, HTTP/3, WOFF/WOFF2 web-font decode,
partial-glyph clipping (13D limit), interactive per-element overflow
scrollbars (13D limit), CSS Grid named areas/line-placement (13C
limit), flex height-stretch, `min()`/`max()`/`clamp()` (13B).

## WORKFLOW (the user is strict)
- One feature branch per item, off `browser-webfonts`. Commit there;
  **do NOT merge to `main` without asking** — the user reviews + merges.
- Commit trailer: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
  If git says "Author identity unknown": `git config user.name
  tobyish111 && git config user.email tdude37@me.com`.
- `make` from `/c/CustomOS/tobyOS`; `git` from `/c/CustomOS`. NEVER
  commit `*.log`, `*.img`, `.claude/`, or scratchpad test assets.
- Verify EVERY feature in QEMU with screenshots before committing. Add
  a stage doc `docs/browser-stage13<x>-<name>.md` and a memory update.
- Finish with an EliteDesk real-HW pass when hardware is available
  (serial COM4 @ 38400, AMT off) — stages 12E + 13A–E have not had one.

## BUILD & TEST (gotchas that cost hours — all still true)
```
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"
# TKAPP build (auto-launches the browser for screenshot tests):
touch src/kernel.c   # REQUIRED whenever toggling TKAPP flags (no dep tracking)
make "CC=TMP='C:\t' clang" "HOST_CC=TMP='C:\t' gcc" \
     EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DTKAPP_BOOT -DTKAPP_BROWSER" iso
python -c "print(b'[TKAPP] launching' in open('tobyos.bin','rb').read())"  # must be True
# Rebuild PLAIN (no TKAPP) before handing an ISO to the user:
touch src/kernel.c
make ... EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT" iso
```
- **make clean** after changing the LAYOUT of any widely-included struct
  (no header dep tracking) — e.g. abi structs, `struct proc`.
- FP / anything exchanging doubles with QuickJS builds `-msse -msse2`.
- Test rig lives in the session scratchpad (copy from a prior session's
  scratchpad — see the memory files for the path pattern):
  `websrv.py` (test server, port **8077**; 8000 = Epic Games on this
  host) + `browser_drive.py` (QMP driver: types URLs, boot-retries 3x,
  screendumps per stage). Add a route + a driver `mode` per feature.
- **STALE-WEBSRV TRAP**: `pkill`/edits don't free port 8077 — kill via
  PowerShell `Get-NetTCPConnection -LocalPort 8077 ... Stop-Process`
  before restarting, or curl serves the OLD page (looks like your code
  is broken when it isn't).
- `browser_drive.py wait_quiet` must filter `[hb]` heartbeat lines
  (2 s ticks → "serial quiet" is never true otherwise).
- Real-internet stages must be **marker-anchored** (wait for the page's
  own `[http]` serial line before screenshotting): TCG is ~3x slower
  than wall clock, so wall-clock sleeps let queued QMP keys garble the
  next stage.
- Async-nav screenshot timing: a shot right after typing a URL can
  catch the PREVIOUS page still "Loading..." — that's the harness, not
  a regression. Give net stages settle time / re-run.
- The `-DTKAPP_BOOT` harness boot is intermittently flaky (retry 2–3x;
  the driver already does). Don't single-sample-bisect app code.
- **Footgun (13E)**: in the browser paint function `fd` is literally 0;
  the `sys_gui_fill`/`sys_gui_text` stubs IGNORE their `fd` and use the
  global `&win`. If you add a syscall that needs the real window fd,
  pass `win.fd`, not the paint `fd`.

## DEFINITION OF DONE (per feature)
Screenshot-verified in QEMU (a local test page in `websrv.py`, and a
real site where relevant), regressions clean (CSS torture `/css`,
tables `/tables`, grid `/grid`, `/js2` events, `/spa` Preact), a stage
doc, a memory update, branch pushed and left UNMERGED for review.
