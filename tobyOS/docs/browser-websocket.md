# Browser — WebSocket (RFC 6455)

Branch `browser-websocket`, stacked on `browser-woff2`. Full-duplex
sockets for real-time web apps: `new WebSocket(url)` works in page JS
for both `ws://` (plain TCP) and `wss://` (over the TLS 1.3 stack,
certificate validation included). Events dispatch through the existing
JS pump, so the UI never blocks — the same non-blocking contract as
async HTTP (stage 12D).

## What shipped
- **`src/ws.c` + `include/tobyos/ws.h`** — a kernel-side RFC 6455
  client on a ring-0 worker (the `proc_create_kernel` +
  `tcp_yield_wait` model from `http_async.c`). The worker owns each
  connection's transport end to end:
  - **Opening handshake**: HTTP/1.1 Upgrade GET with a random
    `Sec-WebSocket-Key` (base64 of 16 `rng_fill` bytes); the 101
    response's `Sec-WebSocket-Accept` is verified against
    base64(SHA-1(key + magic GUID)) using BearSSL's SHA-1 (vendored in
    13H). Bytes past the response header block are fed straight into
    the frame parser.
  - **Framing**: client→server frames are always masked (4-byte XOR
    key per frame); 7/16/64-bit payload lengths both ways; incoming
    frames are unmasked-if-masked, fragmented messages reassembled
    (text/binary + continuation), pings auto-answered with pongs,
    Close frames echoed (peer's code preserved) — then the transport
    is RST-dropped (`tcp_abort`/new `tls_abort`), never lingering in
    the TIME_WAIT wait that would stall the worker (the 13H lesson).
  - **Idle-proof receive**: the worker only calls `tcp_recv`/`tls_recv`
    when the transport reports buffered bytes (`tcp_poll_flags` / new
    `tls_poll_flags`, which includes TLS's decrypted-but-undelivered
    buffer). An idle connection never sits in a timed read — so an
    idle read can never kill the socket (the 12D timeout gotcha), and
    a mid-record TLS timeout can't desync framing.
  - Decoded messages queue per handle (256 KiB/message, 512 KiB/queue
    caps); outgoing messages are masked+framed at send time into a per
    handle TX queue the worker drains. Only the worker touches the
    connection, so syscalls never race it inside a transport wait.
- **ABI 177–181** (`SYS_WS_OPEN/POLL/RECV/SEND/CLOSE`, `abi.h`):
  OPEN queues connect+handshake and returns a handle immediately; POLL
  reports state (constants match DOM readyState) + head-of-queue
  message info; RECV pops one message; SEND enqueues text/binary;
  CLOSE sends a Close frame (code preserved) and releases the handle.
  Slots owned by exited processes are reaped lazily.
- **`tls.c`**: `tls_abort()` (close_notify + RST, non-blocking) and
  `tls_poll_flags()` (readiness incl. buffered plaintext) — both
  needed by any long-lived-connection owner.
- **Browser (`main.c`)**: a `WebSocket` class in the JS prelude —
  `onopen/onmessage/onclose/onerror` + `addEventListener`, `send()`,
  `close([code])`, `readyState` + the CONNECTING/OPEN/CLOSING/CLOSED
  constants, `event.data`/`code`/`wasClean`. C prims
  (`wsOpen/wsSend/wsClose`) bridge to the syscalls; the pump polls
  each live handle per tick and fires events on the pinned JS object
  (open transition, queued messages, close with code — terminal
  events detach the slot before firing, so handlers can reopen).
  Tab teardown closes handles with code 1001 (going away).

## Verified (QEMU)
- **ws:// (local echo)**: the `/ws` page opens
  `ws://10.0.2.2:8077/echo`, sends two messages, and the screenshot
  shows `onopen fired` → `server sent: echo: hello from tobyOS`
  (deliberately sent by the server as **two fragments** — reassembly
  proven on screen) → `server sent: echo: second message 42` →
  `onclose code=1000 wasClean=true`. The server log confirms the
  kernel's frames: masked text ×2, a **masked pong** answering the
  server's ping (auto-pong), and a Close frame carrying code 1000.
- **wss:// (real internet)**: `wss://echo.websocket.org/` over
  TLS 1.3 — serial shows `[tls] certificate chain + CertificateVerify
  OK` on the WebSocket connection; the page renders the server
  greeting and the echoed `tobyOS says hi over wss`, then a clean
  close. Cert validation runs on wss exactly as on https.
- Regression: `/js2` (timers + fetch + click dispatch through the same
  pump) unchanged.

## v1 limits
- `send()` takes strings (UTF-8 text frames); binary frames are
  received and delivered as byte-strings, but there's no
  ArrayBuffer/Blob `binaryType` support.
- Close is app-detach: `close()` sends the Close frame and frees the
  handle without waiting for the peer's reply; after the WS-level
  close (either side), TCP is RST-dropped rather than FIN-closed, so
  the worker never blocks (peers see a complete WebSocket closing
  handshake, then an abortive TCP close).
- No `Sec-WebSocket-Protocol` (subprotocols) or extensions
  (permessage-deflate is declined by never offering it; frames with
  RSV bits set fail the connection).
- 4 concurrent connections; 256 KiB max message; one worker services
  all connections serially (a slow handshake briefly delays others).
- The URL must be absolute `ws://`/`wss://` (no relative resolution).
