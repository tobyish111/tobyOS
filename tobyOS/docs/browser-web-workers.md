# Web Workers

Branch `browser-utf8-text` (Chrome-parity push item 3). `new Worker(url)`
now runs a script in an isolated JS world with a `postMessage` channel.

## Model
The engine is single-threaded and cooperative (QuickJS, no JIT/threads),
so a Worker is **a second `JSContext` on its own `JSRuntime`, pumped from
the main loop** — concurrency by interleaving, not parallelism. That
gives correct Worker *semantics* (isolation, async message API, no DOM
access, its own timers/microtasks) without shared-state hazards. Heavy
compute in a worker still blocks the UI while it runs a message handler;
what you get is isolation + the offload programming model, not a second
core.

## Pieces (`programs/user_gui_browser/main.c`)
- **Worker table** `g_workers[WORKER_MAX]`: each entry owns a
  `JSRuntime`+`JSContext`, the owning tab's context, the main-side
  `Worker` object, two JSON message ring buffers (main→worker,
  worker→main), and a worker-local timer array.
- **C primitives** (main context): `workerNew(url, obj)` synchronously
  fetches the script (`sys_http_fetch`), creates the runtime/context,
  installs the worker global bindings + `WORKER_PRELUDE` + the script,
  and returns a handle; `workerPost(handle, json)` queues main→worker;
  `workerTerminate(handle)` tears the worker down.
- **C primitives** (worker context): `__w.postMain(json)` queues
  worker→main; `__w.timer/untimer` worker-local timers;
  `__w.import(url…)` synchronous `importScripts`.
- **Messages cross as JSON strings** — all `JSON.stringify`/`parse`
  stays in JS (each side has a `__deliver(json)` method the C pump
  calls), so C only shuttles opaque strings between contexts
  (structured-clone approximation).
- **`worker_pump()`** in the main loop: per worker, delivers queued
  main→worker messages (→ `self.onmessage`), runs due worker timers,
  drains the worker's pending promise jobs, then delivers queued
  worker→main messages (→ `worker.onmessage`).
- **Prelude**: main side gets a `Worker` class
  (`postMessage`/`terminate`/`onmessage`/`addEventListener`);
  `WORKER_PRELUDE` gives the worker global `self`/`globalThis`,
  `postMessage`/`onmessage`/`close`, `importScripts`,
  `setTimeout`/`setInterval`/`clearTimeout`, `queueMicrotask`, and
  `console`. `js_teardown` reaps a page's workers on navigation.

## Verified
A page spawns `new Worker('/worker.js')`; the round-trip on screen:
1. worker boots and posts `{type:'ready'}` → main logs it;
2. main posts `{type:'sum', nums:[1..10]}`; the worker sums and replies
   `{type:'sum', value:55}` → **compute offload correct**;
3. main posts `{type:'ping'}`; worker replies `{type:'pong'}`;
4. the worker's own `setInterval` posts `{type:'tick', n}` twice → main
   receives both, then calls `worker.terminate()`.

## Limits / follow-ups
No real parallelism (cooperative); no `fetch`/`XHR` inside a worker yet
(only `importScripts`); no `SharedWorker`, `MessageChannel`/`MessagePort`,
transferables (`ArrayBuffer` transfer — messages are JSON-cloned so
functions/typed-arrays/cyclic graphs don't survive), `error`/
`messageerror` events, or module workers (`type:'module'`).
