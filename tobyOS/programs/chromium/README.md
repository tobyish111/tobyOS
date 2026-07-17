# Chromium bring-up (Track B / Linux, M0)

This directory holds the **opt-in, uncommitted** Chromium payload used to
generate the M0 syscall/DSO gap list. See
[`docs/chromium-bringup-m0.md`](../../docs/chromium-bringup-m0.md).

## What this is

`chrome-headless-shell` is the standalone, BSD-licensed **headless Chromium**
binary from Google's [Chrome-for-Testing](https://googlechromelabs.github.io/chrome-for-testing/)
channel. It is the real, unmodified engine (V8 + Blink), not a re-implementation.
It bundles **SwiftShader** (`libvk_swiftshader.so`, `libEGL.so`, `libGLESv2.so`)
so headless rendering needs no GPU driver.

- `PT_INTERP` = `/lib64/ld-linux-x86-64.so.2` (glibc — already runs on tobyOS,
  see the `linux-abi-compat-b1` memory: B26/realtool/realtools ran real
  glibc-dynamic GNU Binutils, bash, CPython).
- Directly `NEEDED`s 28 shared objects; only ~5 are glibc. The rest
  (glib/gio/gobject, nspr/nss, atk/atspi, dbus, X11 stack, gbm, expat, udev,
  asound, xkbcommon, gcc_s) plus their transitive closure form the **DSO tier**
  of the gap list and must be staged into a sysroot.

## How to get it

```
bash programs/chromium/build.sh      # fetches ~120 MB zip, extracts ~262 MB tree
```

Then build + boot the gap-list run:

```
bash logs/chromium-m0.sh
```

Nothing here is committed (`.gitignore` excludes the payload). This mirrors the
existing opt-in real-binary milestones (busybox, realtool, realpython, realcc).
