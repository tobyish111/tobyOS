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

/* VirGL 3D support (Phase 3 foundation) */
bool virtio_gpu_virgl_available(void);
int  virtio_gpu_ctx_create(uint32_t ctx_id, const char *debug_name);
void virtio_gpu_ctx_destroy(uint32_t ctx_id);
int  virtio_gpu_submit_3d(uint32_t ctx_id, const void *cmd_buf, uint32_t cmd_size);

/* ---- Per-window GPU resource API --------------------------------
 *
 * Each GUI window can own a VirtIO-GPU 2D resource backed by physically
 * contiguous PMM pages.  The compositor uses these for:
 *
 *   - Per-window dirty tracking: only transfer changed window regions.
 *   - Direct-to-scanout: a fullscreen top window's resource can replace
 *     the compositor scanout, skipping the CPU blit entirely.
 *
 * Resource IDs are bump-allocated starting above the scanout + cursor
 * IDs so there is no collision.  All functions are no-ops when no
 * VirtIO-GPU is bound (safe on Limine-only machines).
 */

/* Allocate a 2D resource (BGRX) + PMM backing for a window.  Returns
 * the resource ID (>0) on success, 0 on failure.  *backing_out receives
 * the HHDM virtual pointer, *phys_out the physical address. */
uint32_t virtio_gpu_create_window_resource(uint32_t width, uint32_t height,
                                           void **backing_out,
                                           uint64_t *phys_out);

/* Destroy a window resource: detach backing, unref the resource, free
 * the PMM pages.  Safe to call with resource_id == 0 (no-op). */
void virtio_gpu_destroy_window_resource(uint32_t resource_id,
                                        uint64_t backing_phys,
                                        size_t   backing_bytes);

/* Transfer a window's backing to the host resource (TRANSFER_TO_HOST_2D
 * over the full surface).  Caller has already copied fresh pixels into
 * the backing. */
void virtio_gpu_transfer_window(uint32_t resource_id,
                                uint32_t width, uint32_t height);

/* Direct-scanout: point scanout 0 at `resource_id` instead of the
 * compositor's global resource.  Returns true on success. */
bool virtio_gpu_set_scanout_resource(uint32_t resource_id,
                                     uint32_t width, uint32_t height);

/* Restore scanout 0 to the compositor's global resource. */
void virtio_gpu_restore_scanout(void);

/* Flush a window resource to the display (RESOURCE_FLUSH). */
void virtio_gpu_flush_window(uint32_t resource_id,
                             uint32_t width, uint32_t height);

/* ---- Hardware cursor API ----------------------------------------
 *
 * The virtio-gpu cursor plane (VQ 1) provides tear-free, zero-copy
 * cursor movement on the host display. When available, the compositor
 * should prefer this over software-rendered cursors.
 */

/* Returns true if a hardware cursor is available and currently active. */
bool virtio_gpu_hw_cursor_available(void);

/* Upload a new cursor image (64x64 ARGB pixels, row-major) and set
 * the hotspot. Pass NULL for pixels_64x64 to re-upload the current
 * backing (e.g. after modifying it in place). */
void virtio_gpu_hw_cursor_update(const uint32_t *pixels_64x64,
                                 int hot_x, int hot_y);

/* Move the hardware cursor to screen coordinates (x, y). */
void virtio_gpu_hw_cursor_move(int x, int y);

/* Hide the hardware cursor (disable the cursor plane). */
void virtio_gpu_hw_cursor_hide(void);

#endif /* TOBYOS_VIRTIO_GPU_H */
