# Service Workers + Cache API

`navigator.serviceWorker.register()`, the service-worker lifecycle, the
**Cache API**, and **fetch interception** (`respondWith`) work, so pages
that register a SW for offline caching / request routing function. This is
a pragmatic subset (see deviations) that delivers the core of what service
workers are *for* — intercepting the page's requests and answering from a
cache or a synthesized response.

## What works

Verified on-screen end-to-end (a page registers a SW served from the host;
the SW caches on install and intercepts fetches):

```
[sw] registered scope=/ controller=true
[sw] fetch /cached (from SW cache)=CACHED_BODY   ; SW served the page fetch from cache
[sw] fetch /synth (SW-synthesized)=SYNTH_26       ; SW respondWith(new Response(...))
[sw] page-side caches match=PAGE_CACHED           ; window Cache API
[sw] RESULT: ALL PASS
```

Crucially, the host server saw **only** `/sw.js` — the `/cached` and
`/synth` fetches were intercepted by the SW and never hit the network,
which is the whole point (offline capability).

## Pieces (all in the prelude)

- **`Response` constructor** — `new Response(body, init)` (body: string /
  `ArrayBuffer` / typed array; `status`/`statusText`/`headers`), with
  `text`/`json`/`arrayBuffer`/`blob`/`body`/`clone`. SWs synthesize
  responses with this; the Cache API stores/replays them.

- **Cache API** — `caches` (a `CacheStorage`) on both the window and the
  SW scope: `open`/`match`/`has`/`delete`/`keys`; a `Cache` has
  `put`/`match`/`add`/`addAll`/`delete`/`keys`. Backed by an in-memory map
  keyed by request URL; cached entries store status + headers + the body
  bytes and replay as a fresh `Response`.

- **`navigator.serviceWorker`** — `register(url, opts)` fetches the SW
  script and runs it via `new Function('self','addEventListener','caches',
  'fetch','location','clients','skipWaiting','registration', src)(scope,
  …)`, so the script sees a service-worker `self` (its own event scope +
  `caches`/`clients`/`skipWaiting`) whether it uses `self.addEventListener`
  or the bare form. Fires `install` then `activate`, honoring
  `event.waitUntil(promise)` so a cache populated during install is ready
  before `register()` resolves. Also `ready`, `controller`,
  `getRegistration(s)`.

- **Fetch interception** — `fetch()` is wrapped: if a SW is active and has
  a `fetch` listener, it builds a `FetchEvent` (`request`, `respondWith`,
  `waitUntil`), fires the handler, and returns the `respondWith` response
  if the handler supplied one — otherwise falls through to the real
  network fetch.

## Deviations from the spec (deliberate)

- **Same JS runtime, not an isolated worker context.** The SW script runs
  in the page's QuickJS runtime inside a sandboxed function scope with its
  own `self` (not `globalThis`), rather than a separate thread/context.
  It doesn't share the page's `document`, but it isn't truly isolated the
  way a real SW is.
- **Per-page, not a persistent background worker.** The registration lives
  for the page session and is torn down at navigation; there's no
  cross-navigation persistence, no waking on push, no `updatefound`
  round-trips, no multiple clients.
- **Cache persists to disk.** Cache contents are serialized (bodies as a
  Latin-1 string that round-trips losslessly through the UTF-8 file I/O)
  and written to `/data/browser/__httpcache` via the same `idbSave` path
  as IndexedDB/localStorage on every mutation, and restored at startup.
  Verified: arbitrary binary bytes round-trip through the serializer and a
  binary body is stored + replayed intact. (Cross-reboot survival then
  depends on tobyfs flushing `/data` on shutdown — the same for all
  `/data` state, not cache-specific.)
- No `push`/`sync`/`periodicsync`/`notification` events, no `Clients`
  focus/navigation, no `importScripts` in the SW, no navigation preload.

These match the browser's cooperative single-runtime model; the
request-interception + caching surface that pages actually build on is
what's implemented.

## Test

```
make ... BROWSER_EXTRA="-DSW_TEST" \
     EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DTKAPP_BOOT -DTKAPP_BROWSER" iso
# serve sw.js on 127.0.0.1:8099 (guest 10.0.2.2:8099 via SLIRP);
# boot with -netdev user -device e1000; serial shows [sw] ... ALL PASS
```
