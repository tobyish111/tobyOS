# Browser stage 13G — brotli decode + HTTP/2

Branch `browser-brotli-h2`, stacked on `browser-observers` (13F). Two
**kernel-side transport** features: brotli `Content-Encoding: br` decode,
and an HTTP/2 client (ALPN over TLS + HPACK). Both are transparent to the
browser — the render engine is unchanged.

## What shipped
- A real brotli (RFC 7932) decoder, kernel-side, via the **vendored
  Google reference decoder v1.0.9** (`third_party/brotli/`, amalgamated
  into one TU by `src/brotli.c`). This includes brotli's fixed ~122 KB
  static dictionary + word transforms — both ship as C source in the
  reference (`common/dictionary.c`), so nothing is embedded by hand.
- `src/http.c` now advertises `Accept-Encoding: gzip, br`, parses
  `Content-Encoding: br` (→ `HTTP_ENC_BR`), and decompresses a `br` body
  into the caller's capped buffer via `brotli_decompress()` — the same
  transparent, truncate-at-cap path the gzip/`puff` decode uses. The
  browser (async worker → `http_get_follow` → `http_get_opt`) gets it for
  free; it never sees the compressed bytes.
- **Bonus unlocked:** WOFF2 web fonts are brotli-plus-glyph-transforms,
  so a WOFF2 decoder can now build on this (a 13E limit). Not wired yet.

## How it's built
- `third_party/brotli/` holds the decode-only subset of v1.0.9: `common/`
  (constants, context, dictionary, transform, platform, version) +
  `dec/` (bit_reader, huffman, prefix, state, decode) + the public
  `include/brotli/` headers. Chosen over master because v1.0.9's decoder
  is self-contained (no shared-dictionary weaving) and much smaller to
  port.
- The kernel is freestanding (`-ffreestanding`, no hosted libc). The only
  hosted headers the reference pulls are `<string.h>` and `<stdlib.h>`
  (endian/assert/stdio are all guarded out for `x86_64-elf`). Both are
  satisfied by tiny shims under `third_party/brotli/shim/` that map onto
  the kernel's `klibc` (`memcpy` etc.) and `heap` (`kmalloc`/`kfree`).
- `src/brotli.c` `#include`s the reference `.c` files in dependency order
  (they carry no colliding file-scope statics) and exposes one function,
  `brotli_decompress(dest, *destlen, src, srclen)`, with the same
  `OK`/`TRUNC`/`ERR` convention as `puff`. It drives the reference's
  streaming API (`BrotliDecoderDecompressStream`) so a body larger than
  the output cap truncates cleanly (like gzip's `PUFF_TRUNC`) instead of
  failing. Allocation goes through `kmalloc`/`kfree` adapters.
- Makefile: `src/brotli.o` gets a dedicated rule adding
  `-Ithird_party/brotli/shim -Ithird_party/brotli/include` (shim first)
  and `-Wno-*` to quiet the vendored source under `-Wall -Wextra`.

## Verified (QEMU)
- Local `/brotli` route (websrv serves a page pre-compressed with the
  `brotli` CLI, `Content-Encoding: br`): serial shows
  `[http] unbrotli 743 -> 1881` — decoded to exactly the original size —
  and the page renders fully, including the bottom marker "if you can
  read this, the whole stream decoded." The dictionary-heavy English text
  (fox / government / internet) reconstructs intact, proving dictionary
  references + word transforms resolve.
- gzip regression clean: `[http] gunzip 571 -> 908`, page renders.
- CSS torture regression clean.
- **Real site over TLS**: `brotlinet` driver mode → live `www.cloudflare.com`.
  Cloudflare served the page and its SVG assets `Content-Encoding: br`
  over TLS 1.3 with keep-alive reuse; serial shows a stream of
  `[http] unbrotli 1683 -> 3700`, `646 -> 1269`, `1538 -> 3742`,
  `890 -> 2547` … and the homepage renders (nav, the "powering 20% of the
  Internet" hero headline, "Region: Earth"). Before this the same
  responses were unusable binary.

## HTTP/2 (the tier's other half)
Nearly every major site is h2 (though they all fall back to h1.1, so this
is a completeness/perf feature, not a blocker). Implemented as a
self-contained single-request client with a bulletproof h1.1 fallback:

- **ALPN over TLS** (`src/tls.c`): `tls_connect(..., offer_h2, ...)`
  advertises ALPN `["h2","http/1.1"]` in the ClientHello (RFC 7301) only
  when the caller can speak h2 — so the proven h1.1 ClientHello is
  byte-identical when `offer_h2` is 0. The server's choice is parsed out
  of EncryptedExtensions and exposed via `tls_alpn()`.
- **`src/http2.c`** (rewritten): over an h2-selected TLS conn it sends the
  connection preface + SETTINGS (push off, large initial window) + a big
  connection WINDOW_UPDATE, then one HEADERS request on stream 1, and
  reassembles the HEADERS(+CONTINUATION)/DATA response into a
  `struct http_response`. **HPACK is complete**: static table, dynamic
  table (insert / evict / size-update), and the RFC 7541 Huffman code
  (table generated from the RFC, `src/hpack_huff_table.h`). Bodies are
  gzip/brotli-decompressed here so the result matches the h1 path.
- **`src/http.c`**: on a fresh HTTPS connect it offers h2; if the server
  selected `h2` it runs `http2_fetch()` and returns. On **any** h2 failure
  it sets a flag and re-enters the loop for a fresh HTTP/1.1 connection
  (GET is idempotent) — so h1.1 is always the safety net and existing
  sites can never regress. h2 connections are not parked in the h1
  keep-alive cache (v1).

Verified in QEMU: `https://www.google.com` and `https://http2.golang.org`
render fully — serial shows `[tls] ALPN selected: h2`, HPACK-decoded
headers (`[h2] 200; type="text/html; charset=UTF-8"`), gzip bodies
decompressed (`[h2] gunzip 30099 -> 85323`), CSS/PNG assets over h2, and a
302 redirect. The h1.1 path is unregressed (local plain-http still serves
`[http] unbrotli 743 -> 1881`, no h2 markers).

v1 limits: one request per h2 connection (no multiplexing / reuse), no
server push (disabled + ignored), no h2 cookie-jar integration, no
trailers.
