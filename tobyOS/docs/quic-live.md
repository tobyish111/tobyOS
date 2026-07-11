# QUIC vs a live server — trusted certs, Retry, VN, ACKs (HTTP/3 slice 4h)

Branch `quic-live`, stacked on main (post the 4th stage-13 merge, which
landed slices 4b–4g). tobyOS now completes the QUIC handshake against a
**real, unmodified QUIC server** (aioquic 1.3.0's full connection
machine — not the scripted responder of 4b–4g) presenting a certificate
that chains to a **trusted root**, so certificate authentication runs
end to end over QUIC for the first time: chain validation *and*
CertificateVerify both pass, and the server independently confirms the
handshake. Retry and Version Negotiation are handled too.

## What a real server added (vs the scripted responder)
- **Multiple datagrams** — the recv hook kept only the *first* datagram;
  a real flight arrives in several. Now a small ring queue
  (`quic_recv_hook` + `qrx_pop`, 8 × 2048 in `quic_conn.c`).
- **CRYPTO reassembly by offset** — the ~2 KB Certificate chain spans
  multiple CRYPTO frames across multiple Handshake packets (and
  retransmissions). CRYPTO data is copied in at its stream offset with
  per-byte coverage bits (`struct qrsm`); the flight is consumed only
  once the prefix is contiguous, and duplicates self-dedupe.
- **ACKs we must send** — servers gate retransmission (and the
  anti-amplification budget) on client ACKs. tobyOS now tracks received
  packet numbers per encryption level (`struct qack`) and sends ACK
  frames at the Initial and Handshake levels (`quic_frame_ack`), plus a
  1-RTT ACK for the HANDSHAKE_DONE packet. Client datagrams carrying an
  Initial are padded to 1200 (RFC 9000 §14.1 applies to ACK-only ones
  too — `build_initial_1200`).
- **Connection routing by CID** — the scripted responder ignored header
  CIDs; a real server routes by them. tobyOS adopts the server's SCID
  as its DCID for all subsequent packets (Initial ACKs, Handshake,
  1-RTT), while Initial *keys* stay derived from the original (or
  post-Retry) DCID.
- **Retry** (RFC 9000 §17.2.5) — on a Retry packet tobyOS verifies the
  RFC 9001 §5.8 integrity tag (`quic_retry_tag_verify` — AES-128-GCM
  with the fixed v1 key/nonce over the pseudo-packet; constants taken
  from the aioquic reference), adopts the server's new CID, re-derives
  the Initial keys from it, and resends the *same* ClientHello with the
  token echoed in every subsequent Initial. Retries after a processed
  Initial, second Retries, and bad tags are ignored per spec.
- **Version Negotiation** — a version-0 packet is parsed, the offered
  versions logged, and the connection aborted fail-closed (tobyOS is
  v1-only).
- **Frame coverage** — the parser (`quic_frame_parse`) now also handles
  ACK-ECN, NEW_TOKEN, NEW_CONNECTION_ID, RETIRE_CONNECTION_ID,
  CONNECTION_CLOSE (error code + reason logged — invaluable against a
  real peer) and HANDSHAKE_DONE.
- **1-RTT receive** — `quic_open_short` / `quic_build_short`
  (`quic_packet.c`): short-header packets, HP over the low 5 bits,
  packet extends to the datagram end. The server's post-handshake
  1-RTT packet (HANDSHAKE_DONE + NEW_CONNECTION_ID…) decrypts with the
  server application-traffic keys.

## Certificate authentication is now REQUIRED
Slices 4f–4g ran chain validation but tolerated an untrusted result
(the scripted responder had a self-signed cert). Against a live server
the client *requires* both halves — an untrusted chain, a missing
Certificate/CertificateVerify, or a bad CertificateVerify signature
each abort the handshake fail-closed.

**Trust for the test**: `tests/quic-ca/make_test_ca.py` generates a
test CA (RSA-2048) + a leaf for `tobyos.test`, and emits
`third_party/bearssl/test_ca_anchor.inc` (the CA in BearSSL
trust-anchor form, same layout `brssl ta` emits). `src/tls_trust.c`
appends it to the 121 Mozilla roots **only under `-DTLS_TEST_CA`** — a
production build never trusts it. The test CA's private key is
committed on purpose (test material only).

## Verified (QEMU SLIRP + a real aioquic 1.3.0 server on the host)
The rig (scratchpad `quic_live_server.py` / `quic_live_run.py`) runs
`aioquic.asyncio.serve` with the test-CA chain (leaf + CA ≈ 1.6 KB, so
the flight spans packets), ALPN `h3`, AES-128-GCM. Pre-checked
aioquic-client↔server first, so failures isolate to tobyOS. Kernel
built with `-DQUIC_SEND_TEST -DTLS_TEST_CA`. Four scenarios, two truth
sources each (tobyOS serial + the server's event log):

1. **Normal**: serial shows `Handshake flight complete (1995 bytes
   reassembled)`, **`cert chain OK (trusted root); CertificateVerify
   VERIFIED (scheme 0x0804, peer holds leaf key)`** (RSA-PSS —
   the 13H path over QUIC, end to end), `server Finished VERIFIED`,
   `sent client Finished`, **`1-RTT HANDSHAKE_DONE received -- LIVE
   QUIC HANDSHAKE COMPLETE (server confirmed our Finished)`**; the
   server logs **`PASS: server HandshakeCompleted (alpn=h3)`** — it
   only fires that (and only sends HANDSHAKE_DONE) after verifying
   *our* Finished.
2. **Retry** (`serve(..., retry=True)`): `Retry: integrity tag OK, new
   dcid=…, token 256 bytes -- resending Initial`, then the identical
   full completion. The tag check is a genuine interop check of the
   §5.8 constants — aioquic computed it.
3. **Version Negotiation** (server supports only QUIC v2): `Version
   Negotiation received; server offers: 0x6b3343cf`, `v1 not supported
   by server -- aborting (fail closed)`.
4. **Fail-closed negative** (rebuilt *without* `-DTLS_TEST_CA`, same
   server): the flight decrypts + reassembles identically, then `cert
   chain UNTRUSTED -- aborting (fail closed)` — the server never
   reaches HandshakeCompleted. Proves the trust requirement is real.

Regressions: the deterministic self-tests still pass (crypto 9/9,
packet 4/4, client-hello 3/3) — the frame-parser and packet-layer
extensions changed no existing behavior.

## What's next (HTTP/3 slices)
- **Slice 5** — HTTP/3: open a client-bidi stream (STREAM frames in
  1-RTT packets — `quic_build_short` is ready), send a QPACK-encoded
  HEADERS frame, read HEADERS + DATA back, decode QPACK; wire into
  `http.c` behind an Alt-Svc / explicit-h3 probe with the h2/h1.1
  fallback ladder intact.

## v1 scope
Still a boot-time test client (`-DQUIC_SEND_TEST`), not yet a
transport for `http.c`. Small packet numbers taken at face value (no
pn reconstruction window); server transport parameters not validated
(`original_destination_connection_id` / `retry_source_connection_id`
unchecked); no loss-triggered retransmission of the client Finished
(the server retransmits its flight, but a lost client Finished would
time out); AES-128-GCM only at every level (a ChaCha20-only server is
refused at ServerHello); no key update, no stateless reset.
