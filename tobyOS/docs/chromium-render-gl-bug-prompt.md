# Task: make Chromium's GL (SwANGLE) initialize headless on tobyOS so it RENDERS — solve the one remaining render-pipeline bug

You are picking up a **live, successful Chromium bring-up** at its final wall.
A previous agent got real, unmodified `chrome-headless-shell` (V8 + Blink,
running on tobyOS's Linux ABI / Track B) from a **77-second startup hang** all
the way to **~7–9 s of real render-pipeline execution** — by fixing two genuine
kernel bugs (timed-futex scheduler starvation; a 256-entry per-proc VMA cap) and
filling a few syscalls. **The Track B kernel ABI is now proven sufficient for
Chromium.** Exactly ONE wall remains, and it is entirely in **chrome's own GL
stack (ANGLE + SwiftShader), not the kernel**: chrome's software GL fails to
initialize headless, and a downstream **NULL GL-dispatch call crashes it**.

**Your job: get SwANGLE's `eglInitialize` to succeed (or make chrome not need
it) so a headless render COMPLETES** — `--dump-dom` prints `<h1>tobyOS</h1>`, and
then `--screenshot` writes a PNG that matches the same chrome build on the host.
This is bounded, well-diagnosed, and does NOT require writing a GPU driver or a
GL implementation. SwiftShader is a complete *software* Vulkan renderer — the
only thing blocking it is a Window-System-Integration (WSI) extension check.

---

## THE PRIME DIRECTIVE: reuse what tobyOS + Chromium already ship — add the smallest shim

Before writing anything new, find the existing implementation and reuse it. The
whole arc's wins came from finding a wall already solved. Concretely for THIS bug:

- **tobyOS's AF_UNIX socket infrastructure** — `src/socket.c`
  (`SOCK_KIND_UNIX`, `sock_unix_pair`/`sock_unix_send`/`sock_unix_recv`),
  `src/syscall.c` (`lx_socket`/`lx_bind`/`lx_listen`/`lx_accept`/`lx_connect`).
  If the fix is a minimal fake X server (see below), it connects over an AF_UNIX
  socket at `/tmp/.X11-unix/X0` — reuse this, don't add a new IPC.
- **Chromium's OWN bundled Vulkan stack** — in
  `programs/chromium/chrome-headless-shell-linux64/`: `libvulkan.so.1` (the
  Khronos loader, which *implements* the WSI surface extensions and is already
  built with XCB support), `libvk_swiftshader.so` (the SwiftShader ICD, a full
  software Vulkan), `vk_swiftshader_icd.json`, `libEGL.so`/`libGLESv2.so`
  (ANGLE). The X11 client libs `libxcb.so.1` + deps `libXau.so.6`/`libXdmcp.so.6`
  and `libX11.so.6` are already in `programs/chromium/sysroot/`. **You almost
  certainly do not need to add any library.**
- **Chrome's own flags/env** — the invocation is the `#ifdef CHROMIUM_BOOT`
  harness in `src/kernel.c` (argv + envp). Prefer a flag/env that makes ANGLE
  behave over patching a `.so`.
- **The in-tree instruments** — the `[linux] UNHANDLED` syscall logger + names,
  the recent-syscall ring + the **user-stack dump + `fault_count`** (dumped by
  `src/isr.c` on any fatal user fault), the `#ifdef CHROMIUM_BOOT` **scheduler
  heartbeat** (`src/sched.c`), and `-DLINUX_SYSCALL_TRACE` (the firehose;
  `TRACE=1 bash logs/chromium-m0.sh`). Keep them; extend as needed.

If something is genuinely missing, add the *smallest* shim that lets chrome's
existing code path work — the way the arc added `socketpair`, futex timeouts, and
`fcntl(F_GETFL)` rather than reimplementing Mojo / glibc / the fd table.

---

## Read first (this is a live effort with detailed, measured state)

- **`docs/chromium-bringup-m1.md`** — the full burn-down. **Slice 11 + "Slice 11
  GL deep-dive"** at the end describe THIS bug and everything already tried. Read
  them fully; do not repeat the disproven experiments.
- **Memory** (`C:\Users\tdude\.claude\projects\c--CustomOS\memory\`):
  `chromium-bringup.md` (read the "RENDER BRING-UP 2026-07-18" and "SLICE 11 GL
  DEEP-DIVE" entries FIRST), `linux-abi-compat-b1.md` (Track B),
  `tobyos-build-env.md` (how to build/run on this Windows machine).
- **`docs/chromium-bringup-render-prompt.md`** / `-m0.md` / `-agent-prompt.md` —
  the strategic framing (the compositor/windowing reuse targets are for M3, AFTER
  this render bug is solved).

---

## The bug, exactly (measured — do not re-derive)

With the scheduler-starvation and VMA-cap fixes in, chrome runs into its render
pipeline and dies at **~7 s** with:

```
*** EXCEPTION 14: Page Fault (in user mode) ***   rip=0x0000000000000000  err=0x14
```

`rip=0`, `err=0x14` (user-mode **instruction fetch** at address 0) = a **call
through a NULL function pointer** on the browser main thread. It is **downstream
of SwANGLE's GL init failing**, whose exact cause the trace pins:

```
vk_renderer.cpp:276 (VerifyExtensionsPresent): Extension not supported: VK_KHR_surface
vk_renderer.cpp:276 (VerifyExtensionsPresent): Extension not supported: VK_KHR_xcb_surface
ANGLE Display::initialize error 0: Internal Vulkan error (-7): A requested extension is not supported ... enableInstanceExtensions
eglInitialize SwANGLE failed with error EGL_NOT_INITIALIZED
```

**ANGLE's Vulkan backend requires the WSI instance extensions `VK_KHR_surface`
and `VK_KHR_xcb_surface` (X11 window surfaces) at instance creation.** In a
headless environment with no X server, they aren't available → `eglInitialize`
fails → chrome later calls a NULL GL dispatch pointer → crash.

### What is already established (measured; do NOT redo these)

- **ANGLE (`libGLESv2.so`) dlopens SwiftShader's ICD (`libvk_swiftshader.so`)
  DIRECTLY** (its `DT_NEEDED` is only libc/libpthread/libgcc_s/ld — Vulkan is
  dlopen'd at runtime), **bypassing `libvulkan.so.1`, the loader that implements
  WSI.** So both surface extensions are absent → both fail.
- **`--use-gl=angle --use-angle=vulkan` + env
  `VK_ICD_FILENAMES=/opt/chrome/vk_swiftshader_icd.json` routes ANGLE through the
  loader → `VK_KHR_surface` RESOLVES.** Only `VK_KHR_xcb_surface` then remains.
  (This is real progress and likely part of the final answer — keep it.)
- The bundled `libvulkan.so.1` **is** built with XCB WSI, and `libxcb.so.1` +
  `libXau.so.6` + `libXdmcp.so.6` are in the sysroot — **but the loader correctly
  FILTERS OUT `VK_KHR_xcb_surface` because there is no functional X server**
  (`xcb_connect` has nothing to connect to; env has no `DISPLAY`). ANGLE requires
  it regardless.
- **`VK_LOADER_DISABLE_INST_EXT_FILTER=1` REGRESSED it** (both surface extensions
  failed again) — not the override it appears to be.
- **Flags that did NOT route around GL** (all still hit the surface-ext wall):
  dropping `--enable-unsafe-swiftshader`, `--ozone-platform=headless`,
  `--disable-features=Vulkan`.

### The key insight

**SwiftShader is a complete software Vulkan renderer — it will render if the
instance is created.** The ONLY blocker is ANGLE's WSI *extension-presence*
check at instance creation. Chrome headless never actually creates an X11 window
surface, so the extension likely only needs to be **present**, not functional.
Get past `VerifyExtensionsPresent` and SwiftShader should produce a real
software GL context → chrome renders → the DOM dumps and `--screenshot` works.

---

## Solution paths (instrument-first; ranked by likely tractability)

Measure before building — three handed-down hypotheses were wrong last arc, each
caught by instrumenting. First, **trace exactly why the loader filters
`VK_KHR_xcb_surface`**: run `TRACE=1 bash logs/chromium-m0.sh` and look at what
`libvulkan.so.1` does right before the `VerifyExtensionsPresent` failure — does
it `connect()` an AF_UNIX socket to `/tmp/.X11-unix/X0`? read `DISPLAY`? `open`
an X auth file? `dlopen` libxcb and call `xcb_connect`? Instrument the relevant
syscalls (add a decoded-path logger to `openat`/`connect`, reusing the
`resolve_user_path` pattern) so the loader's WSI probe self-identifies. That
decides between:

1. **Make `xcb_connect` succeed → the loader stops filtering `VK_KHR_xcb_surface`
   (LEADING candidate — reuses tobyOS AF_UNIX).** The loader hides XCB WSI only
   because there is no X server. Set `DISPLAY=:0` in the harness envp, and stand
   up a **minimal fake X server on an AF_UNIX socket at `/tmp/.X11-unix/X0`** that
   just accepts the connection and answers the X11 **connection-setup handshake**
   (the documented `xConnClientPrefix` → `xConnSetup` reply with one screen).
   `xcb_connect` only needs the handshake to succeed — chrome never creates a
   real window — so a tiny stub server (or even an in-kernel AF_UNIX responder
   for that path) may be enough for the loader to advertise the extension. Reuse
   `src/socket.c`'s AF_UNIX machinery. This is the smallest shim that satisfies
   the *presence* check without any real display.

2. **Make ANGLE use a SURFACELESS/headless Vulkan path (no X11 surface at all).**
   ANGLE has surfaceless/pbuffer modes and can request `VK_EXT_headless_surface`
   instead of XCB. Investigate what makes ANGLE's `vk_renderer.cpp`
   `enableInstanceExtensions` choose X11 vs headless — it derives the WSI from the
   EGL platform / native display chrome's Ozone hands it. Look for: an ANGLE
   feature flag / env (`ANGLE_*`), an EGL platform attribute
   (`EGL_PLATFORM_SURFACELESS_MESA`, ANGLE's device platform), or a chrome switch
   that gives ANGLE a **null/headless native display** so it stops requesting
   XCB. (`--ozone-platform=headless` alone did not do it — find why; it may need
   to be combined with the `--use-angle=vulkan` loader path from above, or a
   different Ozone/EGL wiring.)

3. **A tiny Vulkan ICD/loader shim that advertises `VK_KHR_xcb_surface`.** If (1)
   is fiddly, a thin interposer (an `LD_PRELOAD`-style shim or a small ICD that
   forwards to SwiftShader but reports the surface extension present, stubbing
   `vkCreateXcbSurfaceKHR` since it is never called headless) satisfies ANGLE.
   Prefer (1)/(2) — this is more invasive.

4. **Confirm the NULL dispatch is truly the GL failure, not a second bug.** The
   caller of the NULL is in a `.so` (return addr in the `0x1000_0000_0000+`
   library-map region). If (1)/(2) make `eglInitialize` succeed but a NULL call
   persists, symbolize it: build an ld.so library-load map (log each `.so` path →
   base at load) and locate the caller. It should vanish once GL initializes.

Once GL initializes: verify `--dump-dom` prints, then switch the harness to
`--screenshot` and diff the PNG against the SAME chrome build on the host
(`tools/compare/` / `tools/gui_shot.py`) — near pixel-identical means it worked.

---

## Environment, build, verify (match the project exactly)

- **Build/run:** MSYS2/UCRT64 clang (`tobyos-build-env.md`). Reproduce the bug:
  `bash logs/chromium-m0.sh` (force-rebuilds the initrd, builds `-DCHROMIUM_BOOT`,
  boots QEMU `-m 4096`; ~4–8 min under TCG; chrome crashes ~7 s). `TRACE=1 bash
  logs/chromium-m0.sh` adds the syscall firehose. **A `struct proc`/`struct file`/
  widely-included-struct layout change requires a clean build** —
  `logs/chromium-m0-clean.sh` wraps `make clean`. Carry the TMP fix on
  `$(CC)`/`$(HOST_CC)` (the scripts already do it). Re-run 3× before believing a
  boot failure (TCG is slow/variable).
- **Payload:** opt-in, `.gitignore`d, already staged on this machine. If missing:
  `bash programs/chromium/build.sh` + `bash programs/chromium/sysroot.sh`.
- **The invocation** (flags + envp) is the `#ifdef CHROMIUM_BOOT` block in
  `src/kernel.c` — keep `argc`/`envc` in sync when you add args/env.
- **Symbolization** (proven): chrome's main PIE loads at `0x500000`;
  `objdump -d --start-address=<rip-0x500000> …` on
  `programs/chromium/chrome-headless-shell-linux64/chrome-headless-shell`
  symbolizes a user rip. Library (`.so`) addresses live at `0x1000_0000_0000+`
  and need a runtime load map to symbolize.
- **Greps:** serial logs are binary-ish — always `grep -a`. Key lines:
  `VerifyExtensions|eglInitialize|EXCEPTION 14|VK_KHR`.
- **Verify before committing.** Headless: `--dump-dom` prints `<h1>tobyOS</h1>`,
  then `--screenshot` PNG diffed vs host chrome. **Never** `taskkill msedge` (the
  user's real browser) — always `--user-data-dir`.
- **One coherent slice per branch off `main`; extend `docs/chromium-bringup-m1.md`
  + a `MEMORY.md` line each slice; commit trailer
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`; merge `--no-ff`.**

---

## Walls / non-goals (know them; don't rediscover)

- **GPU-accelerated GL is a LATE tier.** Do this on the **software** SwiftShader
  path. Real GL via i915-lite is much later, if at all.
- **A full X server is NOT the goal.** If you go the fake-X-server route, it only
  needs to satisfy `xcb_connect`'s handshake so the loader advertises the
  extension — chrome never opens a window. Keep it minimal.
- **Sandbox stays off** (`--no-sandbox --single-process --no-zygote`, already set).
- If `eglInitialize` succeeds and chrome renders, the next milestones are M2
  (a real JS/React SPA) and M3 (WINDOWED via the tobyOS compositor —
  `src/gfx.c`/`src/wm.c`/TobyTK, see `docs/chromium-bringup-render-prompt.md`).

---

## Scope honesty

This is one well-characterized bug: ANGLE requires an X11 Vulkan WSI extension
that a headless SwiftShader can't present. The kernel is done; SwiftShader works;
you are closing the gap between ANGLE's WSI *presence* check and a display-less
environment — most likely with a tiny AF_UNIX fake-X-server handshake or an
ANGLE surfaceless configuration. Measure the loader's WSI probe first, pick the
smallest shim, verify with a DOM dump then a host-diffed PNG. When it lands,
Chromium **renders real pixels** on tobyOS via pure software — the payoff of the
whole arc.
