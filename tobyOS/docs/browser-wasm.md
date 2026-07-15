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
[wasmjs] mem.grow(2) old pages=1 ; JS grows wasm linear memory
[wasmjs] old buffer detached? byteLength=0
[wasmjs] post-grow buflen=196608 ; 1 -> 3 pages
[wasmjs] mem[0] survived grow=20 ; data preserved across the realloc
[wasmjs] grown region [100000]=77; new region writable, reflects into wasm
[wasmjs] i64_shl(1,62)=4611686018427387904 (bigint)   ; i64 exact past 2^53
[wasmjs] imp wasm->JS [8]=4242    ; module imports a JS-created Memory
[wasmjs] imp JS->wasm peek(12)=7777; shared both ways, grow works
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

- **`memory.grow` (JS + wasm-internal).** wasm3's `memory.grow` opcode and
  `WebAssembly.Memory.prototype.grow(n)` both drive the same
  `ResizeMemory` (a realloc that preserves contents but *moves the base*).
  The native side caches the aliasing `ArrayBuffer` per instance keyed on
  its base pointer: `.buffer` keeps a stable identity between grows, and a
  grow **detaches** the old buffer (via `JS_DetachArrayBuffer`) so stale JS
  views fault instead of reading freed memory. A wasm-internal grow during
  `wasmCall` is caught by re-checking the base after the call
  (`wasm_sync_membuf`), so the required "re-read `mem.buffer` after a grow"
  pattern is safe. Standalone `new WebAssembly.Memory(...)` (used for
  imported memory, not yet linked) grows a plain copied `ArrayBuffer`.

- **Import layout in the trampoline.** wasm3's raw-call `sp[]` holds the
  result slots first (`nrets`), then the argument slots (`nargs`); each is
  a 64-bit slot with f32 in the low 32 bits.

- **SIMD (`v128`).** wasm3 0.5.2 shipped no SIMD, so it's added here (the
  `0xFD` prefix): `m3_compile.c` decodes the SIMD sub-opcode (LEB) and
  dispatches to per-shape compilers; `m3_exec.h` implements the ops. v128
  is a slot-only type (16 bytes = 4 of wasm3's 32-bit slots; it never uses
  the 64-bit register), so operands/results are read via slot pointers +
  `memcpy` (slots are 4-byte aligned — no aligned SSE loads), and scalar
  operands are forced to slots first (`PreserveRegisters`). Implemented:
  `v128.const`/`load`/`store`, `i8x16.shuffle`, splat / extract_lane /
  replace_lane for all shapes, integer + float add/sub/mul(/div), min/max,
  neg/abs, `v128.and`/`or`/`xor`/`andnot`/`not`, and i32x4/f32x4
  comparisons. Verified host-side (splat+add+extract, v128.const,
  f32x4.mul, memory load/store, shuffle) and end-to-end through the
  browser JS API. Not all 236 SIMD opcodes are present (e.g. widening/dot,
  saturating, conversions, bitmask, relaxed-SIMD); an unimplemented
  sub-opcode compiles to a clean "no compiler" reject rather than a crash.

- **`i64` ⇄ BigInt.** i64 values cross the JS boundary as `BigInt`
  (`JS_NewBigInt64` out, `JS_ToBigInt64` in), so values past 2^53 stay
  exact — both directions of the export call and the import trampoline.
  A plain Number is still accepted as an i64 argument (leniency), but a
  returned i64 is always a BigInt.

- **Imported memory.** wasm3 has one linear memory per *runtime* and
  leaves it unallocated for a module that *imports* its memory
  (`(import "env" "memory" ...)`), which then makes `m3_LoadModule` trip
  (its `InitDataSegments` requires an allocated memory even with zero data
  segments). So `wasmInstantiate` allocates `runtime->memory` from the
  import's declared limits (or the JS `Memory`'s requested size)
  **between** `m3_ParseModule` and `m3_LoadModule` — `InitMemory` then
  skips it (imported) and leaves the allocation intact. The prelude finds
  the `WebAssembly.Memory` in the import object, copies any
  pre-instantiate bytes into the runtime memory, and re-points that same
  object's `.buffer`/`.grow` at the instance — so it aliases wasm memory,
  shared both ways, grow included.

- The engine and the bridge must be compiled with identical struct-layout
  flags (both `-msse -msse2 -I third_party/wasm3`), since the bridge reads
  wasm3's private structs.

## Globals + tables

Exported **globals** are `WebAssembly.Global`-shaped objects on
`instance.exports`: `.value` reads the live global (`m3_GetGlobal`) and,
for a mutable global, writes it (`m3_SetGlobal`, throwing on an immutable
set). `WebAssembly.Global` also exists as a standalone holder. Types
marshal like call args (i32 number, i64 BigInt, f32/f64 number).

Exported **tables** are on `instance.exports` too: `.length` and
`.get(i)` returns a callable that does a JS-driven `call_indirect` through
slot `i` (`m3_GetTableFunction` + the shared `wasm_invoke`). Internal
`call_indirect` (C++ function pointers / vtables) already works inside the
engine regardless.

## Known limits

- `instantiateStreaming`/`compileStreaming` best-effort via `Response`;
  binary-safe network fetch of `.wasm` is a separate transport concern
  (tracked with the streaming/binary-fetch work).
- `WebAssembly.validate` is best-effort (container check, not a full
  bytecode validation pass).
- **Tables** are read/call only from JS — no `table.set`/`grow` or
  imported tables (wasm3's public API exposes only `m3_GetTableFunction`).
  Imported globals aren't linked either (like tables, wasm3 has no host
  hook); exported globals and a module's own globals work.
- **Interpreter, not a JIT.** Execution is wasm3's fast interpreter;
  there is no native-code tier (a JIT would be a whole codegen backend —
  a separate project, not a patch). Correct and fine for small/medium
  modules; a heavy compute kernel runs slower than a JIT'd browser.

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
