# WebAssembly (wasm3) — engine + browser JS API

Brings a real WebAssembly runtime to tobyOS and exposes it through the
browser's `WebAssembly` JS API, so web pages can compile, instantiate, and
call `.wasm` modules — including modules that import JS functions and share
linear memory with JS. Backend is the vendored **wasm3 0.5.2** interpreter
(MIT), built freestanding on the libtoby userland.

## What works

Two proven layers:

**1. Engine (`/bin/wasmtest`).** A hand-encoded module exercising the core
paths, verified in QEMU (`-DWASM_SELFTEST`):

```
[wasm] wasm3 0.5.2: 156-byte module
[wasm] add(7,35) = 42          ; i32.add
[wasm] mem_sum(20,22) = 42     ; i32.store/load through linear memory
[wasm] call_host(6,7) = 84     ; module calls a C host import (env.host_mul)
[wasm] fmul(1.5,2.0) = 3.0     ; f32.mul
[wasm] mem[0] = 20, memsize = 65536
[wasm] engine self-test: ALL PASS
[boot] WASM: wasmtest (pid=2) exit=0 (PASS)
```

**2. Browser JS API.** A page that runs `WebAssembly.instantiate(bytes,
importObject)`, calls the exports, invokes a **JS** host import from wasm,
and reads exported linear memory as a live `ArrayBuffer`. Rendered
on-screen ("WASM: ALL PASS") and on the serial console:

```
[wasmjs] add(7,35)=42
[wasmjs] mem_sum(20,22)=42
[wasmjs] call_host(6,7)=84       ; wasm called back into a JS function
[wasmjs] fmul(1.5,2.0)=3
[wasmjs] mem[0]=20 (buflen=65536); JS read wasm linear memory
[wasmjs] RESULT: ALL PASS
```

## Pieces

- `third_party/wasm3/` — wasm3 0.5.2 core: parse/compile/exec/env/bind/
  code/function/info/module + headers. The WASI/uvwasi/tracer/meta host
  APIs are **not** vendored — imports/exports come through the JS bridge,
  not WASI. Built `-msse -msse2` (wasm's f32/f64 returns need the SSE ABI).

- libtoby freestanding gap-fills wasm3 needs: `strtoull` (full unsigned
  range; `strtoul` now delegates to it), `copysignf`, `rintf`.

- `libtoby/src/wasm_bridge.c` + `toby/wasm_bridge.h` — a thin accessor over
  wasm3's **private** `M3Module`/`M3Function`/`M3FuncType` layout so the
  browser can enumerate a module's function imports (module/field name +
  signature) and its exported functions/memory **without** pulling wasm3's
  private `u8`/`u32` typedefs into the large browser TU. Compiled with
  `-I third_party/wasm3`; nothing else in the tree touches the internals.

- `programs/user_gui_browser/main.c`:
  - Native primitives on `__dom`: `wasmInstantiate(bytes, resolver)`,
    `wasmExportNames`, `wasmMemName`, `wasmCall(id, name, args)`,
    `wasmMemBuffer`, `wasmFree`.
  - `wasm_import_tramp` — one C trampoline linked (via
    `m3_LinkRawFunctionEx` with per-import userdata) for every JS-resolved
    function import; marshals wasm values ⇄ `JSValue` and calls back into
    JS.
  - `WASM_PRELUDE` (in `JS_PRELUDE`) builds
    `WebAssembly.{Module,Instance,Memory,Table,instantiate,compile,
    instantiateStreaming,compileStreaming,validate,*Error}`. Exports become
    JS wrapper functions that call `wasmCall`; imports resolve through the
    `importObject`; exported memory is a `{ buffer }` whose `buffer` is a
    **getter** returning a live `ArrayBuffer` that *aliases* wasm linear
    memory (writes reflect into wasm).
  - `wasm_free_for_context` runs at JS teardown to release a page's
    instances.

- `programs/user_wasmtest/` — `/bin/wasmtest` + embedded `wasm_clip.h`
  (hand-encoded module). Its `main.c` builds `-msse -msse2` for the f32
  `m3_CallV`/`m3_GetResultsV` variadic boundary.

- kernel `WASM_SELFTEST` boot hook spawns `/bin/wasmtest`.

## Design notes / gotchas

- **SSE ABI at every float boundary.** wasm3 returns f32/f64 in XMM
  registers. Anything that crosses a variadic float boundary to it
  (`/bin/wasmtest`'s `main.c`) or manipulates the results must be built
  `-msse -msse2`, or floats silently come back as 0. The browser TU is
  already `-msse`.

- **Linear memory is lazy.** wasm3 allocates a module's memory on first
  use, so `wasmMemBuffer` can return null right after instantiate. The
  prelude makes `memory.buffer` a getter that fetches the live buffer on
  access (after an export has run), rather than snapshotting at
  instantiate time.

- **Import layout in the trampoline.** wasm3's raw-call `sp[]` holds the
  result slots first (`nrets`), then the argument slots (`nargs`); each is
  a 64-bit slot with f32 in the low 32 bits.

- The engine and the bridge must be compiled with identical struct-layout
  flags (both `-msse -msse2 -I third_party/wasm3`), since the bridge reads
  wasm3's private structs.

## Known limits

- No `memory.grow()` — the aliasing `ArrayBuffer` assumes a stable base.
- `instantiateStreaming`/`compileStreaming` best-effort via `Response`;
  binary-safe network fetch of `.wasm` is a separate transport concern.
- `i64` marshals through JS numbers (no BigInt yet).
- `WebAssembly.validate` is best-effort (container check, not a full
  bytecode validation pass).
- No WASM tables/`call_indirect` from JS, globals, or SIMD.

## On-screen browser test

A gated home page (`-DWASM_BROWSER_TEST` via
`BROWSER_EXTRA=-DWASM_BROWSER_TEST`, mirroring the kernel `*_SELFTEST`
hooks) replaces the browser's home page with a WebAssembly self-test that
instantiates an inline module, calls every export, invokes a JS host
import, and reads exported memory — logging `[wasmjs]` lines to serial and
"WASM: ALL PASS" to the page. Not defined in normal builds.

## Build / test

```
# engine self-test at boot
make ... EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DWASM_SELFTEST" iso
#   -> "[boot] WASM: wasmtest (pid=2) exit=0 (PASS)"

# on-screen browser JS API test (browser auto-launched)
make ... BROWSER_EXTRA="-DWASM_BROWSER_TEST" \
     EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DTKAPP_BOOT -DTKAPP_BROWSER" iso
#   -> page shows "WASM: ALL PASS"; serial has [wasmjs] RESULT: ALL PASS
```

Note: `EXTRA_CFLAGS` changes alone do not recompile `kernel.c` (it's not a
prerequisite of the flag); `touch src/kernel.c` when toggling
`-DWASM_SELFTEST`.
