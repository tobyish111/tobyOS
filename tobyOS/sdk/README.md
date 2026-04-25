# tobyOS Developer SDK 1.0

This is the official tobyOS Software Development Kit. It contains
everything you need to build, package, and install third-party
applications for tobyOS, **outside** the OS source tree.

```
sdk/
├── VERSION                     SDK version string
├── README.md                   you are here
├── include/                    public C headers (libtoby + tobyos_* + toby/)
├── lib/                        crt0.o, libtoby.a, libtoby_gui.a, program.ld
├── tools/                      pkgbuild + tobycc/tobyld convenience wrappers
├── docs/                       full API and packaging reference
├── templates/                  starter projects: cli/, gui/, service/
└── samples/                    full working examples: hello_cli/, hello_gui/, notes_app/
```

> Note: `include/` (libtoby/tobyos_*/abi headers), `lib/*.a`, `lib/crt0.o`,
> and `tools/pkgbuild` are populated by the `make sdk` target in the OS
> build tree. The committed `sdk/` directory contains the SDK-authored
> headers, linker script, samples, templates, docs, and shell wrappers.

## Quick start

```bash
# 1. Build the SDK (from the OS source tree):
make sdk
# => build/sdk-1.0.0/                  (assembled SDK)
# => build/tobyOS-sdk-1.0.0.tar.gz     (distributable tarball)

# 2. Point at the SDK and copy a template:
export TOBYOS_SDK=$PWD/build/sdk-1.0.0
cp -r $TOBYOS_SDK/templates/cli my_app
cd my_app

# 3. Edit src/main.c and tobyapp.toml, then:
make help        # show all available targets
make             # build build/myapp.elf
make package     # build + produce build/myapp.tpkg

# 4. Install on a running tobyOS instance:
#    - copy build/myapp.tpkg to /repo/myapp.tpkg on the target
#    - in the tobyOS shell:  pkg install /repo/myapp.tpkg
#    The launcher refreshes; the new app shows up under [Apps].
```

> The template Makefile errors loudly if you forget to set `TOBYOS_SDK`.
> `make` here builds for the **tobyOS target** (freestanding x86_64 ELF) --
> there is no `make run` because the binary is not a host executable;
> install it on a tobyOS instance to run it.

## What's where

| You want to... | Read |
|---|---|
| Understand the architecture | [docs/overview.md](docs/overview.md) |
| Write the manifest | [docs/manifest.md](docs/manifest.md) |
| Call a syscall directly | [docs/syscalls.md](docs/syscalls.md) |
| Use libc-style I/O | [docs/libc.md](docs/libc.md) |
| Open a window / draw widgets | [docs/gui.md](docs/gui.md) |
| Build / install a `.tpkg` | [docs/packaging.md](docs/packaging.md) |
| Write a long-lived service | [docs/services.md](docs/services.md) |

## C and C++

Every SDK surface (libc, GUI, notifications, packaging, samples) is
**C**. Two optional header-only C++ wrappers ship at
`include/toby/gui.hpp` and `include/toby/notify.hpp`; they're fully
opt-in and add no runtime dependencies. You can write SDK apps in C
without touching either.

## Versioning

The SDK and the kernel ABI version together. SDK 1.0 targets
`ABI_VERSION = 1` (the "M25A frozen" ABI). When the kernel grows new
syscalls (M31's `SYS_NOTIFY_*`, future M32+ additions), the SDK's
`include/tobyos/abi/abi.h` is updated and the SDK minor version bumps.

## License

Same license as the rest of tobyOS.
