# HTTP/3 over QUIC — the first page fetch (HTTP/3 slice 5)

Branch `quic-h3`, stacked on `quic-live` (slice 4h). tobyOS fetches a
page over **HTTP/3**: after the live QUIC handshake, it opens the
client-bidi request stream, sends a QPACK-encoded GET, and decodes the
server's QPACK response headers + DATA body — against a real, unmodified
aioquic `H3Connection` server. This is the payoff the QUIC slices built
toward: request and response over 1-RTT STREAM frames, end to end.

## What shipped
- **`src/http3.c` + `include/tobyos/http3.h`** — the
  transport-independent HTTP/3 pieces:
  - **QPACK request encode** (`h3_qpack_encode_request`): field-section
    prefix (Required Insert Count 0 — we never offer a dynamic table)
    + `:method GET` / `:scheme https` / `:path` / `:authority`, each as
    an indexed static field line when the static table has an exact
    match, literal-with-name-reference when only the name matches,
    literal-with-literal-name otherwise. Values are written raw (H=0).
  - **QPACK decode** (`h3_qpack_decode`): all three static/literal
    field-line forms, names/values optionally **Huffman-coded** (the
    RFC 7541 code, shared with HPACK via `hpack_huff_table.h`);
    dynamic-table references are rejected (we advertise capacity 0, so
    a conforming peer never sends them). QPACK prefix integers per
    RFC 9204 §4.1.1.
  - **`src/qpack_static.h`** — the 99-entry RFC 9204 static table,
    *generated from the pylsqpack (ls-qpack) reference decoder* (fed an
    indexed field line per index; scratchpad `qpack_gen.py`) — ground
    truth from a real implementation, not transcription.
  - `h3_frame_hdr` — H3 frame header (varint type + varint length).
- **`src/quic_packet.c`** — **STREAM frames**: `quic_frame_stream`
  builder (OFF+LEN always explicit, FIN flag) and parsing of all 8
  STREAM forms (0x08–0x0f, normalized to one type with
  stream_id/offset/len/fin); plus RESET_STREAM / STOP_SENDING and the
  flow-control family (MAX_DATA … STREAMS_BLOCKED) parsed-and-skipped.
- **`src/quic_conn.c`** — the HTTP/3 exchange after HANDSHAKE_DONE:
  one 1-RTT packet carrying the ACK, the **client control stream**
  (id 2: stream type 0x00 + an empty SETTINGS frame — advertising no
  QPACK dynamic table) and the **request stream** (id 0: HEADERS frame
  with the QPACK section, FIN). The response stream is reassembled by
  offset (same `qrsm` machinery as the CRYPTO flights), server uni
  streams (control/QPACK) are tolerated and ignored, 1-RTT packets are
  ACKed, and once stream 0 is contiguous through its FIN the H3 frames
  are walked: HEADERS → QPACK decode (each field printed, `:status`
  captured), DATA → body.

## Verified
1. **Deterministic self-test** (`-DQUIC_SELFTEST`, `h3_selftest` 3/3):
   decodes a **pylsqpack-encoded** response section (embedded reference
   vector with indexed, name-ref and literal-name lines, Huffman
   literals) to the exact expected fields; encodes the request section
   (18 bytes) and dumps it; a host script fed that dump to pylsqpack,
   which decoded exactly `:method GET / :scheme https / :path / /
   :authority tobyos.test` — both directions verified against the
   reference. Prior self-tests unregressed (crypto 9/9, packet 4/4,
   client-hello 3/3).
2. **Live** (QEMU SLIRP + aioquic 1.3.0 `H3Connection` server with the
   slice-4h test-CA chain, `-DQUIC_SEND_TEST -DTLS_TEST_CA`):
   - tobyOS: full 4h handshake (cert chain + CertificateVerify OK) →
     `sent GET https://tobyos.test/ over HTTP/3 (SETTINGS + QPACK
     HEADERS, 65 byte 1-RTT packet)` → `response HEADERS … :status:
     200, content-type: text/html, server: aioquic-h3,
     content-length: 105` → the full 105-byte HTML body → **`HTTP/3
     GET COMPLETE: status=200 … FIRST PAGE FETCHED OVER HTTP/3`**.
   - server: `request headers [(':method','GET'), (':scheme','https'),
     (':path','/'), (':authority','tobyos.test')] (stream 0,
     ended=True)` → **`PASS: served GET / over HTTP/3`** — aioquic's
     QPACK decoder accepted our encoding on the wire.

### Gotcha (cost a debug cycle)
The first live run timed out with the server having negotiated ALPN but
tobyOS receiving nothing. Cause: the **`udp.c` QUIC recv-hook is gated
by `-DQUIC_SEND_TEST`**, and an intermediate self-test build (full
rebuild — the Makefile had changed) recompiled `udp.o` *without* it;
the subsequent live build's touch list didn't include `src/udp.c`, so
the stale hook-less object shipped. The EXTRA_CFLAGS staleness rule
applies to **every** flag-gated file: touch `src/kernel.c
src/tls_trust.c src/quic_conn.c src/udp.c` before QUIC test builds.

## What's next
- Wire HTTP/3 into `http.c` behind an Alt-Svc / explicit-h3 probe with
  the h2/h1.1 fallback ladder intact (the 13G pattern), so the browser
  fetches real pages over h3 — the QUIC client moves out of the
  boot-test into a reusable transport (connection struct instead of
  locals, DNS + hostname plumbing, response streaming into
  `http_response`).
- Larger responses (flow-control updates — we currently never extend
  the peer's `initial_max_data`/stream windows beyond the handshake
  values), request bodies, multiple requests per connection.

## v1 scope
One GET per boot-test connection on stream 0, response ≤ 8 KB
(reassembly buffer), no flow-control window updates, no dynamic QPACK
table (by design — capacity 0 is fully conformant), no trailers, no
server push (we never send MAX_PUSH_ID), body printed to serial (not
yet surfaced to `http.c`).
