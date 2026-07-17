# Edge-as-oracle visual comparison harness

`tools/compare/compare.sh <url> [slug] [outdir]` renders the same URL in the
tobyOS browser and in headless Microsoft Edge at a matched viewport, and
emits a labeled side-by-side composite plus a coarse structural-difference
score. It exists to *rank* rendering gaps by how different the page actually
looks — a hand-made Wikipedia composite immediately re-ranked the known
defect list (the biggest visual gap was not the one being worked on).

## What it does

1. Builds the ISO with `-DNAV_URL="<url>"` baked in (with the mandatory
   `touch` of main.c/kernel.c — flag changes alone do not recompile).
   Skip with `COMPARE_SKIP_BUILD=1` when the ISO already has the URL.
2. Kills stray QEMU, boots headless with SLIRP networking, waits
   `COMPARE_WAIT` (default 70 s) for the page to settle, QMP-screendumps
   (PNG), kills QEMU. **Stall detection:** the intermittent "TKAPP stall"
   kills a boot at ~3 s with no panic; the driver reads the newest
   `[N ms]` timestamp from the serial log and retries (up to 3×) when it
   is < 15 s.
3. Crops the browser content area — `(91,110)–(810,545)` of the 1280×800
   dump, i.e. **719×435**.
4. Renders the same URL in Edge headless at `--window-size=719,435`
   (`--hide-scrollbars`), the matching viewport.
5. Composes `compare_<slug>.png`: both renders scaled to width 640 on a
   dark canvas, labeled, with the score in the header.
6. Writes `score_<slug>.txt`: **grid-diff** = mean per-cell |luminance
   delta| on a 16×16 grid (0–255, lower = more similar), plus an ASCII
   heat map of the grid so you can see *where* the page differs.

Outputs land in `tools/compare/out/` (gitignored) unless an outdir is
given. Artifacts per slug: `toby_<slug>_full.png` (raw dump),
`toby_<slug>.png` (crop), `edge_<slug>.png`, `compare_<slug>.png`,
`score_<slug>.txt`, `serial_<slug>.log`, `build_<slug>.log`.

## Reading the numbers

The grid-diff is a *structural* signal, not a correctness metric:

- Same layout, different font rasterization → small score (single digits).
- Missing hero image / giant whitespace block / wrong background → the
  affected cells go hot (visible in the heat map rows).
- Do not chase pixel equality across engines; it is meaningless. Use the
  score to rank pages and the heat map to localize, then eyeball the
  composite.

## Caveats (by design)

- **Dynamic pages** (news fronts) rotate content between the two renders;
  compare structure, not stories.
- **UA differences:** Edge sends its native UA — that is the point (it is
  the oracle). tobyOS sends its own UA and may be served different HTML
  (youtube serves ES5 to us; some sites serve mobile markup). Note it
  per-site in the punch list instead of fighting it.
- **Top-of-page only** in v1 — neither side scrolls.
- Re-running Edge is cheap; re-running tobyOS costs a build + ~70 s boot.
  `COMPARE_SKIP_TOBY=1` reuses the existing `toby_<slug>.png` when
  iterating on scoring/compositing only.

## Punch-list workflow

For each site: run the harness, list visible deltas ranked by
area/prominence, merge into `docs/browser-visual-punchlist.md`. Fix top
items in ranked order, using the harness before/after composites as the
verification artifact. Prefer general fixes; park single-site yak-shaves
with a reason.
