/* virtio_gpu.h -- virtio-gpu (modern, basic 2D mode) display driver.
 *
 * Bound through the milestone-21 PCI driver registry. Matches the Red Hat
 * virtio vendor (0x1AF4) at modern non-transitional device id 0x1050. Probe
 * walks the modern virtio cap chain (same scaffolding as src/virtio_net.c),
 * brings up scanout 0 with a single host-side resource backed by a
 * physically-contiguous PMM-allocated guest framebuffer, and stashes a
 * "ready" flag.
 *
 * The two-call public API is on purpose:
 *
 *   virtio_gpu_register()        -- registers the PCI driver. Called from
 *                                   kernel.c alongside e1000_register(),
 *                                   xhci_register(), etc., BEFORE
 *                                   pci_bind_drivers(). The probe runs as
 *                                   part of pci_bind_drivers; if no
 *                                   virtio-gpu is present this is silent.
 *
 *   virtio_gpu_install_backend() -- called from kernel.c AFTER
 *                                   gfx_layer_init(). If the probe found a
 *                                   virtio-gpu and brought it up cleanly,
 *                                   AND the gfx layer's resolution matches
 *                                   the GPU's preferred resolution, this
 *                                   installs a gfx_backend that overrides
 *                                   the default Limine-memcpy gfx_flip()
 *                                   path with TRANSFER_TO_HOST_2D +
 *                                   RESOURCE_FLUSH. If anything is wrong
 *                                   (no GPU, dim mismatch, gfx not ready)
 *                                   this is a silent no-op and the existing
 *                                   Limine framebuffer fallback handles
 *                                   everything as before.
 *
 * The two phases are split because the milestone-21 boot order runs
 * pci_bind_drivers() well before gfx_layer_init() (drivers may need /data
 * on AHCI/NVMe before the heap-backed gfx surface comes up). Splitting
 * "talk to the device" from "register the flush hook" lets each phase
 * happen at the correct moment without forcing the gfx layer to peek at
 * driver-internal state.
 */

#ifndef TOBYOS_VIRTIO_GPU_H
#define TOBYOS_VIRTIO_GPU_H

#include <tobyos/types.h>

/* Register the virtio-gpu PCI driver. No-op-on-absent: if no matching
 * device exists, the probe is never called and nothing logs. */
void virtio_gpu_register(void);

/* Hook the virtio-gpu flush path into gfx_flip(), iff the probe brought a
 * device up cleanly AND its resolution matches gfx's. Idempotent. Safe to
 * call when no GPU was found (silent no-op). Must be called AFTER
 * gfx_init() succeeds, otherwise there's no compositor to hook. */
void virtio_gpu_install_backend(void);

/* Diagnostics: did probe bring a device up? Used by the boot log to print
 * the regression-table-friendly "[virtio-gpu] backend active" line. */
bool virtio_gpu_present(void);

#endif /* TOBYOS_VIRTIO_GPU_H */
