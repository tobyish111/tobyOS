# ChaCha20-Poly1305 for QUIC Handshake + 1-RTT (HTTP/3 slice 5e)

Branch `quic-chacha20`, stacked on `browser-h3-altsvc` (slice 5d).
Completes the QUIC cipher story: the Handshake and 1-RTT encryption
levels now use whichever TLS 1.3 suite the ServerHello names —
AES-128-GCM (0x1301) **or ChaCha20-Poly1305 (0x1303)**. Previously a
server that selected ChaCha20 was refused at ServerHello; now the h3
fetch completes over either.

## What shipped
- **ChaCha20 header protection** (`src/quic_crypto.c`,
  `quic_hp_mask_chacha`, RFC 9001 §5.4.4): the first 4 bytes of the
  sample are the ChaCha20 block counter (little-endian) and the
  remaining 12 the nonce; the mask is ChaCha20 applied to 5 zero bytes,
  via Monocypher's `crypto_chacha20_ietf`. (AES header protection stays
  `quic_hp_mask` = AES-ECB.)
- **AEAD dispatch** (`src/quic_packet.c`): a `QUIC_AEAD_*` selector
  (`AES128GCM` / `CHACHA20`) now threads through `quic_build_long` /
  `quic_open_long` / `quic_build_short` / `quic_open_short`. Small
  `pkt_seal` / `pkt_open` / `pkt_hp` dispatchers pick AES-128-GCM
  (`quic_aead_*` + AES-ECB HP) or ChaCha20-Poly1305 (the existing
  `tls_aead_*` IETF construction from `tls13.c` + ChaCha20 HP). The
  Initial level is hard-wired to AES-128-GCM (RFC 9001 §5.2), so
  `quic_build_initial` / `quic_open_initial` are unchanged wrappers and
  the key/hp buffers are now pointers (16 bytes for AES, 32 for
  ChaCha20).
- **Suite-aware key derivation** (`src/quic_conn.c`): the ServerHello
  handler accepts 0x1301 **and** 0x1303, sets `aead` + the key/hp
  lengths (16/16 or 32/32), and derives the Handshake and 1-RTT
  `quic key`/`quic hp` at that length; every Handshake/1-RTT
  build/open call passes `aead`.

## Verified
1. **Deterministic** — `quic_packet_selftest` is now 6/6: the four
   existing AES checks plus a ChaCha20 1-RTT short-header packet built
   by tobyOS whose whole protected form is **byte-identical (SHA-256)
   to the Python `cryptography` reference** (ChaCha20-Poly1305 body +
   ChaCha20 header protection), and its round-trip through
   `quic_open_short`. This proves both the ChaCha20 AEAD and the
   ChaCha20 header protection against an independent implementation.
   Crypto 9/9, client-hello 3/3, http3 3/3, altsvc 5/5 unchanged.
2. **Live** — the aioquic h3 rig forced to `CHACHA20_POLY1305_SHA256`:
   tobyOS logs `ServerHello OK: cipher=0x1303`, `derived Handshake keys
   (suite=0x1303 …)`, validates the certificate over the
   ChaCha20-protected Handshake flight, and returns `HTTP/3 GET
   COMPLETE: status=200`; the server logs `PASS: served GET /`. The
   full handshake + GET runs over ChaCha20 end to end.
3. **No AES regression** — with the default AES-128-GCM server the
   small fetch and the 923 KB `/big` fetch both still complete
   (`suite=0x1301`, `LARGE RESPONSE OVER HTTP/3`).

### Gotcha (re-hit)
The `/big` fetch briefly failed with `HTTP_ERR_DNS` — not a code bug:
`src/dns.c` holds the `#ifdef TLS_TEST_CA` test-host seam, and because
`EXTRA_CFLAGS` changes don't trigger recompiles, a stale `dns.o` (built
earlier without the flag) shipped without the seam, so `tobyos.test`
went to real DNS → NXDOMAIN. Touch `src/dns.c` too on `-DTLS_TEST_CA`
builds (added to the standard QUIC touch list:
`kernel.c tls_trust.c quic_conn.c udp.c dns.c`).

## What's next
- `MAX_STREAM_DATA` / `MAX_DATA` flow-control updates to lift the 1 MiB
  h3 response cap (large single responses currently fall back to
  h2/h1.1).
- Persisting the Alt-Svc cache across reboots.
- openh264 High-profile / libgav1 AVIF (unblocked by the C++ runtime),
  and the owed EliteDesk real-hardware pass.

## v1 scope
Both TLS 1.3 suites for Handshake + 1-RTT; Initial is always
AES-128-GCM per spec. No key update (`0x1301`/`0x1303` fixed for the
connection's lifetime), no other suites.
