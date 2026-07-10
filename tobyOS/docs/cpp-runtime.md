# Freestanding C++ runtime for tobyOS userland

Branch `cpp-runtime`, stacked on `browser-h264`. tobyOS userland now
runs REAL C++ (`-fno-exceptions -fno-rtti`, C++17): classes, virtual
dispatch, templates, `new`/`delete`, global constructors/destructors,
and function-local statics — proven by `/bin/cpptest`, a clang++-built
TobyTK program. This is the gate for High-profile H.264 (openh264)
and AVIF (libgav1), both C++ codebases.

## What shipped
- **`libtoby/src/cxxrt.cpp`** — the C++ runtime over the C library:
  - `operator new/new[]/delete/delete[]` (plain, sized, nothrow) over
    the libtoby heap; allocation failure aborts (nothing to throw
    with exceptions off).
  - `__cxa_pure_virtual` (loud abort), `__dso_handle`.
  - `__cxa_guard_acquire/release/abort` — function-local static
    guards, Itanium ABI layout, single-threaded semantics (matching
    the rest of libtoby).
  - `__cxa_atexit` — static-object destructors in a bounded table,
    drained newest-first via one `atexit()` hook.
- **`libtoby/include/new`** — placement/nothrow `operator new`
  declarations (placement forms inline, zero runtime).
- **Global constructors** — `__libtoby_init` (init.c) walks
  `__init_array_start..end` before `main` and registers a reverse
  `__fini_array` walk with `atexit()`. The bounds are **weak**
  symbols: every existing C program's linker script doesn't define
  them, the references resolve to 0, and the walks are no-ops — no
  linker-script churn across the tree. A C++ program's `program.ld`
  defines them with `KEEP(.init_array/.fini_array)` sections (see
  `programs/user_cpptest/program.ld`, the template for C++ programs).
- **Makefile** — `CXX` derived from `CC` (`clang` → `clang++`, so the
  `TMP='C:\t'` wrapper carries through); `LIBTOBY_CXXFLAGS` = the
  freestanding C base minus `-std=c11` plus
  `-std=c++17 -fno-exceptions -fno-rtti -nostdinc++`;
  `LIBTOBY_CPP_PROGRAM_RULES` template links `main.cpp` programs
  against crt0 + libtoby.a exactly like C programs.
- **`programs/user_cpptest`** (`/bin/cpptest`) — the acceptance test,
  launchable via the TKAPP harness (`-DTKAPP_BOOT -DTKAPP_CPPTEST`).
  Note freestanding C++ mangles `main`; programs declare it
  `extern "C"`.

## Verified (QEMU, TKAPP_CPPTEST boot)
Seven checks, each printed as a `[cpp]` serial marker and rendered
PASS/FAIL in the app's TobyTK table:
1. global constructors (`.init_array` walk; 2 objects, right values);
2. virtual dispatch through base pointers from `operator new`
   (vtables + overrides + delete through virtual dtor);
3. `operator new[]`/`delete[]`;
4. templates (function + class);
5. function-local static (`__cxa_guard_*` — constructor exactly once
   across three calls);
6. placement new + explicit destructor;
7. nothrow new.
Serial ends with `[cpp] ALL PASS 7/7`; pressing `q` exits the app and
the static destructor's `[cpp] static destructor ran (cxa_atexit) OK`
marker fires on the way out.

## v1 limits
- No exceptions, no RTTI (by policy — the flags are part of
  `LIBTOBY_CXXFLAGS`); `dynamic_cast`/`typeid`/`throw` don't build.
- No hosted C++ standard library — `<new>` only. C headers have
  `extern "C"` guards and work directly. Vendored C++ codebases
  bring their own containers or get headers added as needed (the
  openh264/libgav1 step).
- Static-linked programs only (ld-toby.so doesn't run DT_INIT_ARRAY
  for shared objects yet).
- `__cxa_guard` is single-threaded, like the rest of libtoby (malloc
  is unlocked too); revisit when userland threads land.
- 64 `__cxa_atexit` slots.
