# Alt-Svc discovery — the browser upgrades to HTTP/3 automatically (slice 5d)

Branch `browser-h3-altsvc`, stacked on `quic-h3-realworld` (slice 5c).
This closes the loop: instead of a caller having to pass
`HTTP_F_TRY_H3`, `http_get_opt` now **discovers HTTP/3 support from an
`Alt-Svc` response header (RFC 7838) and transparently upgrades the
next same-origin fetch to h3** — the real browser behaviour. The
browser is unchanged; because it already fetches through
`http_get_opt`, it now uses HTTP/3 wherever a site advertises it, with
the h2/h1.1 fallback ladder still underneath.

## What shipped
- **Alt-Svc cache + parser** (`src/http.c`): a small per-session,
  in-memory table (`g_altsvc`, 32 origins → h3 port). `altsvc_parse`
  scans an `Alt-Svc` value by hand (klibc has no `strstr`): it finds an
  `h3`/`h3-<draft>` protocol id at a token boundary, reads the quoted
  authority, and records the port; the literal value `clear` removes
  the record. `altsvc_lookup` drives the upgrade decision.
- **Discovery on every response path** — a response can arrive over
  h1.1, h2, or h3, and each bypasses the others' header code, so all
  three now record Alt-Svc via one public recorder
  `http_altsvc_note(host, value)`:
  - h1.1 — a new branch in `parse_headers`;
  - h2 — a new branch in `http2.c`'s `on_header` (the h2 response's
    `struct h2_resp` gained a `host`);
  - h3 — a new branch in `quic_conn.c`'s `h3_hdr_to_resp` (its build
    struct gained a `host`).
- **Auto-upgrade in `http_get_opt`** — `try_h3` is now
  `(flags & HTTP_F_TRY_H3) || altsvc_lookup(host, &h3_port)`; the h3
  port comes from the Alt-Svc record. When the upgrade is Alt-Svc-driven
  it logs `Alt-Svc: <host> advertises h3 on :<port> -- upgrading to
  HTTP/3`. A parked keep-alive TCP connection is **not** reused on the
  attempt where we still intend to probe h3 (QUIC first, TCP is the
  fallback). Any h3 failure still falls through to the h2/h1.1 ladder.
- **Concurrency guard** — the transport uses a fixed UDP source port
  and single static reassembly buffers, so only one h3 fetch can be in
  flight. `http3_fetch` is now a thin wrapper around
  `http3_fetch_core` that serialises on a `g_h3_busy` flag: a
  concurrent caller (the browser firing parallel fetches for a page's
  subresources) gets `HTTP_ERR_CONNECT` and falls back to h2/h1.1
  rather than corrupting the in-flight fetch.

## Verified
1. **Deterministic** — `http_altsvc_selftest` 5/5 (basic `h3=":443"`;
   `h2=…, h3=":8443"` picks the h3 port; draft `h3-29`; `clear`
   removes; an h2-only value records nothing). Prior self-tests
   unchanged (crypto 9/9, packet 4/4, client-hello 3/3, http3 3/3).
2. **Local auto-upgrade** (aioquic rig now sends `alt-svc: h3=":4433"`):
   fetch #1 (explicit h3) records the header, then fetch #2 **with no
   h3 flag** → `Alt-Svc: tobyos.test advertises h3 on :4433 --
   upgrading to HTTP/3` → `AUTO-UPGRADED TO HTTP/3 VIA ALT-SVC`,
   status 200.
3. **The real internet, authentic discovery** — two fetches to
   `https://cloudflare-quic.com/`, neither with an h3 flag:
   - **fetch #1** went over **HTTP/2** (`[tls] ALPN selected: h2`),
     status 200, and recorded Cloudflare's `alt-svc: h3=":443"`;
   - **fetch #2** logged `Alt-Svc: cloudflare-quic.com advertises h3 on
     :443 -- upgrading to HTTP/3`, ran the QUIC handshake to
     `104.18.27.14:443`, validated the real cert chain
     (`CertificateVerify … scheme 0x0403`), and returned the gzipped
     page — `LIVE PAGE FROM THE INTERNET OVER HTTP/3`.
   This is the exact browser flow: fetch over h2, learn the origin
   speaks h3, and upgrade the next fetch — fully automatic.
4. **No regression** — with an empty cache and no `HTTP_F_TRY_H3`,
   `try_h3` is false, so an ordinary fetch is byte-identical to before;
   the plain build excludes the test CA, the DNS seam, and the boot
   test, while keeping the (now production) Alt-Svc logic.

## What's next
- A ChaCha20-Poly1305 1-RTT suite and `MAX_STREAM_DATA` updates to lift
  the 1 MiB h3 response cap (large pages currently fall back to h2).
- Persisting the Alt-Svc cache (and honouring `ma`) across reboots.
- openh264 High-profile / libgav1 AVIF (unblocked by the C++ runtime),
  and the owed EliteDesk real-hardware pass.

## v1 scope
In-memory Alt-Svc cache (no `ma`/expiry accounting, cleared on reboot),
one h3 fetch in flight at a time (others fall back to h2/h1.1), h3
responses still capped at 1 MiB and AES-128-GCM only. The upgrade
decision is per-origin by exact host match.
