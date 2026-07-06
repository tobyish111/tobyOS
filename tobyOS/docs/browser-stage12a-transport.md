# Browser stage 12A: transport depth — chunked + keep-alive + raised caps

Branch `http-chunked-keepalive` (stage-12 scope item A, kernel-side).
Before this, many real sites failed at the transport before the engine
ever saw a byte: `Transfer-Encoding: chunked` was detected and refused
(HTTPE -5), every asset paid a fresh TCP+TLS handshake, and the 96 KiB
page cap truncated any serious article.

## What landed

### Chunked transfer decoding (src/http.c)
- Incremental state machine (`struct dechunk` / `dechunk_feed`):
  wire bytes in as they arrive from the transport, decoded payload
  appended to a geometrically-grown body buffer. Handles chunk sizes
  split across TCP segments, 1-byte chunks, chunk extensions
  (`;ext=...`) and trailer fields after the terminal chunk.
- gzip + chunked compose in the right order: the dechunker reassembles
  the COMPRESSED stream, then the existing kernel-side `puff_gzip`
  inflate runs on the dechunked bytes (dechunk BEFORE inflate).
- `HTTP_F_TRUNCATE` semantics preserved: hitting the caller cap
  mid-stream stops reading, keeps the partial body, and poisons the
  connection (mid-frame — never parked). Without the flag it is
  HTTP_ERR_TOOBIG, as before.
- `HTTP_ERR_CHUNKED` (-5) is no longer returned; the define stays so
  the error table remains index-stable.

### HTTP/1.1 keep-alive (src/http.c)
- New flag `HTTP_F_KEEPALIVE`: speaks HTTP/1.1 with
  `Connection: keep-alive`; without it requests remain exactly the old
  HTTP/1.0 + `Connection: close` (wget/pkg/SYS_HTTP_GET contract
  untouched). `SYS_HTTP_FETCH` (the browser path) sets it.
- Per-(host, port, scheme) connection cache: `KEEP_MAX` 4 parked
  connections, LRU eviction, `KEEP_IDLE_MS` 8 s idle sweep on every
  lookup (servers drop idle conns after a few seconds; better to close
  here than discover a corpse mid-request). Plain-TCP entries are also
  liveness-checked via `tcp_poll_flags` (FIN/RST while parked).
- A connection is parked ONLY when the response was consumed exactly
  to its framing boundary: HTTP/1.1, no `Connection: close`, body
  complete per Content-Length or terminal chunk, not truncated, no
  over-read, no FIN seen. Everything else closes as before.
- Stale-reuse recovery: a reused connection that dies before yielding
  a single response byte (send fails, EOF, reset, timeout at 0 bytes)
  is closed and the request retried ONCE on a fresh connection — GET
  is idempotent. TLS conns can't be pre-probed (opaque), this covers
  them.
- 1xx interim responses (e.g. Cloudflare `103 Early Hints`, which only
  appear once you speak 1.1) are parsed and skipped, up to 4 blocks.
- 204/304 are treated as body-less by definition.
- `http_keepalive_flush()` / `http_keepalive_stats()` exposed for
  tests and future link-down handling.
- TCP_MAX_CONNS 12 → 16 (tcp.c): 4 parked conns + active fetch +
  listeners + TIME_WAIT remnants need the headroom.

### Raised caps (browser + kernel)
- `RAW_CAP` 96 K → 512 K (page HTML; kernel truncates past it).
  `raw[]` is inline in `struct tab` → 6 tabs × 512 K = 3 MiB BSS.
- `RENDER_CAP` 128 K → 576 K (must exceed RAW_CAP so view-source of a
  max-size page keeps every character).
- eng pools scaled to match (still per-tab heap, now ~8 MiB/tab):
  NODE 12288→32768, ATTR 12288→32768, TPOOL 224 K→768 K,
  PART 6144→24576, DECL 8192→32768, RULE 2048→8192,
  CSSPOOL 96 K→320 K, ITEM 20480→49152.
- Stylesheets: `SHEET_MAX` 3 → 6, `SHEET_FETCH_CAP` 160 K → 256 K.
- External `<script src>` cap `JS_SRC_CAP` 192 K → 384 K.

## Verified in QEMU (screenshots in the session scratchpad)

Local (websrv.py on 8077, hand-rolled awkward chunk framing):
- `/chunked` — 1-byte chunk, sizes split across segments, extension,
  trailer: renders styled and complete (`[http] dechunked 898 bytes`).
- `/chunkedgz` — gzip sent chunked: `dechunked 571` → `gunzip 571 ->
  908`, renders complete. Compose order proven.
- `/big` — 211 KiB Content-Length page: `body=211130 bytes`, renders
  and scrolls (the old cap truncated at 96 K).
- `/assets` — page + linked sheet + 3 images, same host: serial shows
  `keep-alive: reuse ... (reused=4 handshakes=4)` — one handshake
  bought the page AND all four asset fetches.
- `/css` torture page regression: identical to the stage-7 baseline.

Real internet (QEMU SLIRP, live sites):
- google.com — 301 → www.google.com, then `dechunked 30125` →
  `gunzip 30125 -> 85453`: a real chunked+gzip site decodes end-to-end
  over TLS 1.3 and renders (logo, search box, links). Ladder target 1.
- en.wikipedia.org/wiki/QuickJS — `dechunked 67490` → `gunzip ->
  469183`: a 469 K article arrives UNTRUNCATED (old cap kept 96 K).
  Its whole asset battery (skin JS/CSS, SVG icons, cross-host
  auth.wikimedia.org redirect) ran at `reused=32 handshakes=4`.
- en.wikipedia.org/wiki/Cat — `gunzip 248041 -> 524288 (truncated)`:
  the 1.6 MiB article fills the full new 512 K cap; the TOC renders
  through all 15 sections (the 96 K cap used to cut it off).
- mojeek search results — regression clean, assets reused.
- Whole-session tally: 46 HTTP requests over 6 TCP+TLS handshakes
  (`reused=40 handshakes=6`).

Found in the field: Wikipedia's combined Vector-skin `load.php`
stylesheet URL carries a path up to ~460 chars — HTTP_MAX_PATH_LEN 384
rejected it (`bad URL`) and wiki pages rendered unstyled. Raised to
496 (`struct http_url` layout change ⇒ clean rebuild).

## Gotchas found/confirmed this round
- Python http.server only does keep-alive + chunked when
  `protocol_version = "HTTP/1.1"` is set on the handler class; use
  `ThreadingHTTPServer` or a kept-alive connection blocks every other
  client.
- The kernel heartbeat (`[hb]`, 2 s) means "serial went quiet" is
  never true — the QMP driver's wait_quiet must filter those lines.
- Wikipedia's full Cat article is ~1.6 MiB of HTML; 512 K covers the
  lead + first sections. "Untruncated" is provable with any article
  under 512 K (most are). Going further is a cap decision, not a
  transport gap.

## Not in this branch (later stage-12 items)
Real `<table>` layout (B, `browser-tables`), flexbox (C,
`browser-flexbox`), the async-fetch kernel ABI (D, `kernel-async-http`
— the UI still freezes during fetches), position/SVG/popstate/
persistent storage (E, reach).
