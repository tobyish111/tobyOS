# SDK overview

An SDK app is a freestanding 64-bit ELF that:

1. Talks to the kernel via the **frozen syscall ABI** (`abi.h`).
2. Optionally links **libtoby** (POSIX-shape libc) + **libtoby_gui**
   (GUI toolkit) for ergonomics.
3. Ships as a **`.tpkg`** package; the in-kernel `pkg install` command
   unpacks it under `/data/`, registers any launcher entry, and
   refreshes the desktop menu.

## Layered surface

```
         ┌─────────────────────────────────────────────────────┐
         │          your app (src/main.c, src/*.c)             │
         └────────────┬────────────────────────────┬───────────┘
                      │                            │
                      │ <toby/gui.h>               │ <stdio.h>, <unistd.h>,
                      │ <toby/gui.hpp>             │ <fcntl.h>, <string.h>,
                      │                            │ <tobyos_notify.h>,
                      │                            │ <tobyos_slog.h>, ...
                      ▼                            ▼
         ┌──────────────────────────┐ ┌──────────────────────────┐
         │   libtoby_gui.a (toolkit)│ │   libtoby.a (libc-shape) │
         └────────────┬─────────────┘ └────────────┬─────────────┘
                      │                            │
                      │     <tobyos/abi/abi.h>     │
                      ▼                            ▼
                ┌──────────────────────────────────────┐
                │  syscall instruction (ring 3 -> 0)   │
                └────────────────────┬─────────────────┘
                                     │
                                     ▼
                          tobyOS kernel (ring 0)
```

You can pick where to enter:

- **Highest level** -- include `<toby/gui.h>`, write a callback-driven
  GUI app. See `samples/hello_gui/`.
- **POSIX-ish** -- include `<stdio.h>`, write CLI tools that look like
  Unix utilities. See `samples/hello_cli/`.
- **Raw syscalls** -- include `<tobyos/abi/abi.h>`, drop into inline
  assembly. Useful for the lowest-level tooling; almost never the
  right choice for an application.

## Static, non-PIE binaries

SDK apps are **statically linked**, **non-PIE** ELFs anchored at
virtual address `0x400000`. The canonical linker script
(`lib/program.ld`) does the work; the templates' Makefiles include it
via `-T`. The kernel ELF loader (`src/elf.c`) honours the program
headers and never relocates the load address.

This means you don't need to think about position-independent code,
PLT, GOT, or a dynamic linker. The dynamic option still exists in the
kernel (it loads `programs/c_dynhello` via `/lib/ld-toby.so`), but
the SDK does not document or support it for first-party apps.

## Lifecycle of an SDK app

```
  +-----------+           +----------+
  | manifest  |  pkgbuild |  .tpkg   |
  | + ELF     | --------> |          |
  +-----------+           +----------+
                                |
                          (transfer to target)
                                |
                                v
                          +-----------+
                          | pkg       |   in-kernel installer
                          | install   |   (src/pkg.c)
                          +-----------+
                                |
                                v
                +-----------------------------------+
                |  files written under /data/apps/  |
                |  + APP entry registered in        |
                |    /data/apps/*.app launcher list |
                +-----------------------------------+
                                |
                                v
                        compositor refresh
                  (start menu shows "Your App")
                                |
                                v
                          user clicks ->
                          kernel ELF load ->
                          _start (crt0.o) ->
                          __libtoby_init ->
                          main(argc, argv, envp)
```

`pkg remove <name>` walks the install record, unlinks every file the
package owns, and refreshes the launcher again. Nothing outside
`/data/` is ever touched by the package manager.
