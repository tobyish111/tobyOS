# Connection reuse + a close that doesn't block

The two follow-ups from `browser-blocking-close.md`.

## 1. h2 connection reuse

`http2_fetch` was one-request-per-connection: it allocated `struct h2`,
sent the preface + SETTINGS, made its request on a hardcoded **stream 1**,
then freed everything. Every subresource therefore paid a fresh TLS
handshake (~140 ms) — 22 of them on a GitHub page.

What reuse needed:

- **`struct h2` is CONNECTION state, not request state.** The HPACK
  dynamic table is the reason: the server indexes into it across requests
  on the same connection, so it must survive or headers decode to
  garbage. It is now parked with the connection.
- **Stream ids increment** (`next_sid`, +2, client streams are odd)
  instead of always 1, and the END_STREAM/RST_STREAM matches follow it.
- **Preface + SETTINGS once per connection** (`h->started`), not per
  request.
- **Connection flow control must be topped up.** The initial
  `WINDOW_UPDATE` grants `H2_MY_WINDOW` (8 MiB) *once*; across reused
  requests that depletes and the connection would eventually stall. Each
  request now hands back the DATA bytes it consumed — counted on the
  **whole payload including padding** (RFC 7540 6.9.1), before de-padding.

API: `http2_fetch_on(&state, tls, …)` carries state across calls;
`http2_fetch()` remains as a one-shot wrapper. **On any failure the state
is freed and set to NULL** — a half-failed h2 connection has
indeterminate HPACK/stream state and must never be reused.

The keep-alive cache (`struct keep_conn`) now parks the `struct h2`
alongside the `tls_conn`, and `keep_entry_close` frees it.

> **Placement gotcha:** the h2 branch used to live *inside* the
> fresh-connect `else`. It now sits **after** the reuse/connect split —
> otherwise a parked h2 connection falls through to the h1 request
> builder and writes HTTP/1.1 bytes onto an h2 connection. A stale parked
> connection retries once on a fresh one (still h2) before falling back
> to h1.

## 2. Alt-Svc auto-upgrade is now opt-in (`HTTP_F_ALTSVC_AUTO`)

With h2 reuse landed, measuring github.com showed **84 h3 fetches → 84
QUIC handshakes**, because our h3 client opens a fresh QUIC connection per
request. Its sheets cost ~3.0 s, versus 94 ms for Wikipedia's over one
reused h2 connection.

So auto-upgrading a multi-resource page load to h3 is currently a
**pessimisation**: reused h2 beats per-request QUIC. The Alt-Svc
auto-upgrade is now behind `HTTP_F_ALTSVC_AUTO` (off for page loads).
`HTTP_F_TRY_H3` and the QUIC/Alt-Svc self-tests still exercise the h3
path explicitly, so the feature stays live and tested. **Re-enable the
default once h3 connections are parked and reused.**

## 3. A close that doesn't block (`tcp_close_nowait`)

`tcp_close()` blocks the caller ~7 s (5 s FIN wait + `TCP_TW_MSL_MS`
linger). The previous fix used `tcp_abort()` (RST) at the h2 completion
and keep-alive eviction, but `transport_close` — the h1 path, error
paths, and every non-keepalive fetch (wget/pkg downloads) — still paid it.

The blocker was that **nothing advanced a closing connection in the
background**: `tcp_tick_all()` was `static` and only ran inside active
poll loops, so a deferred close would leak its slot forever.

Now:
- `tcp_service_tick()` — drives retransmit timers **and reaps detached
  conns** whose TIME_WAIT expired — is called from `net_service_tick()`
  in the kernel idle loop.
- `tcp_close_nowait()` sends FIN, marks the conn `detached`, and returns.
  The tick runs out the handshake/linger and recycles the slot.
- `tls_close_nowait()` = close_notify + `tcp_close_nowait`.
- `transport_close` uses them.

`detached` is the safety interlock: the reaper only frees conns whose
owner has explicitly abandoned them, never one a caller still holds.

This keeps the peer getting a proper FIN (unlike the RST of `tcp_abort`)
while still never blocking.

## Results

| `collect+sheets` | original | close fix | **+ reuse** |
|---|---|---|---|
| wikipedia.org | 14,659 ms | 470 ms | **92 ms** |
| github.com | 10,226 ms | 3,073 ms | **483 ms** |

Wikipedia: **1 TLS handshake** for the page (was 7).
GitHub: `reuse github.githubassets.com:443 (reused=84 handshakes=2)` — 84
reuses on 2 handshakes, where h3 had been doing 84 QUIC handshakes.

**~160x** on Wikipedia's sheet phase vs where this started; per-sheet
fetch 7,268 ms → 45 ms.

Verified: both sites render identically, 0 h2 failures, 0 fallbacks, 0
TCP errors, default home page unchanged, ESM 3/3 ALL PASS, no panics.

## Still open

- **h3 connection reuse** — the reason the Alt-Svc auto-upgrade is off.
  Needs QUIC connection state (keys, CIDs, packet number spaces, QPACK)
  parked like h2's.
- **h2 multiplexing** — we still serialise requests on the reused
  connection (one stream at a time). Real browsers run them concurrently.
- `http2_fetch` allocates `max_body` (up to the read cap) for the body
  upfront regardless of the actual response size.
