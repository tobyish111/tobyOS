# The 7-second blocking close

## Symptom

Real pages took ~50 s to load. Wikipedia's *Operating system* article spent
**15 s** in `collect+sheets` — the phase that fetches `<link rel=stylesheet>`
— while `dom_build` parsed 661 KiB of HTML in 34 ms.

The obvious diagnosis was "sheets are fetched synchronously, make them
async/parallel." **That would have fixed nothing.** Two measurements
killed the theory:

1. **The CSS parser is fast.** rdtsc accumulators over the parser's
   sub-phases: `sel=11M + decl=13M` TSC ≈ **6 ms** to parse Wikipedia's
   1,060 rules. Not the parse.
2. **Per-sheet breakdown** (`malloc` / `fetch` / `parse` / `free`):

```
sheet malloc=2 fetch=7268 parse=53 free=1 ms bytes=207748
sheet malloc=0 fetch=7328 parse=1  free=0 ms bytes=6898
```

`fetch` was **~7.3 s** — and a **6,898-byte** sheet cost the same as a
207 KiB one. A fixed per-fetch cost, independent of size, is a timeout,
not a transfer. The serial log confirmed it: the body arrived at 9844 ms,
the connection FIN'd at 9876 ms, and the syscall didn't return until
~17265 ms. **7.4 s of dead wait after the data was already in hand.**

## Root cause

`tcp_close()` blocks the caller twice:

```c
uint64_t dl = pit_ticks() + hz * 5u;              /* 5 s */
(void)tcp_poll_until(c, dl, pred_closed_basic);   /* wait out the FIN handshake */

if (c->in_use && c->state == TCP_TIME_WAIT) {
    while (pit_ticks() < c->tw_deadline_tick) {   /* + TCP_TW_MSL_MS (2 s) */
```

A client that closes first is the **active closer**: FIN_WAIT_1 →
FIN_WAIT_2 → TIME_WAIT, so it eats the 5 s wait plus the 2 s linger ≈ the
measured 7.3 s. `http_get_opt` calls `tls_close()` on **every completed h2
fetch**, so every HTTPS request on the modern web paid it.

`tcp_abort()` already existed for exactly this reason — its comment notes
that `tcp_close` "would make us the active closer and hang in TIME_WAIT
for the full linger."

## Fix

Two places where a graceful close buys nothing and costs everything:

- **`http_get_opt`, h2 completion**: the response is fully in hand and the
  connection is being discarded → `tls_abort()` (RST + immediate free).
- **`keep_entry_close`**: a *parked* connection is idle and being
  discarded; blocking the caller (who only wanted a cache slot) for 7 s is
  indefensible → `tls_abort()` / `tcp_abort()`.

## Result

| measurement | before | after |
|---|---|---|
| fetch, 207 KiB sheet | 7,268 ms | **224 ms** |
| fetch, 6.9 KiB sheet | 7,328 ms | **234 ms** |
| Wikipedia `collect+sheets` | 14,659 ms | **470 ms** |
| **Wikipedia full load** (7 resources) | **48.3 s** | **2.0 s** |
| GitHub `collect+sheets` | 10,226 ms | **3,073 ms** (now 22 sheets, not 16) |

**~24× faster page loads**, on every h2 HTTPS site. Per-sheet cost is now
real network RTT (~140-230 ms), not a timeout.

Verified: Wikipedia renders identically (128 links, 0 JS errors), default
home page unchanged, ESM 3/3 ALL PASS, no panics.

## Still open

- `transport_close` (the h1 / error / non-keepalive paths) still uses the
  blocking `tcp_close`. Modern sites negotiate h2/h3 so they no longer hit
  it, but an h1-only origin would still pay ~7 s per fetch.
- The real cure is a non-blocking close: send FIN and let the stack reap
  FIN_WAIT/TIME_WAIT in the background. That needs `tcp_tick_all()` (today
  `static`, and only pumped from inside active poll loops) driven from
  `net_service_tick()`; without background reaping a deferred close would
  leak connection slots, which is why abort is the right call for now.
- h2 connections are not reused: every sheet re-does a TLS handshake
  (~140 ms). Connection reuse is the next win after this.

## Lesson

The instinct ("fetching is slow → make it parallel") was wrong twice over:
the network was fine and the parser was fine. Two cheap measurements —
rdtsc around the parser, and a malloc/fetch/parse/free split — pointed
straight at a teardown timeout. Measure before optimising.
