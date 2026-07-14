# Streams + binary fetch (ReadableStream, arrayBuffer, TextEncoder/Decoder)

Makes the browser's fetch bodies **binary-safe** and adds the Streams +
encoding APIs pages expect around them, so binary downloads,
`response.arrayBuffer()`, `WebAssembly.instantiateStreaming(fetch(...))`,
and `response.body.getReader()` loops work.

## What works

Verified on-screen end-to-end (a page fetches a binary `.wasm` from the
host and streams it):

```
[strm] arrayBuffer bytes=156           ; binary-safe response.arrayBuffer()
[strm] fetched-wasm add(7,35)=42        ; instantiate from the fetched bytes
[strm] reader total bytes=156           ; response.body.getReader() read loop
[strm] textcodec len=10 roundtrip=true  ; TextEncoder/Decoder ('héllo €')
[strm] instantiateStreaming add(10,20)=30
[strm] RESULT: ALL PASS
```

## The core fix: binary-safe bodies

The fetch layer delivered the response body only as a JS string
(`JS_NewStringLen`, UTF-8), which corrupts any byte ≥ 0x80 — fine for
text/JSON, wrong for binary. The async fetch pump (and the sync
`fetchSync`) now **also** attach `bodyBytes`, an `ArrayBuffer`
(`JS_NewArrayBufferCopy`) holding the exact bytes the kernel read. Text
callers keep reading the UTF-8 `body`; binary callers read `bodyBytes`.

## JS surface (prelude)

On the `Response` object:
- `arrayBuffer()` → `Promise<ArrayBuffer>` (a copy of `bodyBytes`).
- `blob()` → `Promise<Blob>`.
- `body` → a `ReadableStream` over the bytes (getter).
- `text()`/`json()` unchanged (UTF-8 `body`).

New globals (guarded with `if (!g.X)` so a host-provided one wins):
- `ReadableStream(underlyingSource)` — `start`/`pull`/`enqueue`/`close`/
  `error`, `getReader()` → `{ read()→Promise<{value,done}>, cancel,
  releaseLock }`, plus `cancel()`. A simple synchronous queue model
  (no backpressure), enough for body streams and typical reader loops.
- `TextEncoder` — `encode(str)` → `Uint8Array` (hand-rolled UTF-8,
  surrogate pairs handled).
- `TextDecoder` — `decode(bufferOrView)` → string (UTF-8, incl. 4-byte).
- `Blob(parts, opts)` — `size`, `type`, `arrayBuffer()`, `text()`.

`WebAssembly.instantiateStreaming`/`compileStreaming` already routed
through `response.arrayBuffer()`; now that it's binary-safe they work
against a real network fetch.

## Stream plumbing

`ReadableStream` is an **async queue**: `read()` returns a pending promise
when no chunk is buffered yet, resolved by a later `enqueue`/`close`, so
async producers work (not just fully-buffered body streams). On top of
that:

- `WritableStream(sink)` — `start`/`write`/`close`/`abort`, `getWriter()`.
- `ReadableStream.pipeTo(writable)` — read loop into a writable, closing
  at end; `pipeThrough(transform)` returns the transform's readable.
- `TransformStream(transformer)` — a `writable` feeding
  `transformer.transform(chunk, controller)` and a `readable` of the
  results (`enqueue`/`terminate`).
- `ReadableStream.tee()` — two readables that each get every chunk.

Verified (STREAM_TEST): `pipeTo` from an async-producing source collects
`abc`; a `pipeThrough` upcase transform yields `XY`; `tee` gives both
branches `12`.

## Known limits

- No backpressure / `highWaterMark` / queuing strategies; `pull` is called
  but not awaited. `tee` buffers the source fully.
- The body is buffered fully before delivery (the kernel fetch reads the
  whole body), so `response.body` streams an already-complete buffer
  rather than incremental network chunks.
- `TextDecoder` is UTF-8 only (no other labels), non-streaming (no partial
  multibyte carry across `decode()` calls).
- XHR `responseType='arraybuffer'` isn't wired to `bodyBytes` yet (fetch
  is the binary path).

## Test

```
make ... BROWSER_EXTRA="-DSTREAM_TEST" \
     EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DTKAPP_BOOT -DTKAPP_BROWSER" iso
# serve add.wasm on 127.0.0.1:8099 (guest sees 10.0.2.2:8099 via SLIRP),
# boot with -netdev user -device e1000; serial shows [strm] ... ALL PASS
```
