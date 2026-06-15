# busybox — Linux-ABI compatibility demo (Track B / milestone B2)

This directory hosts an **opt-in, third-party** musl-static busybox binary used
to demonstrate tobyOS's Linux x86-64 binary compatibility layer (see
`src/syscall.c` `linux_syscall()` and `docs/win10-gap.md`). The binary itself is
**not committed** (it is GPLv2 and ~1.1 MB — see `.gitignore`); only this README
and the build/test wiring live in the repo.

## What it proves

busybox is a real, unmodified Linux binary built against **musl libc**. When
present, tobyOS bundles it into the initrd as `/bin/busybox` (branded
`EI_OSABI=Linux` at staging time so the loader runs it under the Linux ABI
personality — the binary's code is byte-for-byte untouched). The
`-DLINUXBB_BOOT` harness then runs a battery of applets
(`echo/true/uname/pwd/cat/wc/stat`) and asserts each exits 0.

## How to obtain it

```sh
# 1) STATIC busybox (B2/B3/B4 demos) -- any static musl/uClibc busybox works.
curl -L -o programs/busybox/busybox \
    https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox

# 2) DYNAMIC busybox + the musl loader (B5 demo). The Alpine minirootfs ships
#    a PIE busybox (PT_INTERP=/lib/ld-musl-x86_64.so.1, NEEDED libc.musl) plus
#    the loader (which IS libc in musl):
curl -L -o /tmp/alpine.tgz \
    https://dl-cdn.alpinelinux.org/alpine/v3.19/releases/x86_64/alpine-minirootfs-3.19.1-x86_64.tar.gz
tar xzf /tmp/alpine.tgz -C /tmp ./bin/busybox ./lib/ld-musl-x86_64.so.1
cp /tmp/bin/busybox            programs/busybox/busybox-dyn
cp /tmp/lib/ld-musl-x86_64.so.1 programs/busybox/ld-musl-x86_64.so.1

# 3) MULTI-DSO demo (B7): a real binary that loads a SEPARATE shared library
#    beyond libc. Alpine `file` -> libmagic.so.1 + libc. Running `file
#    --version` makes ld-musl file-backed-mmap libmagic.so.1.
curl -L -o /tmp/file.apk \
    https://dl-cdn.alpinelinux.org/alpine/v3.19/main/x86_64/file-5.45-r1.apk
curl -L -o /tmp/libmagic.apk \
    https://dl-cdn.alpinelinux.org/alpine/v3.19/main/x86_64/libmagic-5.45-r1.apk
tar xzf /tmp/file.apk     -C /tmp usr/bin/file
tar xzf /tmp/libmagic.apk -C /tmp usr/lib/libmagic.so.1.0.0
cp /tmp/usr/bin/file                 programs/busybox/file
cp /tmp/usr/lib/libmagic.so.1.0.0    programs/busybox/libmagic.so.1
```

That's it — the Makefile's initrd rule detects these files, copies them in
(the dynamic loader to `/lib/`), and brandelf's the executables automatically.
No manual branding needed.

## How to run the demo

```sh
# from PowerShell on the build host (see the build-env notes):
make iso EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DLINUXBB_BOOT"   # static busybox battery
make iso EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DLINUXDYN_BOOT"  # DYNAMIC busybox (B5)
# then boot headless and grep the serial log for the verdict:
#   [LXBB]  VERDICT: PASS pass=10/10
#   [LXDYN] VERDICT: PASS pass=7/7   (real ld-musl dynamic linking)
```

## Status / scope

- **Static (B2/B3/B4):** echo, true, uname -a, pwd, cat, wc, stat, ls, ls -la
  (real file I/O + Linux `struct stat` + getdents64 + signals).
- **Dynamic (B5):** the same applets run through the **real musl `ld.so`** — the
  kernel loads the PIE busybox + the `ld-musl` interpreter (which IS libc), and
  the loader self-relocates + relocates busybox + resolves symbols. Works
  because busybox's only DSO is the kernel-loaded interpreter.
- **Multi-DSO (B6 file-backed mmap + B7 end-to-end):** `file --version` is a real
  binary that depends on a *separate* shared library, `libmagic.so.1`. ld-musl
  opens it and **file-backed-mmaps** its segments (the loader maps the whole span,
  then MAP_FIXED-maps each segment at the lib's 16 TiB base + offset), relocates
  it, and resolves its symbols — `[LXMULTI] VERDICT: PASS`. (Surfaced + fixed a
  latent kernel bug: `mmap.c`'s `PAGE_MASK` truncated addresses >= 4 GiB.)
