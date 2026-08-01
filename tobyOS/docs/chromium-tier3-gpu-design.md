# Tier 3 — real GPU acceleration for Chromium on tobyOS (design)

**Status: DESIGN ONLY. Nothing here is implemented.** Written during slice 90
so the arc has a plan on paper; it deliberately does not start coding,
because tier 3 is gated on work that is not finished (see §6).

Companion docs: `chromium-tier25-ozone-handoff.md` (current front),
`chromium-hypothesis-ledger.md` (evidence log).

---

## 1. Correct the premise first

The tiers table says tier 3 is *"Replace SwiftShader software GL with actual
GPU acceleration."* **That description is out of date for the path we
actually run.** The headed Ozone command line (dumped from `[execve-argv]`)
is:

```
--disable-gpu --disable-vulkan --use-gl=disabled --in-process-gpu
```

So the headed browser is not using SwiftShader at all — it is doing **CPU
rasterization in Blink/cc**, with no GL context anywhere. SwiftShader is
what the *headless* flavour used back in the M1 era (slices 9–13).

That matters because it changes what tier 3 has to deliver. It is not
"swap one GL implementation for another". It is **"give chrome a working GL
or Vulkan stack for the first time on this platform"** — and the reason
those flags are there is that every previous attempt to give it one hit the
ANGLE/Vulkan WSI wall (slices 11–13).

## 2. What already exists in tree (and what it is worth)

| Piece | File | Use to tier 3 |
|---|---|---|
| virtio-gpu driver, 2D | `src/virtio_gpu.c` | Real: scanout, resource create/flush. |
| **VirGL feature probe** | `src/virtio_gpu.c:1416-1452` | Already negotiates `VIRTIO_GPU_F_VIRGL` and records `virgl_enabled`. Nothing consumes it yet. |
| Display/DRM abstraction | `src/gpu_drm.c` | tobyOS-internal only — **not** the Linux DRM ABI. |
| i915-lite | `src/gpu_intel_modeset.c` | Real-HW modeset/blit/cursor. Display, not render. |
| Fake X server + MIT-SHM | `src/xserver.c` | Tier 2.5's pixel path; tier 3 replaces *what fills* the buffer, not the buffer. |

**The gap that defines tier 3:** `grep -r DRM_IOCTL src/` returns **nothing**.
There is no `/dev/dri/card0`, no `/dev/dri/renderD128`, and no DRM ioctl
surface. Every userspace GL/Vulkan driver on Linux talks to the kernel
through exactly that. Tier 3 is, at its core, *building that node*.

## 3. Why we do not write a GL driver

The instinct is to implement GL ourselves. Do not. The whole Track B thesis
— proven across ~90 slices — is **close the ABI gap and run the real
userspace unmodified**. The same applies here:

- Chrome's GL comes from **ANGLE**, which is statically linked into the
  chrome binary (measured in slice 12) and speaks either GL or Vulkan
  downward.
- The Linux userspace that turns those into hardware commands is **Mesa**.
- Mesa's `virtio-gpu` back ends already exist and are shipped as ordinary
  `.so`s by every distro: **VirGL** (GL) and **Venus** (Vulkan).

So the realistic shape is: *stage prebuilt Mesa from the Debian sysroot we
already assemble for chrome, and implement the kernel ioctls it issues.*
That is the same trade that made chrome itself work — 60+ DSOs staged, ABI
closed underneath. Writing a GL stack from scratch is a multi-year detour
and was already rejected for the browser engine (`browser-chromium-v8-roadmap.md` §0).

## 4. Proposed path, cheapest decisive step first

### Phase 0 — attach the device (hours)

`run_watch.py` passes **no GPU device at all**; the guest runs on the
default VGA + Limine framebuffer. Add `-device virtio-gpu-gl-pci` (QEMU's
VirGL-capable model; `virtio-vga-gl` for the VGA-compat variant) and confirm
from the existing log line that the guest negotiates it:

```
[virtio-gpu] features: device=... virgl=yes
```

This is a **one-line experiment that either proves or kills the whole VirGL
route before any kernel work**, in the spirit of the control-rig runs that
have carried this arc. If the host QEMU build lacks VirGL (`virgl=no`), the
route is dead on this machine and we go to §5 instead. **Do this first.**

### Phase 1 — the DRM device node (the real work, multi-slice)

Implement `/dev/dri/card0` + `/dev/dri/renderD128` with enough of the DRM +
virtio-gpu ioctl ABI for Mesa's virgl driver to initialise:

- generic DRM: `DRM_IOCTL_VERSION`, `GET_CAP`, `GEM_CLOSE`, `PRIME_HANDLE_TO_FD`/`FD_TO_HANDLE`
- virtio-gpu: `VIRTGPU_GETPARAM`, `VIRTGPU_GET_CAPS`, `VIRTGPU_RESOURCE_CREATE`,
  `VIRTGPU_MAP`, `VIRTGPU_TRANSFER_TO_HOST`/`FROM_HOST`, `VIRTGPU_EXECBUFFER`,
  `VIRTGPU_WAIT`, `VIRTGPU_CONTEXT_INIT`

Most of these map onto commands `virtio_gpu.c` already builds. The genuinely
new kernel concepts are **GEM handles** (a per-fd handle→resource table) and
**PRIME/dma-buf** (an fd that can be passed over AF_UNIX — we already have
SCM_RIGHTS, slice 77).

Expect this to be where the ABI-gap instrument (`[linux] UNHANDLED ioctl`)
earns its keep, exactly like the syscall gap list did in M0/M1. **Add an
unhandled-ioctl logger before writing any handler** — measure, don't guess.

### Phase 2 — stage Mesa

Extend `programs/chromium/sysroot.sh` to pull `libgbm`, `libdrm`,
`libEGL`/`libGLESv2` (Mesa), and `virtio_gpu_dri.so` / `libvulkan_virtio.so`.
Same Debian-bookworm closure mechanism already used for chrome's 60+ DSOs.

### Phase 3 — point chrome at it

Drop `--disable-gpu --use-gl=disabled`; try in order:
1. `--use-gl=egl --use-angle=gl` (ANGLE→GL→Mesa virgl)
2. `--use-angle=vulkan` + Venus (needs `VK_KHR_xcb_surface` from our fake X
   server — **the slice 11–13 wall; re-read those before attempting it**)

Phase 3 is where the old GL arc's scar tissue lives. Budget for it.

## 5. The honest alternative: don't chase GPU at all

**Tier 1 measured our display path at ~1 ms/frame — essentially free — and
concluded frame PRODUCTION inside chrome is the bottleneck.** Tier 2 then
bought 630→1050 frames purely by removing kernel serialization. Nothing in
that evidence says the *rasterizer* is the limit.

Before spending multiple slices on Phase 1, **measure what CPU rasterization
actually costs** once chrome survives and paints via SHM: sample chrome's
own `cc`/raster threads with the existing ring-3 profiler. If raster is not
dominant, tier 3 is a large investment aimed at a small win, and the honest
move is to re-rank it below tier 4 (audio) or below further tier-2-style
kernel work. **Do not start Phase 1 without that measurement.** This arc has
twice been rescued by measuring instead of assuming; the same discipline
applies to deciding whether a tier is worth doing at all.

## 6. Gate — why implementation has NOT started

Tier 3 changes what fills the frame buffer. Tier 2.5 is what makes a frame
buffer exist at all, and it is **not finished**: as of slice 90 chrome
reaches `MapWindow` but a thread still NULL-derefs (`addr=0x18`,
`rip=0x59be96f`) before any `ShmPutImage`, so **zero SHM frames have ever
flowed**. Building a GPU path for a pipeline that does not yet deliver a
single frame would be building on nothing, and there would be no baseline to
measure the result against.

**Order: fix the NULL deref → SHM frames flow → close tier 2.5 and re-measure
→ take the §5 measurement → only then decide whether Phase 1 is worth it.**
