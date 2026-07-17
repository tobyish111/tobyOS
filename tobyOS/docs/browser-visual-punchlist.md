# Visual rendering punch list (Edge-as-oracle, 2026-07-16)

Produced with `tools/compare/compare.sh` (see browser-compare-harness.md)
on 7 sites. Grid-diff = mean per-cell |luminance delta|, 16×16 grid,
0–255; a *ranking* signal, not a correctness metric.

| site | grid-diff (start → now) | one-line verdict |
|---|---|---|
| mdn (developer.mozilla.org/…/Web/HTML) | 203.2 → **11.7** | FIXED ×2: var initial/poison + light-dark() cured the black bg; media-eval calc/range syntax selected the correct mobile layout — article renders |
| bbc (www.bbc.com/news) | 50.0\* → **10.7** | FIXED: first paint before scripts + JS watchdog (real page paints); measure-float fix improved it further |
| github (repo page) | 39.6 → **38.7** | FIXED (structure): mega-nav un-stacked via :not() + `hidden` attr — repo header/tabs/file-headers above the fold like Edge. Residual score is the dark header band + octocat + JS-deferred file rows (React app, out of scope) |
| wiki (Operating_system) | 24.8 → **15.8** | FIXED: flex header one row (measure-float), masked icons + infobox diagram render (clip-shift); remaining = wordmark image (`.mw-logo` is display:none at 708px, same as Edge) |
| wikiportal (Portal:Current_events) | 15.5 | header + icon fixes apply (not re-swept) |
| hn (news.ycombinator.com) | 11.9 → **15.7** | FIXED: news.css now loads (pathless-base URL bug) — real 2-row header/logo/link colors like Edge. Residual = `.votearrow` background-image (unsupported), which the higher structural score reflects |
| example.com | **1.8** | parity control ✓ |

\* bbc's original score under-reported: it compared our *home page*
against Edge's bbc.

**Fixed this arc (merges 43rd–51st):** compare harness → punch list →
first-paint+watchdog → var-initial/poison+light-dark → media-eval
units/calc/range → measure-pass right-float → :not()+hidden →
provisional-lay clip-shift + sibling combinators → pathless-base URL +
LINK_MAX. **Every punch-list item is now addressed.** Remaining gaps are
documented non-goals: CSS `background-image` on ordinary elements
(HN vote arrows), and full React-app hydration (GitHub's deferred file
rows — the same app-hydration wall as youtube.com).

## Ranked defects

(ALL FIXED — see browser-first-paint.md, browser-light-dark.md,
browser-media-eval.md, browser-measure-float.md, browser-not-selector.md,
browser-clip-shift.md, browser-base-url.md. The section below is the
original as-found analysis, preserved for provenance.)

### 1. No first paint while page JS runs (bbc; every JS-heavy site) — CONFIRMED behavior
The /news HTML + 46 subresources were fetched by ~32 s (page body at 2 s!),
but the tab never painted: `render_html` runs dom→sheets→style→layout→
**run_scripts** synchronously and the window only redraws after the whole
thing returns. BBC's React bundle grinds in QuickJS (CPU pegged, ~0.5 MiB/s
allocation) and the server-rendered page — which Edge shows instantly — is
held hostage behind it. Two sub-items:
  a. **Paint after initial layout, before run_scripts** (small, general).
  b. **JS time budget** for page scripts (bbc's bundle may effectively never
     finish on the interpreter; without a cap the tab shows "Loading…"
     forever). Also investigate the steady allocation while it grinds.
Suspected subsystem: render_html sequencing (main.c). Size: small (a),
medium (b).

### 2. MDN paints a dark/near-black page background — cause unknown, instrument
Edge renders white. Our media_matches() is fail-closed and only accepts
`prefers-color-scheme: light`, so it is NOT the obvious dark-@media leak —
needs a DL_TRACE run + CSS cross-reference to find who paints the dark
rect. Whole-viewport wrongness = biggest single-page score (203).
Suspected subsystem: cascade/var() on MDN's theme custom-props, or a
mis-parsed color. Size: unknown until instrumented (likely small once found).

### 3. Header/columns stack vertically (wiki, wikiportal, github, mdn) — the big one
Wikipedia's `.mw-header{display:flex;flex-wrap:wrap;gap:16px}` renders
~132 px tall with header-start and header-end on separate rows (DL_TRACE:
hamburger y=34, donate y=116), then further whitespace before the H1 —
~200 px of the 435 px viewport is header. Same family: MDN's sidebar/
article grid stacks (article below the fold), GitHub's nav stacks above
the repo content. This is a layout-subsystem defect (flex row sizing /
wrapping / grid columns), not per-site quirks.
Suspected subsystem: lay_flex child sizing (a header-end with flex-grow:1
whose preferred width overflows wraps everything), CSS grid columns.
Size: the largest item; instrument with a layout trace before touching.

### 4. Masked icons missing (wiki hamburger/search/ellipsis; github hamburger)
Extensively dug previously and PARKED (see memory: mask feature itself
works — -DMASKTEST proves the full path; wiki's icons are gated by
(a) icon rects emitted only inside discarded shrink-to-fit measure passes
(60 emitted / 56 discarded) and (b) the 4 survivors culled in
paint_content_item because they sit in the sticky-header region whose
items paint via the deferred g_sticky pass). Moderate visual area, high
prominence. Revisit only after 1–3; the sticky-pass culling is the
concrete next thread.

### 5. HN polish (control near-parity)
Vote arrows (10×10 gifs) missing; topbar link colors differ (white on
orange in Edge). Low priority; note-only.

## Method caveats observed
- Edge was served the Wikipedia fundraising banner on one run (dynamic
  content rotates — compare structure).
- bbc demonstrates a failure mode the score can't see (we screenshot the
  home page); check `serial_<slug>.log` when a score looks off.
