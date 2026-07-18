# Task: finish bringing up Chromium on tobyOS — get it to actually RENDER (headless, then windowed), reusing what tobyOS already has

You are continuing a live, successful bring-up. A previous agent got a **real,
unmodified headless Chromium** (`chrome-headless-shell`, V8 + Blink) to run on
tobyOS's Linux ABI (Track B) all the way from *"can't load `libpthread`"* to
**running its full multi-threaded engine** — glibc dynamic linking, V8's memory
cage, Mojo IPC, a 17-thread pool, working futex timeouts, fontconfig — and it now
**reaches the actual render pipeline and stalls there**. Your job is to close the
remaining gap so Chromium **produces output** (a DOM dump / a rendered PNG), then
put it **on screen in a native tobyOS window** — and to do M4/M5 (the Windows
build) after.

**This is a porting/bring-up job, not engine work.** Chromium brings its own
engine; you are closing the gap between what Chromium's renderer demands and what
tobyOS provides — and, crucially, **wiring Chromium's output into tobyOS's
EXISTING graphics stack** rather than building a new one.

---

## THE PRIME DIRECTIVE: reuse tobyOS's existing code — do not reimplement

tobyOS already has a mature graphics/GUI/compositor stack. **Use it.** Before
writing any new compositor, window system, rasterizer, GL, or display code, find
the existing implementation and reuse it. The previous arc's biggest wins came
from finding that a wall was already solved (glibc already worked; SwiftShader is
already bundled; the demand-paging fault handler already existed). Keep that
instinct. Concretely, reuse:

- **The software compositor + framebuffer primitives** — `include/tobyos/gfx.h`,
  `src/gfx.c`: `gfx_init`, `gfx_clear`, `gfx_fill_rect`, **`gfx_blit`** (raw RGB
  blit — this is how you copy Chromium's framebuffer to the display),
  `gfx_blit_blend` (alpha), **`gfx_flip`** (present the back buffer). This is
  already the double-buffered software compositor every native app draws through.
- **The window manager / compositor** — `src/wm.c`, `src/compositor_accel.c`.
  Windows, stacking, dirty-rect present. A Chromium window is just another
  compositor window.
- **TobyTK, the GUI toolkit** — `include/tobyos/gui.h`, `sdk/`. Every native
  tobyOS app is a TobyTK client (see the `gui_*` programs). A Chromium host window
  should be a TobyTK window that blits Chromium's pixels and forwards input.
- **A working model of "an app that owns a big pixel buffer in a TobyTK window"**
  — `programs/user_gui_browser/main.c` (the from-scratch browser's GUI shell) and
  the other `gui_*` programs. Model the Chromium **Ozone/host window** on how
  these create a window, get a framebuffer, blit into it, and pump events. (Do
  NOT reuse the from-scratch browser's *engine* — Chromium has its own — but DO
  reuse its window/present/input integration pattern.)
- **The virtio-gpu present path** — `src/virtio_gpu.c` (proven headless in the
  `virtgpu` milestone) and the **i915-lite** real-HW GPU path
  (`src/gpu_intel_modeset.c`, memory `igpu-i915lite.md`). These are the display
  backends `gfx_flip` already targets. GPU-*accelerated* GL for Chromium is a
  late tier — start with the software path these already provide.
- **Chromium's OWN bundled software renderer** — `libvk_swiftshader.so`,
  `libvulkan.so.1`, `libEGL.so`, `libGLESv2.so` ship inside
  `programs/chromium/chrome-headless-shell-linux64/`. SwiftShader is a software
  Vulkan/GL implementation. Prefer making Chromium's *own* software raster/GL
  path work over writing any GL yourself.
- **The screenshot / compare harness** — `tools/gui_shot.py` (QMP screenshots)
  and `tools/compare/` (the Edge-oracle diff harness). For Chromium the oracle is
  the SAME chrome build on the host, so outputs should be **near pixel-identical**
  — a big diff means an ABI/render bug, not a rendering gap.

If something is genuinely missing, add the *smallest* shim that lets Chromium's
existing code path work — the same way the previous arc added `socketpair`, fd
sharing, and futex timeouts rather than reimplementing Mojo, the fd table, or
glibc's condvars.

---

## Read first (this is a live effort with detailed state)

- **`docs/chromium-bringup-m1.md`** — the burn-down so far (slices 1–8), with the
  current wall (slice 8) described in detail. Read it fully.
- **`docs/chromium-bringup-m0.md`** — the M0 gap-list methodology + how the
  payload is staged (RAM initrd) + how to run it.
- **`docs/chromium-bringup-agent-prompt.md`** and
  **`docs/browser-chromium-v8-roadmap.md`** — the original strategic framing
  (Linux first, the walls, headless→windowed→Windows sequencing).
- **Memory** (`C:\Users\tdude\.claude\projects\c--CustomOS\memory\`):
  `chromium-bringup.md` (the whole arc + every fix + the current wall — READ THIS
  FIRST), `gui-framework-toolkit.md` (TobyTK + the compositor),
  `igpu-i915lite.md` (real-HW GPU), `virtgpu`/`browser-*` docs (the software
  compositor + paint), `linux-abi-compat-b1.md` (Track B),
  `win32-pe-compat-c1.md` (Track C for M4/M5), `tobyos-build-env.md`,
  `storage-provisioning.md`.
- Update `chromium-bringup.md` + `MEMORY.md` as you go; keep the slice-by-slice
  burn-down going (it's the primary progress metric).

---

## The current wall, precisely (slice 8 — start here)

Chromium runs its full concurrency stack, then: it waits ~85 s (a navigation /
render timeout), **dumps no DOM**, and the **main thread** hits
`base::ImmediateCrash()` (`int3; ud2`) — a message-less `NOTREACHED`/watchdog.
Its own stderr shows the tell:

```
ERROR: eglInitialize SwANGLE failed with error EGL_NOT_INITIALIZED
ERROR: Initialization of all (1) EGL display types failed.
ERROR: GLDisplayEGL::Initialize failed.
```

So the **renderer can't finish a frame because GL (SwANGLE) won't initialize**,
the compositor/paint path stalls, navigation never completes, and a watchdog
fires. Even `--dump-dom` (which needs no pixels) is gated behind the renderer
completing navigation.

**Your first job is to get a headless render to COMPLETE.** Instrument-first —
measure, don't guess (three handed-down hypotheses were wrong last arc, each
caught by instrumenting; the int3 was symbolized, not guessed). Concretely:

1. **Reproduce + trace.** `bash logs/chromium-m0.sh` builds + boots it; the gap
   list prints at the end. The permanent instruments are already in-tree: the
   `[linux] UNHANDLED syscall` logger, the **recent-syscall ring** (dumped by
   `src/isr.c` on any fatal user fault — cheap crash context), and
   **`-DLINUX_SYSCALL_TRACE`** (the firehose; `TRACE=1 bash logs/chromium-m0.sh`).
   **Symbolization recipe** (already proven): Chromium's main PIE loads at
   `0x500000` (confirmed: ELF `e_entry` `0x4899c90` + base), so
   `objdump -d --start-address=<rip-0x500000> …` on
   `programs/chromium/chrome-headless-shell-linux64/chrome-headless-shell`
   symbolizes any user rip. Use it to stack-walk the main thread at the crash and
   to see what the renderer thread is blocked on.
2. **Try the flags first (cheapest).** The invocation lives in the
   `#ifdef CHROMIUM_BOOT` harness in `src/kernel.c`. Chromium has software paths
   that avoid GL: `--disable-gpu-compositing`, `--in-process-gpu`,
   `--use-gl=angle --use-angle=swiftshader`, `--use-gl=swiftshader`,
   `--disable-features=Vulkan`, `--disable-software-rasterizer` (or the reverse),
   `--enable-features=...`. For a pure DOM dump, `--run-all-compositor-stages-
   before-draw` / `--virtual-time-budget=…` can force completion. Find the flag
   set that makes navigation COMPLETE and the DOM print. Measure which one moves
   the needle.
3. **If SwANGLE must initialize:** find what its `eglInitialize` needs that
   tobyOS doesn't provide (trace the syscalls/opens right before the EGL failure —
   likely `/dev/dri*`, a `gbm`/`drm` ioctl, `memfd`, or a `/proc`/`/sys` read).
   Add the smallest shim. SwiftShader is pure-software, so it should need no real
   GPU — only whatever platform surface its EGL/Vulkan init probes.
4. **Verify M1:** `chrome-headless-shell --dump-dom data:text/html,<h1>tobyOS</h1>`
   prints the DOM, and `--screenshot` writes a PNG. Diff the PNG against the SAME
   chrome build on the host — near pixel-identical.

---

## Then: the milestones you own (in order)

- **M1 (finish) — headless renders.** A static page's DOM dumps and a PNG matches
  host chrome. (You are ~here; the wall above is the last step.)
- **M2 — headless runs a real JS app.** Point it at a React/Angular SPA; confirm
  it hydrates and renders. This proves V8's JIT (W^X), threads, and the platform
  surface end-to-end — the thing the from-scratch engine could never do.
- **M3 — WINDOWED, via the tobyOS compositor (this is where "use the tobyOS
  compositor/GUI" is the whole point).** Give Chromium a display by writing a
  **TobyTK/Ozone host**: a native tobyOS window (TobyTK, `gui.h`) that maps
  Chromium's shared-memory framebuffer and **`gfx_blit`s it into the window each
  frame, then `gfx_flip`s** — reusing `src/gfx.c` + `src/wm.c` + the
  virtio-gpu/i915-lite present path. Forward TobyTK input events to Chromium.
  Model the window/present/input plumbing on `programs/user_gui_browser/`. Do NOT
  build a new compositor or window system — Chromium is just another compositor
  client. On-screen QEMU screenshot (`tools/gui_shot.py`) verifies.
- **M4 — Windows headless (Track C).** Same instrument-first loop with a Windows
  x64 headless Chromium; the gap list is unresolved Win32 imports
  (DirectWrite/D3D/process/sandbox). See `win32-pe-compat-c1.md`.
- **M5 — Windows windowed (Track C).** Chromium painting via its Windows backend
  (GDI/D3D → WARP/SwiftShader first), hosted in a tobyOS window the same way.

---

## The walls / non-goals (know them; don't rediscover)

- **GPU-accelerated GL is a LATE tier.** Do M1–M3 on the **software** path
  (SwiftShader / software compositing) that tobyOS + Chromium already provide.
  Real GL via i915-lite (`igpu-i915lite.md`) comes much later, if at all.
- **Sandbox stays off** (`--no-sandbox --single-process --no-zygote`, already
  set). The real seccomp/userns sandbox is a large optional sub-project; document
  that `--no-sandbox` runs untrusted JS at full privilege.
- **Widevine/DRM is a non-goal** (closed source). Clear Key only.
- **The non-fatal platform bits Chromium already degrades gracefully** (D-Bus
  `connect`-by-path AF_UNIX, `AF_NETLINK`, the inotify sysctl) can stay stubbed
  unless a target site needs them — measure before building.

---

## Environment, instruments, and conventions (match the project exactly)

- **Build/run:** MSYS2/UCRT64 clang (`tobyos-build-env.md`). From git-bash carry
  the TMP fix on `$(CC)`/`$(HOST_CC)` (see `logs/chromium-m0.sh`, which already
  does it). **A `struct proc`/widely-included-struct layout change requires
  `make clean`** (bit the previous arc twice) — `logs/chromium-m0-clean.sh` wraps
  it. QEMU boots with `-m 4096`; the 262 MB payload rides a RAM initrd (no tobyfs
  populator needed — see `chromium-bringup.md` Tier 0).
- **Payload:** opt-in, `.gitignore`d. `bash programs/chromium/build.sh` (the
  binary) + `bash programs/chromium/sysroot.sh` (the ~120-`.so` sysroot). Both
  are already wired into the Makefile's `/opt/chrome` staging.
- **Instruments (permanent, in-tree):** the `[linux] UNHANDLED` logger + syscall
  names + first-hit dedup, the recent-syscall ring, `-DLINUX_SYSCALL_TRACE`, and
  the `#ifdef CHROMIUM_BOOT` harness. Keep them; extend as needed. Serial logs are
  binary-ish — always `grep -a`. The QEMU boot is slow (TCG ~3×); re-run 3× before
  believing a boot failure.
- **Verify before committing.** Headless: the DOM dump / the PNG diffed vs host
  chrome. Windowed: an on-screen `tools/gui_shot.py` screenshot. **Never**
  `taskkill msedge`/the user's real browser — always `--user-data-dir`.
- **One coherent slice per branch off `main`; extend `docs/chromium-bringup-*.md`
  + a `MEMORY.md` line each slice; commit with trailer
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`; merge `--no-ff`;
  push.** Track the burn-down.

---

## Scope honesty

Getting Chromium to complete a headless render, then compositing it into a native
window, then doing the Windows build, is real bring-up — expect weeks of
render-pipeline and Win32-import whack-a-mole. But it is **bounded and it stays
done**, and the previous eight slices proved the method: measure the exact wall,
find the existing tobyOS/Chromium code that already does the work, add the
smallest shim to connect them, verify, merge. The payoff is the *actual*
Chromium — real apps, real V8, real pixels — running in a tobyOS window, in both
its Linux and Windows personalities. Start with the headless render (slice 8) and
report the first result before going wide.
