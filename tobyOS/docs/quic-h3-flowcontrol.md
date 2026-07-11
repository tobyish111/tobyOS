# HTTP/3 flow control — lift the 1 MiB cap + fast oversized-fallback (slice 5f)

Branch `quic-h3-flowcontrol`, stacked on `quic-chacha20` (slice 5e).
The h3 response was hard-capped at a fixed 1 MiB static buffer, and a
larger response *timed out* before the ladder fell back. Now the
response buffer and the advertised flow-control window are sized
**per fetch** to the caller's `max_body` (up to a 4 MiB ceiling), so
larger pages fetch over h3, and a response that exceeds the window
**fails fast** to h2/h1.1 instead of stalling.

## What changed (all in `src/quic_conn.c`)
- **Per-fetch flow-control window.** `quic_build_client_hello` /
  `quic_build_transport_params` take an `fc_window`;
  `initial_max_stream_data_bidi_local` is advertised at that value so a
  server can send a whole response within the initial window (no
  `MAX_STREAM_DATA` update needed). `initial_max_data` (connection
  level) gets **+64 KiB of headroom** above the stream window, so the
  server's own control / QPACK uni-streams don't eat into stream 0's
  allowance — which makes the oversized check below deterministic.
- **Dynamic response buffer.** The 1 MiB static response buffer is gone.
  `http3_fetch` (the wrapper) `kmalloc`s the reassembly buffer + its
  coverage bitmap sized to `max_body + framing headroom` (capped at
  `H3_RESP_MAX` = 4 MiB) and frees them after `http3_fetch_core`
  returns — the wrapper split means the core's many exit paths don't
  each have to free. The handshake CRYPTO buffers stay small + static.
- **Fast oversized-fallback.** Two ways to conclude "too big for this
  fetch's window": a stream FIN whose offset exceeds the buffer, or —
  the fast path — the flow-control window filling (`contig >= s0cap`)
  with no FIN, meaning the server is blocked waiting for a window
  extension we won't send. Either returns `HTTP_ERR_TOOBIG`
  immediately, so the `http.c` ladder falls back to h2/h1.1 without a
  timeout.

## Verified (live, one boot)
Built `-DQUIC_SEND_TEST -DTLS_TEST_CA` (+`-DQUIC_REALWORLD`); the aioquic
rig's `/big` page was grown to ~1.7 MiB (above the old 1 MiB cap):
1. **Large response** — `http_get_opt("https://tobyos.test:4433/big")`
   returns `status=200, body=1704027 bytes -- LARGE RESPONSE OVER
   HTTP/3` (≈1250 packets reassembled). Previously this exceeded the
   1 MiB buffer and timed out.
2. **Fast oversized-fallback** — the same ~1.7 MiB `/big` fetched with a
   **200 KB `max_body`**: the server fills the 204 096-byte window and
   blocks, tobyOS logs `response filled the 204096-byte window with no
   FIN -- HTTP_ERR_TOOBIG (fall back)`, and the fetch returns
   `rc=-6 … in ~0s -- FAST OVERSIZED-FALLBACK OK` (was a 15 s timeout).
3. **No regression** — the 105-byte local page, and the real
   `cloudflare-quic.com` page (`status=200`, 13 390-byte gzipped body)
   still complete. Deterministic self-tests unchanged (crypto 9/9,
   packet 6/6, client-hello 3/3, http3 3/3, altsvc 5/5).

## What's next
- MAX_STREAM_DATA / MAX_DATA *updates* (grow the window mid-stream) if a
  truly unbounded streaming consumer is ever needed — not required
  while we buffer-then-parse within a per-fetch window.
- Persisting the Alt-Svc cache across reboots.
- openh264 High-profile / libgav1 AVIF (unblocked by the C++ runtime),
  and the owed EliteDesk real-hardware pass.

## v1 scope
The h3 response cap is now the caller's `max_body` (browser render cap),
bounded by a 4 MiB hard ceiling; larger responses fall back fast to
h2/h1.1. One h3 fetch in flight at a time (fixed UDP source port). The
initial flow-control window is not grown mid-stream (we buffer the
whole response), which is why the ceiling exists.
