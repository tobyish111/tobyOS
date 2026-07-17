# Pathless-base URL resolution + LINK_MAX (HN was rendering unstyled)

Punch-list closeout, item 3. HN's control-page deltas (blue underlined
titles, no topbar layout, no logo) were not polish items at all — the
page was rendering **without its stylesheet**.

## Root cause — isolated with a local test page first

A minimal `a:link {color:green}` page rendered perfectly, exonerating
the selector engine. The real bug: with a **pathless base URL**
(`https://news.ycombinator.com` — no slash after the origin, exactly
what `-DNAV_URL`/typed navigations produce), `resolve_relative_url`'s
relative branch had `last_slash == origin_end == strlen(base)` and its
inclusive copy loop embedded base's NUL terminator into the output —
every relative URL resolved to the bare origin. HN's
`href="news.css?…"` fetched **the HTML page again** (status 200, parsed
as CSS, zero rules), and the logo/`item?id=` links broke the same way.
Any bare-origin navigation on any site hit this.

Fix: stop the copy at the NUL and synthesize the root slash when the
base has no path. Bases with paths are unchanged.

Also: `LINK_MAX` 128 → 512. Wikipedia saturated the old cap dead-on
("Done - 128 links"); links past it were both unstyled (`a:link` never
matched, `nd->link < 0`) and **unclickable**.

## Verification

- HN renders its real design: orange topbar with logo, stacked
  "Hacker News" + nav row (`.pagetop b{display:block}`), black
  no-underline titles, gray subtext — near-parity with Edge.
  Remaining known gap, documented: `.votearrow` uses
  `background-image: url(triangle.svg)` — CSS background images on
  ordinary elements are still unsupported (the mask/registry machinery
  is ready for a future slice).
- wiki / github / mdn composites unchanged (their bases carry paths);
  home page; ESM 3×.
