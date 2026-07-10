# TLS 1.3 crypto core extraction (HTTP/3 groundwork)

Branch `tls-engine`, stacked on main (post the 2nd stage-13 merge).
The first slice of HTTP/3: pull the transport-independent parts of the
TLS 1.3 implementation out of `tls.c` into a reusable `tls13` module so
a future QUIC crypto layer can share them. Behavior-identical — a pure
move, verified against a live TLS 1.3 handshake.

## Why this first
QUIC (the transport under HTTP/3) does not use the TLS record layer at
all: it carries the TLS 1.3 handshake *messages* in QUIC CRYPTO frames
and protects packets with its own header. But it uses the **same TLS
1.3 key schedule** (RFC 8446 §7.1 — HKDF-Expand-Label, Derive-Secret,
the handshake/application traffic secrets) and the **same AEAD**
(ChaCha20-Poly1305) that tobyOS already implements for TLS-over-TCP.
Until now those lived tangled inside `tls.c` next to the TCP record
framing. Extracting them creates the seam QUIC needs without a
rewrite, and does it as a low-risk mechanical move that leaves the
proven TLS-over-TCP path byte-for-byte the same.

## What shipped
- **`include/tobyos/tls13.h` + `src/tls13.c`** — the
  transport-independent TLS 1.3 core:
  - HKDF: `hkdf_extract`, `hkdf_expand_label`, `derive_secret`.
  - Key schedule: `derive_handshake_keys` (early → derived →
    handshake secret → c/s handshake traffic keys+IVs),
    `derive_app_keys` (→ master secret → c/s application keys+IVs),
    `compute_finished` (the Finished HMAC).
  - AEAD: `tls_aead_encrypt` / `tls_aead_decrypt` — the RFC 8439 IETF
    ChaCha20-Poly1305 construction (composed from Monocypher's
    ChaCha20-IETF + Poly1305, since Monocypher's one-shot AEAD is the
    XChaCha 24-byte-nonce variant TLS can't use).
  - Wire helpers `tls_put_u16/u24` / `tls_get_u16/u24` as inline
    header functions, shared by the record layer and QUIC framing.
  All pure, no I/O, no global state, single-suite
  (`TLS_CHACHA20_POLY1305_SHA256`).
- **`src/tls.c`** — the moved definitions are deleted and it now
  `#include <tobyos/tls13.h>`. Its record I/O (`tls_send_record`,
  `tls_read_record`, `tls_send_encrypted`, `tls_decrypt_record`), the
  handshake orchestration (`tls_do_handshake`), the ClientHello
  builder / ServerHello parser, and the stage-13H certificate chain +
  CertificateVerify validation all stay put and call into `tls13`.
  The short `get_u16`/`put_u16` names it has always used are now
  `#define` aliases onto the `tls_*` inlines — so every call site is
  untouched. This was a pure move: no logic changed.
- **Makefile** — `src/tls13.c` builds with the default rule (it needs
  only Monocypher + `sec.h` + klibc, no BearSSL), added next to
  `tls.c` in the kernel source list.

## Verified (QEMU)
- **`wss://echo.websocket.org` over TLS 1.3** — the full handshake
  runs through the extracted key schedule + AEAD and the stage-13H
  certificate validation: serial shows `[tls] certificate chain +
  CertificateVerify OK` and `[tls] handshake complete (TLS 1.3,
  ChaCha20-Poly1305)`, the WebSocket opens, and the echoed message
  renders. That single path exercises every moved function
  (ClientHello AEAD-nothing, ServerHello key derivation, encrypted
  handshake-record decrypt, Finished MAC, app-key derivation,
  app-record encrypt/decrypt) plus cert validation.
- Symbol linkage checked with `nm`: every key-schedule/AEAD symbol
  `tls.o` references is defined by `tls13.o` (not just an `ld -r`
  closure — the stage-13H lesson).

## What's next (HTTP/3 slices)
- **Slice 2** — the QUIC transport: UDP + QUIC long/short-header
  packets, Initial-secret derivation (fixed salt → HKDF, reusing this
  module), packet protection with `tls_aead_*`, CRYPTO/STREAM/ACK
  frames, and the handshake state machine driving the *same*
  ClientHello/ServerHello/Certificate/Finished logic — which the next
  slice will also lift out of `tls.c` into `tls13` (message layer)
  once QUIC needs it.
- **Slice 3** — HTTP/3 framing (HEADERS/DATA) + QPACK over QUIC
  streams, wired into `http.c` behind an Alt-Svc / explicit-h3 probe,
  with the h2/h1.1 fallback ladder intact.

## v1 note
This slice moves the cryptographic core only; the TLS *message*
builders/parsers and cert validation remain in `tls.c` for now (they
move when the QUIC handshake consumes them). No behavior change, no
new capability — it's the foundation the QUIC work builds on.
