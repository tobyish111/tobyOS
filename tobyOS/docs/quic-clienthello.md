# QUIC ClientHello + client Initial (HTTP/3 slice 4)

Branch `quic-clienthello`, stacked on `quic-packet`. The QUIC
handshake's first flight: a real TLS 1.3 ClientHello configured for
QUIC, wrapped in a CRYPTO frame inside a protected client Initial
packet. This is the packet a QUIC client actually sends first —
verified **interoperably** against the aioquic reference, which
removes protection, decrypts, and parses it.

## What shipped
- **`src/quic_conn.c` + `include/tobyos/quic_conn.h`**:
  - **`quic_build_client_hello()`** — a TLS 1.3 ClientHello with
    X25519 `key_share`, `supported_versions` (1.3), `supported_groups`,
    the TLS 1.3 `signature_algorithms` set, SNI, ALPN **`h3`**, and the
    mandatory **`quic_transport_parameters`** extension (0x0039). The
    transport params carry `initial_source_connection_id` plus
    client-default idle timeout / flow-control limits. Offers both
    `TLS_AES_128_GCM_SHA256` and `TLS_CHACHA20_POLY1305_SHA256` (both
    ciphers now exist in the tree).
  - **`quic_build_client_initial()`** — wraps the ClientHello in a
    CRYPTO frame, optionally pads to 1200 bytes (required for a live
    send, per RFC 9000 anti-amplification), and builds + protects the
    Initial packet through the slice-2 crypto + slice-3 packet layers.

## Verified (QEMU + offline aioquic interop)
Deterministic kernel self-test (`-DQUIC_SELFTEST`), 3/3: ClientHello
builds (201 bytes), the Initial builds (247 bytes), and it round-trips
through `quic_open_initial`. Then the **interop proof**: the self-test
dumps the protected packet as hex; a host script feeds it to aioquic,
which
- derives the Initial keys from the packet's DCID,
- removes header protection (recovers first byte `c9`, packet number 0),
- **authenticates the AES-128-GCM tag** and decrypts the 205-byte
  payload,
- **parses the ClientHello** and reports `ALPN = ['h3']` with the
  transport parameters present.

`RESULT: PASS -- aioquic accepts tobyOS QUIC Initial`. (The slice-2
crypto 9/9 and slice-3 packet 4/4 self-tests still pass.)

### Gotcha found + fixed
`quic_open_initial` decrypts and removes header protection **in
place**. The self-test originally dumped the packet *after* the
round-trip open, so the offline validator saw the decrypted plaintext
(no encryption/HP visible) and failed the tag check. Fix: dump the
protected packet *before* the round-trip, and round-trip on a copy.

## What's next (HTTP/3 slices)
- **Slice 4b** — UDP: send this Initial (padded to 1200) to a server
  and receive its Initial/Handshake packets; the kernel has
  `udp_send`/`udp_recv` already.
- **Slice 4c** — the handshake state machine: parse ServerHello from
  the server's Initial, derive Handshake keys, process
  EncryptedExtensions/Certificate/CertificateVerify/Finished from
  Handshake packets (reusing the stage-13H cert validation), send the
  client Finished, install 1-RTT keys. This is where the TLS message
  builders/parsers move out of `tls.c` into a shared layer.
- **Slice 5** — HTTP/3 HEADERS/DATA framing + QPACK over QUIC streams.

## v1 scope
Produces and verifies the outbound client Initial only. No UDP send,
no server-response processing, no state machine yet.
