# data: image URIs

## Symptom

Inline images (`<img src="data:image/png;base64,…">`), common on modern
sites (icons, tracking pixels, small logos), never rendered: the `T_IMG`
collect case explicitly skipped any `data:` src, so no image record was
ever created.

## Design

Decode at **collect time** — there is no network transfer, so the async
fetch pump is the wrong tool. `collect_node`'s `T_IMG` case:

- Detects `data:` case-insensitively on the (possibly truncated) 512-byte
  src copy — only the first 5 bytes matter.
- Reads the **full** URI straight from the DOM attribute pool via
  `node_attr` (a data: URI carries the whole image; it does not fit the
  `struct img src[512]` field, which now just holds the marker `"data:"`
  and is never fetched — `state` never stays 0).
- `data_uri_decode_image` parses `data:[<mediatype>][;base64],<payload>`,
  base64-decodes (whitespace/padding tolerant) or percent-decodes the
  payload, caps the decoded size at `IMG_FETCH_CAP` (1 MiB, mirroring the
  network path), and hands the bytes to the shared decoder.
- The raster-vs-SVG decode block that lived inline in
  `load_one_pending_image` is factored into `img_decode_bytes()` (libtoby
  raster formats first, `svg_render` fallback, `IMG_MAX_DIM` guard) and
  shared by both paths — so `data:image/svg+xml,…` routes through the SVG
  renderer for free.
- Decode failure (malformed URI, no comma, oversized, undecodable bytes)
  → `state = -1`, the normal broken-image path. The record is still
  created once per node (`nd->img` guard), so light re-collects don't
  re-decode.

## Verification

Gated `-DDATAURI_TEST` home page: a base64 PNG (4 colored quadrants), a
percent-encoded SVG (`data:image/svg+xml,%3Csvg…`), and a malformed URI
(no comma). On-screen: both images render with correct colors/geometry,
the malformed one shows as broken, no crash.

Regressions: wikipedia renders unchanged (its images are all fetched),
home page unchanged, ESM self-test ALL PASS.

## Limits

- `<img>` only: CSS `background-image: url(data:…)` and `srcset` still
  skip data: URIs.
- Synchronous decode at collect time: a page with many megabyte-scale
  inline images pays the decode during collect (bounded by IMG_FETCH_CAP
  each, IMG_MAX records).
