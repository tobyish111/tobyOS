# HTTP/3 against the real internet — large responses (HTTP/3 slice 5c)

Branch `quic-h3-realworld`, stacked on `quic-h3-transport` (slice 5b).
tobyOS's from-scratch QUIC/HTTP-3 stack now fetches a **live page from
the open internet over HTTP/3** — real DNS, a real certificate chain
validated to a Mozilla root over QUIC, real QPACK — and handles
**multi-hundred-KiB responses**. This is the milestone the QUIC slices
were built toward: HTTP/3 that works against real servers, not just a
local test rig.

## What changed (all in `src/quic_conn.c`)
A large response exposed limits that a 105-byte test page never did:

- **Reassembly frontier is O(1) amortised.** The response/CRYPTO
  reassembler (`struct qrsm`) tracked a per-byte coverage bitmap and
  re-scanned it from 0 on every datagram to find the contiguous prefix
  — O(n) per datagram, i.e. O(n²) over a multi-megabyte transfer. It
  now keeps a monotone `contig` high-water mark that only advances, so
  the total scan work is O(n) across the whole stream.
- **Generous initial flow control.** The client transport parameters
  now advertise `initial_max_data` 2 MiB and
  `initial_max_stream_data_bidi_local` 1 MiB, so a server may send a
  whole ordinary page before flow control would block — no dynamic
  `MAX_STREAM_DATA` updates needed within that window. The response
  reassembly cap (`H3_RESP_MAX`) matches at 1 MiB; a larger response
  returns `HTTP_ERR_TOOBIG` and the `http.c` ladder falls back to
  h2/h1.1.
- **Bigger receive ring.** The UDP recv ring grew from 8 to 64 slots so
  a burst of response packets is buffered rather than dropped (drops
  cost a round-trip of loss recovery).
- **Bigger handshake reassembly.** The Handshake-flight buffer grew
  from 8 KiB to 16 KiB — a real certificate chain (leaf + one or more
  intermediates) is larger than the single self-signed cert the local
  rig sent.

## Verified (three proofs in one boot, reproduced)
Built `-DQUIC_SEND_TEST -DTLS_TEST_CA -DQUIC_REALWORLD`; the boot test
runs all three and each was seen on two consecutive runs:

1. **Small local** — `http3_fetch` to the aioquic rig, 105-byte page,
   `transport OK: status=200`.
2. **Large local** — `http_get_opt("https://tobyos.test:4433/big",
   HTTP_F_TRY_H3)`, a **923 027-byte** page: `LARGE RESPONSE OVER
   HTTP/3` (≈660 packets reassembled by offset, fetched in ~1.5 s under
   TCG). This exercises the flow-control window, the O(1) frontier, and
   the recv ring.
3. **The open internet** — `http_get_opt("https://cloudflare-quic.com/",
   HTTP_F_TRY_H3 | HTTP_F_GZIP)`:
   - real DNS: `cloudflare-quic.com -> 104.18.26.14`,
   - QUIC v1 handshake to Cloudflare's edge, **cert chain validated to
     a Mozilla root** with `CertificateVerify VERIFIED (scheme 0x0403)`
     — a real ECDSA-P256 Cloudflare certificate (vs the test CA's RSA-PSS
     0x0804, so both signature schemes are exercised),
   - `HANDSHAKE_DONE`, then the response: **`status=200, 9 headers,
     13390-byte gzipped body`** (`enc=1`), a **123-byte QPACK section
     with 9 fields** decoded (real-server QPACK, more static entries +
     Huffman than the rig sent),
   - `REAL-WORLD OK … LIVE PAGE FROM THE INTERNET OVER HTTP/3`.

Cloudflare selected `TLS_AES_128_GCM_SHA256` (honouring our preference
order), so AES-128-GCM sufficed. Regression: the deterministic
self-tests still pass (crypto 9/9, packet 4/4, client-hello 3/3, http3
3/3). The plain build excludes the test CA, the DNS seam, and the
`QUIC_REALWORLD` boot code.

## What's next
- Have the browser opt into `HTTP_F_TRY_H3` via Alt-Svc discovery
  (cache `Alt-Svc: h3=":443"` from an h1/h2 response, upgrade the next
  same-origin fetch), so real navigation uses h3 where offered.
- A ChaCha20-Poly1305 1-RTT suite (some servers prefer it), and
  `MAX_STREAM_DATA`/`MAX_DATA` updates to lift the 1 MiB response cap.
- openh264 High-profile / libgav1 AVIF (unblocked by the C++ runtime),
  and the owed EliteDesk real-hardware pass.

## v1 scope
Still one GET per connection (fresh handshake each call, fixed UDP
source port → one fetch at a time), response capped at 1 MiB,
AES-128-GCM only (a ChaCha20-only server is refused at ServerHello),
no connection reuse, no request body, no flow-control window growth
beyond the advertised 1 MiB. The browser does not set `HTTP_F_TRY_H3`
yet. Real-world reachability depends on the guest having working
outbound UDP/443 (here via QEMU SLIRP NAT).
