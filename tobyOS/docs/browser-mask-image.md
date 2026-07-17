# CSS mask-image (icon rendering)

Modern design systems (Wikipedia Codex, GitHub Primer) don't ship UI
icons as `<img>` — they render them as a solid-colored element clipped by
an SVG applied via `mask-image` / `-webkit-mask-image`. Wikipedia alone
uses **359 `mask-image` + 179 `-webkit-mask-image`** vs 21
`background-image:url`. Without mask support these are blank boxes.

## What it does

`background-color: <color>; mask-image: url(<icon.svg>)` fills the element
box with the color, shaped by the mask image's **alpha** (mask-size:
contain, centered). `background-color: currentColor` — the ubiquitous
Codex pattern — resolves to the element's text `color`.

## Pipeline

- **Parse** (`st_apply` `CP_MASK`): `mask-image` / `-webkit-mask-image`
  register in prop_lookup. The url() value is interned into a page-global
  `g_masks[]` registry (deduped by URL, so shared icons load once), and
  the index is stored in `cstyle.mask` (not inherited). `none` clears it.
- **currentColor**: resolved in the `background-color` apply against
  `st->color`.
- **Fetch/decode** (`load_one_pending_mask`): mirrors the async image
  loader — one transfer in flight, polled from the main loop. `data:`
  masks decode inline. The decoded ARGB's alpha channel becomes the mask
  stencil (`mask_store_alpha`). A finished mask only repaints (icons have
  a CSS size regardless), so no re-layout.
- **Paint** (`DI_RECT` path): the element's bg rect carries the mask
  index; if the mask is loaded, the box is filled with its color × the
  mask alpha (contain-scaled, centered) via `tk_draw_blit_blend`.
  Pending/failed masks paint nothing (a bare colored square would look
  worse than a momentarily-absent icon).

## The bug that took the longest

`url()` parsing must distinguish **quoted** from **unquoted** content. A
`data:` URI legitimately contains quotes in its SVG attributes
(`viewBox='0 0 24 24'`), so stopping at the first quote truncated the
whole URL to `data:image/svg+xml,%3Csvg%20viewBox=` — svg_render then got
a shapeless fragment and produced a 0%-opaque (invisible) mask. Fix: if
the content opens with `"`/`'`, read to the **matching** quote; otherwise
read to `)`. This is general (`mask-image: url("…")` too), not just
data:.

## Verified

`-DMASKTEST` home page: two `<span>`s with `background:#cc0000`/`#1a73e8`
and a data: SVG `<rect>` mask render as a **red square and a blue
square** on screen — the full parse → intern → decode → SVG-render →
alpha-stencil → currentColor-fill path. No panics; home page + ESM 3/3
unaffected.

## Still open: Wikipedia's icons don't show yet (separate layout issues)

The mask feature is correct, but Wikipedia's specific icons remain blank
because of orthogonal **layout** bugs, confirmed by instrumentation:

- Its `.vector-icon` sizes via `width: var(--font-size-medium, 1rem)`,
  where `--font-size-medium` is **self-referentially** defined
  (`--font-size-medium: var(--font-size-medium, 1rem)`); our var resolver
  collapses it to ~4px instead of 16px, and we don't honor the rule's
  `min-width/min-height: 10px`.
- The real icon elements are ~8px inline-blocks that end up laid **only
  in discarded shrink-to-fit measure passes** — their masked rects never
  reach the display list.
- `.vector-icon` (the base class, with the 1×1 placeholder mask) also
  lands on wide text elements (a 207×19 link), so it's not purely an
  icon-box class in this markup.

Their masks *do* decode with real coverage (24–54% opaque), so once the
sizing + measure-pass issues are fixed the icons will fill. Those are CSS
custom-property + inline-block-layout fixes, tracked separately from
mask-image.
