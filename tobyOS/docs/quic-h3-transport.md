# HTTP/3 as a reusable transport — wired into http.c (HTTP/3 slice 5b)

Branch `quic-h3-transport`, stacked on `quic-h3` (slice 5). The HTTP/3
GET that slice 5 proved as a boot-time test becomes a **reusable
transport**: `http3_fetch()` runs the whole QUIC handshake + GET for
any host/port/path and fills a `struct http_response`, and `http.c`
probes it first (behind an opt-in flag) with the proven h2/h1.1
fallback ladder intact. A browser-shaped call —
`http_get_opt("https://tobyos.test:4433/", …, HTTP_F_TRY_H3, &resp)` —
now fetches a page over HTTP/3 end to end.

## What shipped
- **`http3_fetch(ip_be, port, host, path, flags, max_body, timeout_ms,
  out)`** (`src/quic_conn.c`) — the slice-4h/5 boot test refactored
  into a parameterised transport. Destination, SNI/`:authority`,
  certificate-validation hostname, and `:path` all come from the
  caller; the response is assembled into `out` (status, content-type,
  content-length, content-encoding → `HTTP_ENC_*`, and a `kmalloc`'d
  body freed by `http_free`). Returns `0` or a negative `HTTP_ERR_*`
  (mapped per failure: `CERT` for an untrusted chain / failed
  CertificateVerify, `TOOBIG` past the 256 KiB cap, `TIMEOUT`,
  `RESET` on CONNECTION_CLOSE, `CONNECT` on VN / handshake failure).
  The request now also sends a `user-agent` and, under `HTTP_F_GZIP`,
  `accept-encoding: gzip, br` (the QPACK encoder gained a `want_gzip`
  argument). The response stream is reassembled with the same offset
  bitmap as the CRYPTO flights, capped at 256 KiB (= the
  `initial_max_stream_data` we advertise, so flow control never blocks
  inside it); a larger response returns `HTTP_ERR_TOOBIG` so the ladder
  falls back.
- **`http_get_opt` h3 probe** (`src/http.c`) — new flag
  **`HTTP_F_TRY_H3`**: for `https`, once DNS has resolved the IP, try
  `http3_fetch` first; on success return, on **any** failure fall
  through to the existing TLS `tls_connect` → h2/h1.1 path (GET is
  idempotent, so h3 can never regress a fetch). A definitive
  `HTTP_ERR_CERT` is surfaced rather than retried over TCP. One-shot
  per request (`tried_h3`), skipped on keep-alive reuse and after an
  h2→h1 downgrade. **Off by default** — existing callers are
  byte-identical; the block is skipped when the flag is clear.
- **UDP recv hook un-gated** (`src/udp.c`) — the QUIC client reply port
  (56789) is now always routed to `quic_recv_hook`, not just under
  `-DQUIC_SEND_TEST`, because `http3_fetch` is a real transport. The
  hook only queues into a ring the active fetch drains + resets, so a
  stray datagram with no fetch in flight is harmless.
- **Test-host DNS seam** (`src/dns.c`, `#ifdef TLS_TEST_CA`) — resolves
  `tobyos.test` → `10.0.2.2` (the SLIRP host) so the aioquic test
  server is reachable *by name* (its cert SAN is `tobyos.test`).
  Compiled in only under the same flag that trusts the test CA;
  production never sees it.

## Verified
1. **Live, two proofs in one boot** (QEMU SLIRP + aioquic 1.3.0
   `H3Connection` server, `-DQUIC_SEND_TEST -DTLS_TEST_CA`):
   - **transport in isolation** — `http3_fetch(10.0.2.2, 4433,
     "tobyos.test", "/", …)` → `transport OK: status=200
     type='text/html' body=105 bytes: <html>…</html>` (the response
     fully assembled into `http_response`).
   - **the integrated path** —
     `http_get_opt("https://tobyos.test:4433/", HTTP_F_TRY_H3 |
     HTTP_F_TRUNCATE)` → URL parse → DNS (test seam) → h3 probe →
     `http3_fetch` → `http_get_opt over h3 OK: status=200 body=105
     bytes -- HTTP/3 WIRED INTO http.c`.
   The server logs the decoded request both times, now including
   `('user-agent', 'tobyOS')`, and `PASS: served GET /`.
2. **Deterministic** — `h3_selftest` still 3/3 (QPACK decode of the
   pylsqpack vector, request encode, frame header); the request
   section — now 27 bytes with the user-agent field — was re-validated
   by feeding the kernel's serial hex to pylsqpack, which decoded it to
   exactly `:method GET / :scheme https / :path / / :authority
   tobyos.test / user-agent tobyOS`. QUIC crypto 9/9, packet 4/4,
   client-hello 3/3 unchanged.
3. **No regression** — `HTTP_F_TRY_H3` is off for every existing
   caller, so the h2/h1.1 fetch path is byte-identical; the plain build
   excludes the test CA and the DNS seam (both `#ifdef TLS_TEST_CA`),
   with `http3_fetch` compiled but never invoked without the opt-in.

## What's next
- Have the browser opt into `HTTP_F_TRY_H3` (Alt-Svc discovery: cache
  `Alt-Svc: h3=":443"` from an h1/h2 response, then upgrade the next
  fetch to the same origin), once the h3 path grows the robustness it
  needs for the open web: flow-control window updates (MAX_DATA /
  MAX_STREAM_DATA) so responses can exceed 256 KiB, and a
  ChaCha20-Poly1305 1-RTT suite so servers that prefer it aren't
  refused at ServerHello.
- openh264 High-profile / libgav1 AVIF (unblocked by the C++ runtime),
  and the owed EliteDesk real-hardware pass for the 12E+13 stack.

## v1 scope
`http3_fetch` is one GET per connection (fresh QUIC handshake each
call, fixed UDP source port → one fetch at a time), response capped at
256 KiB, AES-128-GCM only (a ChaCha20 server is refused at
ServerHello), no connection reuse / keep-alive, no request body, no
flow-control window growth. The h3 probe is an explicit opt-in
(`HTTP_F_TRY_H3`); the browser does not set it yet.
