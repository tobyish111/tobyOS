# QUIC crypto primitives (HTTP/3 slice 2)

Branch `quic-crypto`, stacked on `tls-engine`. The cryptographic
foundation of the QUIC transport (RFC 9000/9001): variable-length
integers, Initial-packet key derivation, header protection, and packet
protection (AEAD). Built on the extracted TLS 1.3 key schedule
(`tls13.h`) plus AES-128-GCM. Verified deterministically against the
RFC 9001 Appendix A test vectors — no live HTTP/3 server needed.

## Why AES-128-GCM (a new cipher for tobyOS)
tobyOS's TLS is ChaCha20-Poly1305 only, but QUIC **Initial** packets
are *always* AES-128-GCM regardless of the negotiated suite (RFC 9001
§5.2), and header protection uses AES-ECB. So QUIC can't reuse the
existing AEAD — it needs AES. Rather than hand-roll a security
primitive, this vendors the AES + GCM subset of BearSSL (already the
trust store for stage-13H): `aes_ct.c` + `aes_ct_enc.c` +
`aes_ct_ctr.c` (constant-time AES) and `aead/gcm.c` (GCM over the
already-vendored `ghash_ctmul`). ~4 files, compiles freestanding with
the existing BearSSL shim.

## What shipped
- **`src/quic_crypto.c` + `include/tobyos/quic_crypto.h`**:
  - **Varints** (RFC 9000 §16): `quic_varint_decode/encode`, the
    1/2/4/8-byte self-describing integers used everywhere in QUIC.
  - **Initial keys** (RFC 9001 §5.2): `quic_initial_keys(dcid, len,
    is_client, key, iv, hp)` — `HKDF-Extract(salt, dcid)` →
    `Expand-Label("client in"/"server in")` → `"quic key"`/`"quic
    iv"`/`"quic hp"`, all via the `tls13` HKDF. AES-128, so key/hp are
    16 bytes, iv 12.
  - **Header protection** (RFC 9001 §5.4): `quic_hp_mask(hp, sample,
    mask)` = `AES-ECB(hp, sample)[:5]`. BearSSL's AES exposes CTR, not
    a raw ECB block, so this runs CTR with the counter block set to
    the 16-byte sample (iv = sample[0:12], cc = sample[12:16]) over 16
    zero bytes — the keystream is exactly `AES-ECB(sample)`.
  - **Packet protection** (RFC 9001 §5.3): `quic_packet_nonce` (iv XOR
    right-aligned packet number) and `quic_aead_encrypt/decrypt`
    (AES-128-GCM over the header as AAD, via BearSSL `br_gcm`).
- **`quic_crypto_selftest()`** — gated behind `-DQUIC_SELFTEST`, run
  from kernel boot; prints `[quic] ...` lines.

## Verified (QEMU, headless `-DQUIC_SELFTEST` boot)
9/9 checks pass — no network, fully deterministic:
- **RFC 9001 A.1** — the derived client + server `key`/`iv`/`hp` match
  the published Initial-keys vector exactly (6 checks). This exercises
  the whole HKDF path (extract → `client in`/`server in` →
  `quic key/iv/hp`).
- **RFC 9001 A.2** — `AES-ECB(client hp, sample)[:5]` equals the
  published header-protection mask `437b9aec36`.
- **AES-128-GCM KAT** — the NIST all-zero vector (key=0, iv=0, empty
  AAD/PT) produces tag `58e2fcce…455a`, confirming the vendored GCM.
- **AEAD round-trip** — encrypt-then-decrypt with AAD verifies the tag
  and recovers the plaintext.

Serial ends `[quic] crypto self-test: 9/9 ALL PASS`.

### Gotcha that cost the first run
The self-test initially reported the six A.1 key checks FAIL while the
mask + GCM checks passed. Cause: a wrong **Initial salt** constant —
the real QUIC v1 salt is `0x38762cf7f55934b34d179ae6a4c80cadccbb7f0a`
(only the first 4 bytes, `38762cf7`, matched the wrong value I'd
started with). The mask check passed regardless because it fed the
*expected* hp key directly; only the derived keys depended on the
salt. Ground truth for the fix came from the `aioquic` reference
implementation (a model-summarized RFC fetch had garbled the hex).

## What's next (HTTP/3 slices)
- **Slice 3** — QUIC packets + frames: UDP, long/short-header packet
  assembly using these primitives, CRYPTO/ACK/STREAM frames, and the
  handshake state machine driving the TLS 1.3 messages (which move
  from `tls.c` into a `tls13` message layer when QUIC consumes them).
- **Slice 4** — HTTP/3 framing (HEADERS/DATA) + QPACK over QUIC
  streams, into `http.c` behind an Alt-Svc / explicit-h3 probe with
  the h2/h1.1 fallback ladder intact.

## v1 scope
Crypto + varint only: no UDP, no packet/frame layer, no state machine.
AES-128-GCM (Initial + any AES suite); ChaCha20-Poly1305 1-RTT keys
would reuse the existing `tls13` AEAD when the frame layer lands.
