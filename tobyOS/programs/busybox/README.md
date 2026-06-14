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
# A prebuilt musl-static busybox (x86-64). Any static musl/uClibc busybox works.
curl -L -o programs/busybox/busybox \
    https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox
```

That's it — the Makefile's initrd rule detects `programs/busybox/busybox`,
copies it in, and brandelf's it automatically. No manual branding needed.

## How to run the demo

```sh
# from PowerShell on the build host (see the build-env notes):
make iso EXTRA_CFLAGS="-DFAST_BOOT -DQUICK_BOOT -DLINUXBB_BOOT"
# then boot headless and grep the serial log for the verdict:
#   [LXBB] VERDICT: PASS pass=7/7
```

## Status / scope

- **Works:** echo, true, uname -a, pwd, cat, wc, stat (real file I/O + the
  Linux `struct stat` translation).
- **Deferred to B3:** `ls` and friends — they need `getdents64`, which requires
  a directory-fd abstraction tobyOS doesn't have yet. Dynamic (non-static)
  Linux binaries also need a Linux `ld.so` + `/lib`, which is a later stage.
