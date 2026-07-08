# Browser stage 13G — brotli decode (Content-Encoding: br)

Branch `browser-brotli-h2`, stacked on `browser-observers` (13F). This is
a **kernel-side transport** feature: it makes `Content-Encoding: br`
bodies decode transparently, exactly like the existing gzip path. Brotli
is now more common than gzip on CDNs (Cloudflare, Google, and most of the
modern web serve `br` by default), so before this a large fraction of
real responses arrived as unusable binary.

The branch is named `-h2` because the tier pairs brotli with HTTP/2;
**HTTP/2 is deferred to a follow-up** (see "Remaining" below). Brotli
went first, as planned.

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

## Remaining in 13G — HTTP/2
`src/http2.c` is a 620-line stub: frame layer (SETTINGS / WINDOW_UPDATE /
PING / DATA / HEADERS / GOAWAY / RST_STREAM), a *simplified* HPACK
decoder (static table only — no Huffman, no dynamic table), and
`http2_connect`/stream bookkeeping. To make it usable on the real web it
needs: (1) **ALPN over TLS** — advertise `h2`, detect the server's
selection, and route the fetch to h2 when chosen (real servers do not do
cleartext h2c); (2) **full HPACK** — Huffman string decode + the dynamic
table, so response headers (content-type/length/encoding) are actually
read, not skipped; (3) DATA-frame body reassembly wired into
`http_get_opt`/the async worker; (4) flow-control windows. This is the
harder half and a session of its own.
