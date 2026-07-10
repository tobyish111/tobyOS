# QUIC packet + frame layer (HTTP/3 slice 3)

Branch `quic-packet`, stacked on `browser-mp4` (the current chain tip).
The wire-format layer of the QUIC transport: builds and opens
protected Initial packets (long header + packet protection + header
protection) and encodes/parses the handshake frames, on top of the
slice-2 crypto primitives. Verified byte-for-byte against the
`aioquic` reference — no live server.

## What shipped
- **`src/quic_packet.c` + `include/tobyos/quic_packet.h`**:
  - **Frames**: `quic_frame_crypto()` writes a CRYPTO frame (type
    0x06, varint offset + length + data); `quic_frame_parse()` decodes
    the next frame — CRYPTO, ACK (largest + delay + range walk),
    PADDING (runs collapse), PING.
  - **`quic_build_initial()`** — assembles a long-header Initial
    packet: first byte (`0xc0 | pn_len-1`), version 1, DCID, SCID,
    varint token, varint length, packet number; then applies **packet
    protection** (`quic_aead_encrypt` — AES-128-GCM over the header as
    AAD, tag appended) and **header protection** (`quic_hp_mask` sample
    at `pn_offset + 4`, XOR the first byte's low 4 bits and the packet
    number bytes).
  - **`quic_open_initial()`** — the inverse: removes header protection
    (recovers the first byte + packet number from the sample mask),
    then decrypts and verifies the payload, returning the frames
    region and packet number.

## Verified (QEMU, headless `-DQUIC_SELFTEST` boot)
`quic_packet_selftest()` — 4/4 PASS, deterministic:
1. A client Initial packet (DCID = the RFC 9001 test CID, one CRYPTO
   frame of 32 bytes, pn = 2) builds to exactly 72 bytes.
2. Its protected bytes **match the aioquic reference exactly** —
   compared via SHA-256 of the whole packet (`26e5dfae…75d0`). This
   proves the header layout, length/pn encoding, packet protection,
   and header protection are all interoperable with a mature QUIC
   implementation.
3. `quic_open_initial()` round-trips it: header protection removed,
   payload decrypted, packet number (2) and payload recovered.
4. The recovered CRYPTO frame parses back to offset 0, length 32, and
   the original data.

Serial ends `[quicpkt] packet self-test: 4/4 ALL PASS`.

## What's next (HTTP/3 slices)
- **Slice 4** — the connection: a UDP socket, the QUIC handshake state
  machine driving the TLS 1.3 messages (ClientHello → ServerHello →
  EncryptedExtensions/Certificate/CertificateVerify/Finished) over
  CRYPTO frames across the Initial/Handshake encryption levels, ACK +
  loss handling, transport parameters, and 1-RTT key install. This is
  where the TLS *message* builders/parsers move out of `tls.c` into a
  shared layer so both TCP-TLS and QUIC drive them.
- **Slice 5** — HTTP/3 framing (HEADERS/DATA) + QPACK over QUIC
  streams, wired into `http.c` behind an Alt-Svc / explicit-h3 probe
  with the h2/h1.1 fallback ladder intact.

## v1 scope
Initial packets only (long header, the handshake's first flight);
Handshake and 1-RTT (short-header) packets reuse the same protection
with their own keys when the state machine lands. No UDP, no
connection state, no retransmission yet.
