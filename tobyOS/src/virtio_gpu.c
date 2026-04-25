/* virtio_gpu.c -- virtio-gpu (modern, basic 2D mode) display driver.
 *
 * Drives QEMU's `-device virtio-gpu-pci` on i440fx/q35 and the same
 * silicon on Linux/cloud VMs. Bound through the milestone-21 PCI
 * driver registry; matches Red Hat virtio vendor 0x1AF4 at modern
 * non-transitional device id 0x1050.
 *
 * Spec reference: https://docs.oasis-open.org/virtio/virtio/v1.2/csd01/
 *                 virtio-v1.2-csd01.html  (especially section 5.7,
 *                 "GPU Device").
 *
 * Architecture
 * ------------
 *
 *   Modern virtio-pci transport (cap walk, common/notify/device cfg)
 *   is identical to src/virtio_net.c -- see that file's top-of-file
 *   comment for the full walk-through. We reuse the same scaffolding
 *   shape so a future "common modern virtio-pci helper" can lift the
 *   shared bits out without disturbing either driver.
 *
 *   We bring up exactly ONE virtqueue (queue 0, the controlq). The
 *   cursor queue (vq 1) is ignored -- we draw the cursor in software
 *   via gfx_draw_cursor(). All commands are submitted synchronously
 *   over the controlq:
 *
 *     issue_cmd(req_ptr, req_len, resp_ptr, resp_len)
 *       -- two descriptors (request/device-readable + response/device-
 *       writable), ring the doorbell, poll the used ring until our
 *       chain comes back. No IRQ wiring (matches every other
 *       milestone-21 driver).
 *
 *   Display setup runs four commands at probe time:
 *
 *     1. GET_DISPLAY_INFO       -- learn scanout 0's preferred geometry.
 *     2. RESOURCE_CREATE_2D     -- one host-side BGRX resource at that
 *                                  geometry.
 *     3. RESOURCE_ATTACH_BACKING-- point the resource at a
 *                                  pmm_alloc_pages-allocated, physically
 *                                  contiguous guest framebuffer (so we
 *                                  can describe it in a single
 *                                  mem_entry).
 *     4. SET_SCANOUT            -- bind the resource to scanout 0.
 *
 *   After that the GPU is "ready to flush". We hand a struct
 *   gfx_backend to gfx.c -- but only after gfx_layer_init() has run,
 *   so the install is a separate public call:
 *
 *     virtio_gpu_register()         -> installs the PCI driver
 *     pci_bind_drivers()            -> probe runs (display setup)
 *     gfx_layer_init()              -> Limine FB -> gfx
 *     virtio_gpu_install_backend()  -> if dims match, hook gfx_flip()
 *
 *   The backend's flip(): memcpy g.back -> our PMM backing,
 *   TRANSFER_TO_HOST_2D over the full surface, RESOURCE_FLUSH. The
 *   compositor (src/gui.c) keeps calling gfx_flip() exactly as before
 *   -- it has no idea which path is active.
 *
 * DMA correctness
 * ---------------
 *
 *   Every buffer we point the device at -- the queue ring page, the
 *   per-command request/response page, and the scanout backing -- is
 *   sourced from pmm_alloc_page / pmm_alloc_pages. Those return
 *   physical addresses we can pass straight into descriptors; the
 *   matching kernel-virtual access goes through pmm_phys_to_virt
 *   (HHDM), which is identity-mapped to phys + an offset. No
 *   vmm_translate() is needed in this file because we never put a
 *   heap-allocated pointer into a descriptor (heap memory is
 *   discontiguous-phys-mapped-contiguous-virt; pmm_virt_to_phys would
 *   give the wrong answer there -- that's the lesson from xhci.c).
 *
 *   The scanout backing is allocated via pmm_alloc_pages(N) so it's
 *   PHYSICALLY contiguous, which lets RESOURCE_ATTACH_BACKING describe
 *   it in a single mem_entry instead of building a scatter list.
 *
 * No regressions
 * --------------
 *
 *   If the device is absent the probe never runs and nothing logs.
 *   If the probe runs but any step fails we kprintf and return; no
 *   backend is installed and gfx_flip() stays on the Limine memcpy
 *   path. This keeps every existing run/run-uefi/run-ahci/run-nvme/
 *   run-virtio-net/run-e1000e/run-rtl8139/run-xhci flow byte-identical
 *   to before.
 */

#include <tobyos/virtio_gpu.h>
#include <tobyos/gfx.h>
#include <tobyos/pci.h>
#include <tobyos/pmm.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/cpu.h>
#include <tobyos/irq.h>
#include <tobyos/apic.h>

/* ---- PCI vendor / device --------------------------------------- */

#define VIRTIO_VENDOR             0x1AF4
#define VIRTIO_GPU_DEV_MODERN     0x1050   /* modern non-transitional */

/* ---- virtio cap cfg_type values -------------------------------- */

#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4
#define VIRTIO_PCI_CAP_PCI_CFG     5

/* ---- common config (BAR-mapped MMIO) layout -------------------- */

#define VIRTIO_PCI_DEVICE_FEATURE_SELECT  0x00
#define VIRTIO_PCI_DEVICE_FEATURE         0x04
#define VIRTIO_PCI_DRIVER_FEATURE_SELECT  0x08
#define VIRTIO_PCI_DRIVER_FEATURE         0x0C
#define VIRTIO_PCI_MSIX_CONFIG            0x10
#define VIRTIO_PCI_NUM_QUEUES             0x12
#define VIRTIO_PCI_DEVICE_STATUS          0x14
#define VIRTIO_PCI_CONFIG_GENERATION      0x15
#define VIRTIO_PCI_QUEUE_SELECT           0x16
#define VIRTIO_PCI_QUEUE_SIZE             0x18
#define VIRTIO_PCI_QUEUE_MSIX_VECTOR      0x1A
#define VIRTIO_PCI_QUEUE_ENABLE           0x1C
#define VIRTIO_PCI_QUEUE_NOTIFY_OFF       0x1E
#define VIRTIO_PCI_QUEUE_DESC             0x20   /* 64-bit */
#define VIRTIO_PCI_QUEUE_DRIVER           0x28   /* 64-bit */
#define VIRTIO_PCI_QUEUE_DEVICE           0x30   /* 64-bit */

/* ---- device_status bits ---------------------------------------- */

#define VIRTIO_STATUS_ACKNOWLEDGE          1
#define VIRTIO_STATUS_DRIVER               2
#define VIRTIO_STATUS_DRIVER_OK            4
#define VIRTIO_STATUS_FEATURES_OK          8
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET  64
#define VIRTIO_STATUS_FAILED             128

/* ---- feature bits we care about -------------------------------- */

#define VIRTIO_F_VERSION_1        32    /* mandatory for modern transport */

/* virtio MSI-X "no vector" sentinel; same constant as virtio_net.c. */
#define VIRTIO_MSI_NO_VECTOR      0xFFFFu

/* ---- split virtqueue layout (matches src/virtio_net.c) --------- */

#define VQ_DESC_F_NEXT     1
#define VQ_DESC_F_WRITE    2
#define VQ_DESC_F_INDIRECT 4

struct __attribute__((packed)) virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};
_Static_assert(sizeof(struct virtq_desc) == 16, "virtq_desc must be 16 bytes");

struct __attribute__((packed)) virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

#define VG_QSIZE          8u                /* one outstanding cmd at a time */
#define VQ_DESC_OFF       0u
#define VQ_AVAIL_OFF      512u              /* leaves slack vs desc tbl size */
#define VQ_USED_OFF       1024u

/* ---- virtio-gpu protocol (spec §5.7) --------------------------- */

#define VIRTIO_GPU_MAX_SCANOUTS  16

/* command type codes (only the ones we actually issue) */
enum {
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO        = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      = 0x0101,
    VIRTIO_GPU_CMD_RESOURCE_UNREF          = 0x0102,
    VIRTIO_GPU_CMD_SET_SCANOUT             = 0x0103,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH          = 0x0104,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     = 0x0105,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING = 0x0107,

    VIRTIO_GPU_RESP_OK_NODATA              = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO        = 0x1101,
};

/* virtio_gpu_formats -- pixel formats. We use B8G8R8X8_UNORM which on
 * little-endian x86 is the same byte order Limine hands us (BGRX), so
 * the back buffer can be memcpy'd verbatim with no swizzle. */
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM   2

struct __attribute__((packed)) virtio_gpu_ctrl_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t  ring_idx;
    uint8_t  padding[3];
};
_Static_assert(sizeof(struct virtio_gpu_ctrl_hdr) == 24,
               "virtio_gpu_ctrl_hdr must be 24 bytes");

struct __attribute__((packed)) virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

struct __attribute__((packed)) virtio_gpu_display_one {
    struct virtio_gpu_rect r;
    uint32_t enabled;
    uint32_t flags;
};

struct __attribute__((packed)) virtio_gpu_resp_display_info {
    struct virtio_gpu_ctrl_hdr      hdr;
    struct virtio_gpu_display_one   pmodes[VIRTIO_GPU_MAX_SCANOUTS];
};

struct __attribute__((packed)) virtio_gpu_resource_create_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
};

struct __attribute__((packed)) virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
};

struct __attribute__((packed)) virtio_gpu_resource_attach_backing {
    struct virtio_gpu_ctrl_hdr     hdr;
    uint32_t                       resource_id;
    uint32_t                       nr_entries;
    struct virtio_gpu_mem_entry    entries[1];   /* we always send 1 */
};

struct __attribute__((packed)) virtio_gpu_set_scanout {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect     r;
    uint32_t                   scanout_id;
    uint32_t                   resource_id;
};

struct __attribute__((packed)) virtio_gpu_transfer_to_host_2d {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect     r;
    uint64_t                   offset;
    uint32_t                   resource_id;
    uint32_t                   padding;
};

struct __attribute__((packed)) virtio_gpu_resource_flush {
    struct virtio_gpu_ctrl_hdr hdr;
    struct virtio_gpu_rect     r;
    uint32_t                   resource_id;
    uint32_t                   padding;
};

/* ---- driver state --------------------------------------------- */

#define VG_RESOURCE_ID  1   /* arbitrary; we only ever create one */

struct vgpu_dev {
    /* Pointers into BAR-mapped MMIO. */
    volatile uint8_t  *common;
    volatile uint8_t  *device_cfg;     /* unused but worth keeping for diag */
    volatile uint8_t  *notify_base;
    uint32_t           notify_mult;

    /* Single virtqueue (controlq -- vq 0). */
    uint16_t           qsize;
    uint16_t           avail_idx;
    uint16_t           used_idx;
    uint64_t           ring_phys;
    uint8_t           *ring;            /* HHDM virt of the queue page */

    struct virtq_desc *desc;
    volatile uint16_t *avail_idx_ptr;
    volatile uint16_t *avail_ring;
    volatile uint16_t *used_idx_ptr;
    struct virtq_used_elem *used_ring;
    volatile uint16_t *notify;          /* per-queue doorbell */

    /* Scratch page -- request struct at offset 0, response at REQ_AREA.
     * 4 KiB is plenty: largest response (display_info) is 408 bytes,
     * largest request (set_scanout) is 56 bytes. */
    uint64_t           scratch_phys;
    uint8_t           *scratch_virt;

    /* Scanout backing (the actual pixel buffer the GPU reads). */
    uint32_t           width;
    uint32_t           height;
    size_t             backing_bytes;
    size_t             backing_pages;
    uint64_t           backing_phys;
    uint8_t           *backing_virt;

    /* MSI-X bring-up state. Even with all commands synchronous, the
     * device still raises a used-ring interrupt per completion if we
     * bind a vector; we use that as a "completion observable" diag
     * counter and to keep the busy-poll bounded by the IRQ rate. */
    uint8_t            irq_vector;
    bool               irq_enabled;
    volatile uint64_t  irq_count;

    /* PCI bus/slot/fn cached at probe time so describe() can report
     * the device address without keeping a back-pointer to pci_dev. */
    uint8_t            pci_bus;
    uint8_t            pci_slot;
    uint8_t            pci_fn;

    /* M27F: per-region transfer counters. Surfaced via describe() so
     * displayinfo --json can prove the dirty-rect path is exercising
     * the partial TRANSFER_TO_HOST_2D + RESOURCE_FLUSH commands and
     * not just falling back to full flips. Both update under the
     * single-threaded gfx_flip caller, so plain uint64_t is safe. */
    uint64_t           full_flips;
    uint64_t           partial_flips;
    uint64_t           partial_pixels;

    /* M27G: snapshot of every scanout the host advertised, including
     * disabled ones. Only scanout 0 is actually driven (we register
     * "virtio-gpu0" with the display layer); the rest are exposed
     * through describe()/displayinfo solely so userland can see that
     * the GPU has more outputs available. enabled_scanouts is the
     * count of pmodes[i].enabled == 1 entries in the host response. */
    uint32_t           total_scanouts;          /* always VIRTIO_GPU_MAX_SCANOUTS */
    uint32_t           enabled_scanouts;        /* count of pmodes[i].enabled    */
    struct virtio_gpu_display_one
                       scanouts[VIRTIO_GPU_MAX_SCANOUTS];
};

/* Single-display scope: first probe wins. Mirrors what virtio_net.c
 * does for multi-NIC. */
static struct vgpu_dev g_vgpu;
static bool            g_vgpu_bound;     /* PCI driver bound, device set up */
static bool            g_vgpu_active;    /* gfx backend installed */

#define SCRATCH_REQ_OFF    0u
#define SCRATCH_RESP_OFF   2048u    /* keeps req+resp in disjoint cache lines */

/* ---- MMIO helpers (verbatim from virtio_net.c) ---------------- */

static inline uint8_t  cfg_r8 (struct vgpu_dev *d, uint32_t off) {
    return *(volatile uint8_t  *)(d->common + off);
}
static inline uint16_t cfg_r16(struct vgpu_dev *d, uint32_t off) {
    return *(volatile uint16_t *)(d->common + off);
}
static inline uint32_t cfg_r32(struct vgpu_dev *d, uint32_t off) {
    return *(volatile uint32_t *)(d->common + off);
}
static inline void cfg_w8 (struct vgpu_dev *d, uint32_t off, uint8_t  v) {
    *(volatile uint8_t  *)(d->common + off) = v;
}
static inline void cfg_w16(struct vgpu_dev *d, uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(d->common + off) = v;
}
static inline void cfg_w32(struct vgpu_dev *d, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(d->common + off) = v;
}
static inline void cfg_w64(struct vgpu_dev *d, uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(d->common + off)     = (uint32_t)(v & 0xFFFFFFFFu);
    *(volatile uint32_t *)(d->common + off + 4) = (uint32_t)(v >> 32);
}

/* ---- queue setup ---------------------------------------------- */

static void queue_layout(struct vgpu_dev *d) {
    d->desc = (struct virtq_desc *)(d->ring + VQ_DESC_OFF);

    uint8_t *avail = d->ring + VQ_AVAIL_OFF;
    /* avail layout: u16 flags; u16 idx; u16 ring[QSIZE]; u16 used_event */
    d->avail_idx_ptr = (volatile uint16_t *)(avail + 2);
    d->avail_ring    = (volatile uint16_t *)(avail + 4);

    uint8_t *used = d->ring + VQ_USED_OFF;
    /* used layout: u16 flags; u16 idx; used_elem ring[QSIZE]; u16 avail_event */
    d->used_idx_ptr = (volatile uint16_t *)(used + 2);
    d->used_ring    = (struct virtq_used_elem *)(used + 4);
}

/* MSI handler: just bump the diagnostic counter. The control flow
 * stays in `issue_cmd`'s busy-poll because every command we send is
 * synchronous; the IRQ is observable but doesn't drive state. */
static void vgpu_irq_handler(void *ctx) {
    struct vgpu_dev *d = (struct vgpu_dev *)ctx;
    if (d) d->irq_count++;
}

static bool vgpu_setup_controlq(struct vgpu_dev *d, uint16_t msix_vec) {
    d->qsize     = VG_QSIZE;
    d->avail_idx = 0;
    d->used_idx  = 0;

    cfg_w16(d, VIRTIO_PCI_QUEUE_SELECT, 0);
    uint16_t max_qs = cfg_r16(d, VIRTIO_PCI_QUEUE_SIZE);
    if (max_qs == 0) {
        kprintf("[virtio-gpu] controlq missing (queue_size_max=0)\n");
        return false;
    }
    /* QSIZE must be a power of 2 and <= queue_size_max. We pick a small
     * fixed value -- 8 is enough for our serial four-step setup. */
    if (max_qs < VG_QSIZE) {
        kprintf("[virtio-gpu] controlq max_size=%u < %u (unsupported)\n",
                max_qs, VG_QSIZE);
        return false;
    }
    cfg_w16(d, VIRTIO_PCI_QUEUE_SIZE, VG_QSIZE);

    d->ring_phys = pmm_alloc_page();
    if (!d->ring_phys) {
        kprintf("[virtio-gpu] OOM allocating controlq ring page\n");
        return false;
    }
    d->ring = (uint8_t *)pmm_phys_to_virt(d->ring_phys);
    memset(d->ring, 0, PAGE_SIZE);
    queue_layout(d);

    cfg_w64(d, VIRTIO_PCI_QUEUE_DESC,   d->ring_phys + VQ_DESC_OFF);
    cfg_w64(d, VIRTIO_PCI_QUEUE_DRIVER, d->ring_phys + VQ_AVAIL_OFF);
    cfg_w64(d, VIRTIO_PCI_QUEUE_DEVICE, d->ring_phys + VQ_USED_OFF);

    /* Latch the doorbell address while QUEUE_SELECT still points at us. */
    uint16_t qoff = cfg_r16(d, VIRTIO_PCI_QUEUE_NOTIFY_OFF);
    d->notify = (volatile uint16_t *)
                (d->notify_base + (uint32_t)qoff * d->notify_mult);

    /* Bind the controlq to msix_vec (or NO_VECTOR to leave it polled).
     * Verify the device accepted the bind; some legacy QEMU revs
     * silently drop QUEUE_MSIX_VECTOR. */
    cfg_w16(d, VIRTIO_PCI_QUEUE_MSIX_VECTOR, msix_vec);
    if (msix_vec != VIRTIO_MSI_NO_VECTOR) {
        uint16_t got = cfg_r16(d, VIRTIO_PCI_QUEUE_MSIX_VECTOR);
        if (got != msix_vec) {
            kprintf("[virtio-gpu] controlq MSI-X bind rejected "
                    "(asked %u, got 0x%04x) -- IRQ disabled for this queue\n",
                    msix_vec, got);
        }
    }

    cfg_w16(d, VIRTIO_PCI_QUEUE_ENABLE, 1);
    return true;
}

/* ---- synchronous request/response ----------------------------- */

/* Submit one request (device-readable) + matching response buffer
 * (device-writable) on the controlq, ring the doorbell, poll the used
 * ring until the device returns the chain. Returns true iff the device
 * processed the chain (it doesn't validate the response opcode -- the
 * caller does that). req_buf and resp_buf MUST point into the scratch
 * page so that the descriptors carry true physical addresses. */
static bool issue_cmd(struct vgpu_dev *d,
                      const void *req_buf, size_t req_len,
                      void       *resp_buf, size_t resp_len) {
    if (!req_buf || !resp_buf || req_len == 0 || resp_len == 0) return false;

    /* Compute physical addresses by trusting that req_buf/resp_buf live
     * in our scratch page (PMM-allocated, so virt = phys + HHDM). */
    uint8_t *r_req  = (uint8_t *)req_buf;
    uint8_t *r_resp = (uint8_t *)resp_buf;
    if (r_req  < d->scratch_virt || r_req  >= d->scratch_virt + PAGE_SIZE ||
        r_resp < d->scratch_virt || r_resp >= d->scratch_virt + PAGE_SIZE) {
        kprintf("[virtio-gpu] issue_cmd: buffers outside scratch page\n");
        return false;
    }
    uint64_t req_phys  = d->scratch_phys + (uint64_t)(r_req  - d->scratch_virt);
    uint64_t resp_phys = d->scratch_phys + (uint64_t)(r_resp - d->scratch_virt);

    /* Two descriptors: req (NEXT) -> resp (WRITE).
     * We use slots 0 and 1; with one outstanding command at a time
     * there's no need to track free slots. */
    d->desc[0].addr  = req_phys;
    d->desc[0].len   = (uint32_t)req_len;
    d->desc[0].flags = VQ_DESC_F_NEXT;
    d->desc[0].next  = 1;

    d->desc[1].addr  = resp_phys;
    d->desc[1].len   = (uint32_t)resp_len;
    d->desc[1].flags = VQ_DESC_F_WRITE;
    d->desc[1].next  = 0;

    /* Publish chain head (slot 0) into the avail ring. */
    d->avail_ring[d->avail_idx % d->qsize] = 0;
    d->avail_idx++;
    *d->avail_idx_ptr = d->avail_idx;

    /* Ring doorbell. MMIO stores on x86 are ordered after prior memory
     * writes, so no fence is needed. */
    *d->notify = 0;

    /* Poll for completion. The device increments used.idx by 1 once the
     * chain is fully processed. We bound the wait to ~1s of CPU spin
     * via a generous iteration cap; in practice QEMU completes the
     * command in microseconds. The `pause` hint keeps the spin friendly
     * to SMT siblings -- same trick blk_ahci/blk_nvme use in their own
     * polling loops. */
    for (uint32_t spins = 0; spins < 50000000u; spins++) {
        if (*d->used_idx_ptr != d->used_idx) {
            d->used_idx++;
            return true;
        }
        __asm__ volatile ("pause" ::: "memory");
    }
    kprintf("[virtio-gpu] issue_cmd timed out (req_type=0x%x)\n",
            ((const struct virtio_gpu_ctrl_hdr *)req_buf)->type);
    return false;
}

/* ---- high-level commands ------------------------------------- */

/* Each helper composes its request struct in the scratch page at
 * SCRATCH_REQ_OFF, and asks the device to write the response at
 * SCRATCH_RESP_OFF. Callers can read the response in place. */

static bool cmd_get_display_info(struct vgpu_dev *d,
                                 uint32_t *out_w, uint32_t *out_h) {
    struct virtio_gpu_ctrl_hdr           *req  = (void *)(d->scratch_virt + SCRATCH_REQ_OFF);
    struct virtio_gpu_resp_display_info  *resp = (void *)(d->scratch_virt + SCRATCH_RESP_OFF);

    memset(req,  0, sizeof(*req));
    memset(resp, 0, sizeof(*resp));
    req->type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    if (!issue_cmd(d, req, sizeof(*req), resp, sizeof(*resp))) return false;
    if (resp->hdr.type != VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        kprintf("[virtio-gpu] GET_DISPLAY_INFO bad response type=0x%x\n",
                resp->hdr.type);
        return false;
    }

    /* M27G: cache every pmodes[] entry plus the enabled count so
     * describe() can list secondary outputs. We do this BEFORE the
     * fallback below so a degraded boot still reports the host's
     * advertised topology truthfully. */
    d->total_scanouts   = VIRTIO_GPU_MAX_SCANOUTS;
    d->enabled_scanouts = 0;
    for (int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
        d->scanouts[i] = resp->pmodes[i];
        if (resp->pmodes[i].enabled) d->enabled_scanouts++;
    }
    if (d->enabled_scanouts > 1) {
        kprintf("[virtio-gpu] %u scanouts enabled -- driving scanout 0; "
                "others reported as informational\n",
                d->enabled_scanouts);
        for (uint32_t i = 1; i < d->total_scanouts; i++) {
            if (!d->scanouts[i].enabled) continue;
            kprintf("[virtio-gpu]   scanout %u: %ux%u@(%u,%u) (info only)\n",
                    i,
                    d->scanouts[i].r.width,  d->scanouts[i].r.height,
                    d->scanouts[i].r.x,      d->scanouts[i].r.y);
        }
    }

    if (!resp->pmodes[0].enabled) {
        kprintf("[virtio-gpu] scanout 0 not enabled -- using QEMU default 1024x768\n");
        *out_w = 1024;
        *out_h = 768;
        return true;
    }
    *out_w = resp->pmodes[0].r.width;
    *out_h = resp->pmodes[0].r.height;
    /* Sanity: clamp absurd values that would blow the heap. QEMU's
     * default is 1024x768; -display sdl,window-resize=on can push it
     * much higher. We cap at something reasonable. */
    if (*out_w == 0 || *out_h == 0 || *out_w > 4096 || *out_h > 2160) {
        kprintf("[virtio-gpu] scanout 0: rejected weird geometry %ux%u "
                "-- using 1024x768\n", *out_w, *out_h);
        *out_w = 1024;
        *out_h = 768;
    }
    return true;
}

static bool cmd_resource_create_2d(struct vgpu_dev *d,
                                   uint32_t resource_id,
                                   uint32_t width, uint32_t height) {
    struct virtio_gpu_resource_create_2d *req  = (void *)(d->scratch_virt + SCRATCH_REQ_OFF);
    struct virtio_gpu_ctrl_hdr           *resp = (void *)(d->scratch_virt + SCRATCH_RESP_OFF);

    memset(req,  0, sizeof(*req));
    memset(resp, 0, sizeof(*resp));
    req->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    req->resource_id = resource_id;
    req->format      = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    req->width       = width;
    req->height      = height;

    if (!issue_cmd(d, req, sizeof(*req), resp, sizeof(*resp))) return false;
    if (resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        kprintf("[virtio-gpu] RESOURCE_CREATE_2D bad response type=0x%x\n",
                resp->type);
        return false;
    }
    return true;
}

static bool cmd_resource_attach_backing(struct vgpu_dev *d,
                                        uint32_t resource_id,
                                        uint64_t backing_phys,
                                        uint32_t backing_len) {
    struct virtio_gpu_resource_attach_backing *req
        = (void *)(d->scratch_virt + SCRATCH_REQ_OFF);
    struct virtio_gpu_ctrl_hdr *resp
        = (void *)(d->scratch_virt + SCRATCH_RESP_OFF);

    memset(req,  0, sizeof(*req));
    memset(resp, 0, sizeof(*resp));
    req->hdr.type        = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    req->resource_id     = resource_id;
    req->nr_entries      = 1;
    req->entries[0].addr   = backing_phys;
    req->entries[0].length = backing_len;

    if (!issue_cmd(d, req, sizeof(*req), resp, sizeof(*resp))) return false;
    if (resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        kprintf("[virtio-gpu] RESOURCE_ATTACH_BACKING bad response type=0x%x\n",
                resp->type);
        return false;
    }
    return true;
}

static bool cmd_set_scanout(struct vgpu_dev *d,
                            uint32_t scanout_id, uint32_t resource_id,
                            uint32_t width, uint32_t height) {
    struct virtio_gpu_set_scanout *req
        = (void *)(d->scratch_virt + SCRATCH_REQ_OFF);
    struct virtio_gpu_ctrl_hdr *resp
        = (void *)(d->scratch_virt + SCRATCH_RESP_OFF);

    memset(req,  0, sizeof(*req));
    memset(resp, 0, sizeof(*resp));
    req->hdr.type    = VIRTIO_GPU_CMD_SET_SCANOUT;
    req->r.x         = 0;
    req->r.y         = 0;
    req->r.width     = width;
    req->r.height    = height;
    req->scanout_id  = scanout_id;
    req->resource_id = resource_id;

    if (!issue_cmd(d, req, sizeof(*req), resp, sizeof(*resp))) return false;
    if (resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        kprintf("[virtio-gpu] SET_SCANOUT bad response type=0x%x\n",
                resp->type);
        return false;
    }
    return true;
}

/* M27F: TRANSFER_TO_HOST_2D over an arbitrary sub-rect of the resource.
 *
 * Spec / QEMU semantics (hw/display/virtio-gpu.c::transfer_to_host_2d):
 *
 *   The command copies r.width * r.height pixels from the guest backing
 *   into the host resource at coordinates (r.x, r.y .. r.x+r.w, r.y+r.h).
 *   For each row n in [0, r.h), the device reads
 *
 *       backing[t2d.offset + n * resource_stride .. + r.w * bpp]
 *
 *   where `resource_stride` = resource.width * bpp. Our backing IS a
 *   full-width, native-stride (BGRX8888 at d->width pixels per row)
 *   buffer, so the offset for the first byte of the rect is simply
 *
 *       offset = ry * (d->width * 4) + rx * 4
 *
 *   and each subsequent row is reached automatically by the device
 *   walking forward by stride per row. No per-row issue_cmd needed --
 *   one TRANSFER + one FLUSH per partial present.
 *
 * The full-frame helper below now delegates to this with
 * (0, 0, width, height) so the offset math stays in one place. */
static bool cmd_transfer_to_host_2d_rect(struct vgpu_dev *d,
                                         uint32_t resource_id,
                                         uint32_t rx, uint32_t ry,
                                         uint32_t rw, uint32_t rh) {
    struct virtio_gpu_transfer_to_host_2d *req
        = (void *)(d->scratch_virt + SCRATCH_REQ_OFF);
    struct virtio_gpu_ctrl_hdr *resp
        = (void *)(d->scratch_virt + SCRATCH_RESP_OFF);

    memset(req,  0, sizeof(*req));
    memset(resp, 0, sizeof(*resp));
    req->hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    req->r.x         = rx;
    req->r.y         = ry;
    req->r.width     = rw;
    req->r.height    = rh;
    req->offset      = (uint64_t)ry * (uint64_t)d->width * 4u
                     + (uint64_t)rx * 4u;
    req->resource_id = resource_id;

    if (!issue_cmd(d, req, sizeof(*req), resp, sizeof(*resp))) return false;
    if (resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        kprintf("[virtio-gpu] TRANSFER_TO_HOST_2D rect (%u,%u %ux%u) "
                "bad response type=0x%x\n", rx, ry, rw, rh, resp->type);
        return false;
    }
    return true;
}

static bool cmd_transfer_to_host_2d(struct vgpu_dev *d,
                                    uint32_t resource_id,
                                    uint32_t width, uint32_t height) {
    return cmd_transfer_to_host_2d_rect(d, resource_id, 0, 0, width, height);
}

static bool cmd_resource_flush_rect(struct vgpu_dev *d,
                                    uint32_t resource_id,
                                    uint32_t rx, uint32_t ry,
                                    uint32_t rw, uint32_t rh) {
    struct virtio_gpu_resource_flush *req
        = (void *)(d->scratch_virt + SCRATCH_REQ_OFF);
    struct virtio_gpu_ctrl_hdr *resp
        = (void *)(d->scratch_virt + SCRATCH_RESP_OFF);

    memset(req,  0, sizeof(*req));
    memset(resp, 0, sizeof(*resp));
    req->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    req->r.x         = rx;
    req->r.y         = ry;
    req->r.width     = rw;
    req->r.height    = rh;
    req->resource_id = resource_id;

    if (!issue_cmd(d, req, sizeof(*req), resp, sizeof(*resp))) return false;
    if (resp->type != VIRTIO_GPU_RESP_OK_NODATA) {
        kprintf("[virtio-gpu] RESOURCE_FLUSH rect (%u,%u %ux%u) "
                "bad response type=0x%x\n", rx, ry, rw, rh, resp->type);
        return false;
    }
    return true;
}

static bool cmd_resource_flush(struct vgpu_dev *d,
                               uint32_t resource_id,
                               uint32_t width, uint32_t height) {
    return cmd_resource_flush_rect(d, resource_id, 0, 0, width, height);
}

/* ---- backend (called from gfx_flip) ------------------------- */

static void vgpu_flip(void) {
    struct vgpu_dev *d = &g_vgpu;
    if (!g_vgpu_active) return;

    /* 1. Pull the freshly-composed back buffer into our PMM backing.
     *    The compositor wrote into gfx_backbuf() (a kmalloc'd page-
     *    fragmented but virt-contiguous region); we copy it into the
     *    physically-contiguous backing the device knows about. */
    uint32_t *back = gfx_backbuf();
    if (!back) return;
    size_t pixels = (size_t)d->width * d->height;
    memcpy(d->backing_virt, back, pixels * 4u);

    /* 2. Tell the device the host-side resource is dirty. */
    if (!cmd_transfer_to_host_2d(d, VG_RESOURCE_ID, d->width, d->height)) {
        /* If the transfer fails we mark the backend dead so future
         * flips fall straight back to the (still-installed) Limine
         * memcpy path. Defensive -- we've never seen this in QEMU. */
        kprintf("[virtio-gpu] flush: TRANSFER failed, deactivating backend\n");
        g_vgpu_active = false;
        gfx_set_backend(0);
        return;
    }

    /* 3. Ask the host to push the resource to the screen. */
    if (!cmd_resource_flush(d, VG_RESOURCE_ID, d->width, d->height)) {
        kprintf("[virtio-gpu] flush: FLUSH failed, deactivating backend\n");
        g_vgpu_active = false;
        gfx_set_backend(0);
        return;
    }

    d->full_flips++;
}

/* M27F: present_rect -- partial TRANSFER_TO_HOST_2D + RESOURCE_FLUSH.
 *
 * The gfx layer (gfx_flip) calls this when it has a dirty union that
 * is strictly smaller than the surface AND the active backend exposes
 * present_rect. We only memcpy the dirty region from the back buffer
 * into the physically-contiguous backing -- the rest of the backing
 * keeps its previous (already-on-the-screen) content, since the GPU
 * is told to re-read just the rect.
 *
 * Caller has already clipped the rect to the surface, but we re-clip
 * defensively because virtio-gpu rejects out-of-bounds rects with a
 * VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER and the comparison to the
 * scanout dims is cheap.
 *
 * Failure in either command falls back exactly like vgpu_flip(): the
 * backend deactivates itself and gfx_set_backend(0) reinstates the
 * Limine memcpy path. The caller (gfx_flip) doesn't observe the
 * fallback because the next flip already routes through the new
 * default backend. */
static void vgpu_present_rect(int x, int y, int w, int h) {
    struct vgpu_dev *d = &g_vgpu;
    if (!g_vgpu_active) return;
    if (w <= 0 || h <= 0) return;

    /* Defensive clip to scanout extent. */
    if (x < 0)                  { w += x; x = 0; }
    if (y < 0)                  { h += y; y = 0; }
    if (x >= (int)d->width)     return;
    if (y >= (int)d->height)    return;
    if (x + w > (int)d->width)  w = (int)d->width  - x;
    if (y + h > (int)d->height) h = (int)d->height - y;
    if (w <= 0 || h <= 0)       return;

    uint32_t *back = gfx_backbuf();
    if (!back) return;

    /* Copy only the dirty rect from back -> backing, row by row. The
     * stride is the same for both buffers (full image width). */
    uint32_t *dst_base = (uint32_t *)d->backing_virt;
    size_t    stride_px = d->width;
    for (int row = 0; row < h; row++) {
        const uint32_t *src = back     + (size_t)(y + row) * stride_px + x;
        uint32_t       *dst = dst_base + (size_t)(y + row) * stride_px + x;
        memcpy(dst, src, (size_t)w * 4u);
    }

    if (!cmd_transfer_to_host_2d_rect(d, VG_RESOURCE_ID,
                                      (uint32_t)x, (uint32_t)y,
                                      (uint32_t)w, (uint32_t)h)) {
        kprintf("[virtio-gpu] present_rect: TRANSFER failed, "
                "deactivating backend\n");
        g_vgpu_active = false;
        gfx_set_backend(0);
        return;
    }
    if (!cmd_resource_flush_rect(d, VG_RESOURCE_ID,
                                 (uint32_t)x, (uint32_t)y,
                                 (uint32_t)w, (uint32_t)h)) {
        kprintf("[virtio-gpu] present_rect: FLUSH failed, "
                "deactivating backend\n");
        g_vgpu_active = false;
        gfx_set_backend(0);
        return;
    }
    d->partial_flips  += 1;
    d->partial_pixels += (uint64_t)w * (uint64_t)h;
}

/* M27F: virtio-gpu describe -- short string for displayinfo --json /
 * display_render. Surfaces:
 *   - PCI BDF (so multi-GPU configs are unambiguous)
 *   - reported scanout geometry + backing footprint (KiB)
 *   - whether MSI-X is wired (and on which IDT vector)
 *   - per-region transfer counters (proves M27E partial path is hot)
 *   - M27G: count of host-enabled scanouts (informational; we only
 *     drive scanout 0, but the desktop layer can later use this to
 *     surface "secondary monitors available" UI)
 *
 * Bounded: caps at `cap` bytes; ksnprintf returns the bytes that would
 * have been written, but the real output is truncated to fit. */
static int vgpu_describe(char *extra, int cap) {
    struct vgpu_dev *d = &g_vgpu;
    if (!extra || cap <= 0) return 0;
    return ksnprintf(extra, (size_t)cap,
                     "pci=%02x:%02x.%x scanout=%ux%u backing_kib=%lu "
                     "irq=%s/0x%02x partial=%lu/full=%lu "
                     "scanouts=%u/%u",
                     d->pci_bus, d->pci_slot, d->pci_fn,
                     d->width, d->height,
                     (unsigned long)((size_t)d->width * d->height
                                     * 4u / 1024u),
                     d->irq_enabled ? "msix" : "poll",
                     (unsigned)d->irq_vector,
                     (unsigned long)d->partial_flips,
                     (unsigned long)d->full_flips,
                     (unsigned)d->enabled_scanouts,
                     (unsigned)d->total_scanouts);
}

/* M27F: present_rect now wired. The gfx layer (src/gfx.c::gfx_flip)
 * picks this up automatically and routes the dirty-rect path through
 * a single TRANSFER + FLUSH over only the dirty union, instead of the
 * full-screen memcpy + transfer the .flip() path does. */
static const struct gfx_backend g_vgpu_backend = {
    .flip            = vgpu_flip,
    .present_rect    = vgpu_present_rect,
    .describe        = vgpu_describe,
    .name            = "virtio-gpu",
    .bytes_per_pixel = 4,
};

/* ---- capability walk (mirrors virtio_net.c) ----------------- */

struct vgpu_cap {
    bool     present;
    uint8_t  bar;
    uint32_t offset;
    uint32_t length;
};

static bool find_virtio_caps(struct pci_dev *dev,
                             struct vgpu_cap caps[6],
                             uint32_t *out_notify_mult) {
    bool got_common = false, got_notify = false, got_device = false;
    *out_notify_mult = 0;

    for (uint8_t off = pci_cap_first(dev); off; off = pci_cap_next(dev, off)) {
        uint8_t id = pci_cfg_read8(dev->bus, dev->slot, dev->fn, off);
        if (id != PCI_CAP_ID_VENDOR) continue;

        uint8_t  cfg_type = pci_cfg_read8 (dev->bus, dev->slot, dev->fn, off + 3);
        uint8_t  bar      = pci_cfg_read8 (dev->bus, dev->slot, dev->fn, off + 4);
        uint32_t bar_off  = pci_cfg_read32(dev->bus, dev->slot, dev->fn, off + 8);
        uint32_t length   = pci_cfg_read32(dev->bus, dev->slot, dev->fn, off + 12);

        if (cfg_type < 1 || cfg_type > 5) continue;
        caps[cfg_type].present = true;
        caps[cfg_type].bar     = bar;
        caps[cfg_type].offset  = bar_off;
        caps[cfg_type].length  = length;

        if (cfg_type == VIRTIO_PCI_CAP_COMMON_CFG) got_common = true;
        if (cfg_type == VIRTIO_PCI_CAP_DEVICE_CFG) got_device = true;
        if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
            got_notify = true;
            *out_notify_mult = pci_cfg_read32(dev->bus, dev->slot, dev->fn,
                                              off + 16);
        }
    }
    return got_common && got_notify && got_device;
}

/* ---- PCI probe ----------------------------------------------- */

static int virtio_gpu_probe(struct pci_dev *dev) {
    if (g_vgpu_bound) {
        kprintf("[virtio-gpu] already bound -- ignoring %02x:%02x.%x\n",
                dev->bus, dev->slot, dev->fn);
        return -1;
    }

    kprintf("[virtio-gpu] probing %02x:%02x.%x  (vid:did %04x:%04x)\n",
            dev->bus, dev->slot, dev->fn, dev->vendor, dev->device);

    pci_dev_enable(dev, PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);

    struct vgpu_cap caps[6];
    memset(caps, 0, sizeof(caps));
    uint32_t notify_mult = 0;
    if (!find_virtio_caps(dev, caps, &notify_mult)) {
        kprintf("[virtio-gpu] %02x:%02x.%x: no modern virtio caps -- declining\n",
                dev->bus, dev->slot, dev->fn);
        return -2;
    }

    struct vgpu_dev *d = &g_vgpu;
    memset(d, 0, sizeof(*d));
    d->notify_mult = notify_mult;
    d->pci_bus     = dev->bus;
    d->pci_slot    = dev->slot;
    d->pci_fn      = dev->fn;

    /* Map only the BARs we drive (skip ISR_CFG -- we poll -- and
     * PCI_CFG -- the alternate-access window). */
    static const int needed_caps[] = {
        VIRTIO_PCI_CAP_COMMON_CFG,
        VIRTIO_PCI_CAP_NOTIFY_CFG,
        VIRTIO_PCI_CAP_DEVICE_CFG,
    };
    void *bars[PCI_BAR_COUNT] = {0};
    for (size_t k = 0; k < sizeof(needed_caps) / sizeof(needed_caps[0]); k++) {
        int t = needed_caps[k];
        uint8_t bi = caps[t].bar;
        if (bi >= PCI_BAR_COUNT) {
            kprintf("[virtio-gpu] cfg_type %d: bogus BAR index %u\n", t, bi);
            return -3;
        }
        if (!bars[bi]) {
            bars[bi] = pci_map_bar(dev, bi, 0);
            if (!bars[bi]) {
                kprintf("[virtio-gpu] BAR%u map failed (phys=%p)\n",
                        bi, (void *)dev->bar[bi]);
                return -4;
            }
        }
    }

    d->common      = (volatile uint8_t *)bars[caps[VIRTIO_PCI_CAP_COMMON_CFG].bar]
                   + caps[VIRTIO_PCI_CAP_COMMON_CFG].offset;
    d->notify_base = (volatile uint8_t *)bars[caps[VIRTIO_PCI_CAP_NOTIFY_CFG].bar]
                   + caps[VIRTIO_PCI_CAP_NOTIFY_CFG].offset;
    d->device_cfg  = (volatile uint8_t *)bars[caps[VIRTIO_PCI_CAP_DEVICE_CFG].bar]
                   + caps[VIRTIO_PCI_CAP_DEVICE_CFG].offset;

    kprintf("[virtio-gpu] common=%p notify=%p (mult=%u) device=%p\n",
            (void *)d->common, (void *)d->notify_base,
            d->notify_mult, (void *)d->device_cfg);

    /* Reset, ACK, DRIVER. */
    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS, 0);
    for (int i = 0; i < 100000; i++) {
        if (cfg_r8(d, VIRTIO_PCI_DEVICE_STATUS) == 0) break;
    }
    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS,
           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Negotiate features. We only need VIRTIO_F_VERSION_1. The
     * GPU-specific feature bits (VIRGL, EDID, RESOURCE_BLOB,
     * CONTEXT_INIT) are all advanced/3D features we don't use. */
    cfg_w32(d, VIRTIO_PCI_DEVICE_FEATURE_SELECT, 1);
    uint32_t devf_hi = cfg_r32(d, VIRTIO_PCI_DEVICE_FEATURE);
    cfg_w32(d, VIRTIO_PCI_DEVICE_FEATURE_SELECT, 0);
    uint32_t devf_lo = cfg_r32(d, VIRTIO_PCI_DEVICE_FEATURE);

    if (!(devf_hi & (1u << (VIRTIO_F_VERSION_1 - 32)))) {
        kprintf("[virtio-gpu] device does not advertise VIRTIO_F_VERSION_1 "
                "-- aborting\n");
        cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -5;
    }
    cfg_w32(d, VIRTIO_PCI_DRIVER_FEATURE_SELECT, 0);
    cfg_w32(d, VIRTIO_PCI_DRIVER_FEATURE,        0);
    cfg_w32(d, VIRTIO_PCI_DRIVER_FEATURE_SELECT, 1);
    cfg_w32(d, VIRTIO_PCI_DRIVER_FEATURE,        1u << (VIRTIO_F_VERSION_1 - 32));

    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS,
           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
           VIRTIO_STATUS_FEATURES_OK);
    if (!(cfg_r8(d, VIRTIO_PCI_DEVICE_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        kprintf("[virtio-gpu] device cleared FEATURES_OK -- subset rejected\n");
        cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -6;
    }
    kprintf("[virtio-gpu] features: device=0x%08x_%08x driver=0x%08x_%08x\n",
            devf_hi, devf_lo,
            1u << (VIRTIO_F_VERSION_1 - 32), 0u);

    /* Try MSI-X for the controlq. Fall back to no IRQ if the device
     * has no MSI-X cap (rare on modern virtio, but the legacy path
     * is "device fires INTx that we ignore -- the busy-poll inside
     * issue_cmd still resolves every command"). */
    uint16_t cq_vec = VIRTIO_MSI_NO_VECTOR;
    uint8_t  vec    = irq_alloc_vector(vgpu_irq_handler, d);
    if (vec == 0) {
        kprintf("[virtio-gpu] no IDT vectors free -- staying polled\n");
    } else if (!pci_msix_enable(dev, vec, (uint8_t)apic_read_id(), 1u)) {
        kprintf("[virtio-gpu] no MSI-X cap -- staying polled "
                "(vec 0x%02x is now idle)\n", (unsigned)vec);
    } else {
        d->irq_vector  = vec;
        d->irq_enabled = true;
        cq_vec         = 0;
        cfg_w16(d, VIRTIO_PCI_MSIX_CONFIG, VIRTIO_MSI_NO_VECTOR);
    }

    /* Bring up the controlq (vq 0). The cursor queue (vq 1) is
     * intentionally skipped -- we draw the cursor in software via
     * gfx_draw_cursor(). */
    if (!vgpu_setup_controlq(d, cq_vec)) return -7;

    if (d->irq_enabled) {
        kprintf("[virtio-gpu] IRQ live on vec 0x%02x  controlq=msix0\n",
                (unsigned)d->irq_vector);
    }

    /* Allocate the per-command scratch page (request + response). */
    d->scratch_phys = pmm_alloc_page();
    if (!d->scratch_phys) {
        kprintf("[virtio-gpu] OOM allocating scratch page\n");
        return -8;
    }
    d->scratch_virt = (uint8_t *)pmm_phys_to_virt(d->scratch_phys);
    memset(d->scratch_virt, 0, PAGE_SIZE);

    /* DRIVER_OK -- the device will now process commands. */
    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS,
           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
           VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    /* Display setup, four steps. */
    if (!cmd_get_display_info(d, &d->width, &d->height)) return -9;
    kprintf("[virtio-gpu] scanout 0: %ux%u (preferred)\n", d->width, d->height);

    d->backing_bytes = (size_t)d->width * d->height * 4u;
    d->backing_pages = (d->backing_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    d->backing_phys  = pmm_alloc_pages(d->backing_pages);
    if (!d->backing_phys) {
        kprintf("[virtio-gpu] OOM allocating %lu-page (%lu KiB) backing\n",
                (unsigned long)d->backing_pages,
                (unsigned long)(d->backing_bytes / 1024));
        return -10;
    }
    d->backing_virt = (uint8_t *)pmm_phys_to_virt(d->backing_phys);
    memset(d->backing_virt, 0, d->backing_bytes);
    kprintf("[virtio-gpu] backing: %lu KiB at phys=%p (%lu pages contiguous)\n",
            (unsigned long)(d->backing_bytes / 1024),
            (void *)d->backing_phys,
            (unsigned long)d->backing_pages);

    if (!cmd_resource_create_2d(d, VG_RESOURCE_ID, d->width, d->height))
        return -11;
    kprintf("[virtio-gpu] RESOURCE_CREATE_2D ok (id=%u, BGRX, %ux%u)\n",
            VG_RESOURCE_ID, d->width, d->height);

    if (!cmd_resource_attach_backing(d, VG_RESOURCE_ID,
                                     d->backing_phys,
                                     (uint32_t)d->backing_bytes))
        return -12;
    kprintf("[virtio-gpu] RESOURCE_ATTACH_BACKING ok\n");

    if (!cmd_set_scanout(d, 0, VG_RESOURCE_ID, d->width, d->height))
        return -13;
    kprintf("[virtio-gpu] SET_SCANOUT 0 -> resource %u ok\n", VG_RESOURCE_ID);

    g_vgpu_bound     = true;
    dev->driver_data = d;
    kprintf("[virtio-gpu] device live -- waiting for gfx_layer_init to "
            "install backend\n");
    return 0;
}

static const struct pci_match g_vgpu_matches[] = {
    /* Modern non-transitional virtio-gpu only. The legacy/transitional
     * id 0x1040+0x10 = 0x1050 collision is intentional in the spec
     * (modern device id = 0x1040 + virtio_device_id; for GPU that's 16).
     * We don't try to drive the truly-legacy transitional 0x1010 line
     * (it would require I/O-port virtio, which we don't speak). */
    { VIRTIO_VENDOR, VIRTIO_GPU_DEV_MODERN,
      PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    PCI_MATCH_END,
};

static struct pci_driver g_vgpu_driver = {
    .name    = "virtio-gpu",
    .matches = g_vgpu_matches,
    .probe   = virtio_gpu_probe,
    .remove  = 0,
};

void virtio_gpu_register(void) {
    pci_register_driver(&g_vgpu_driver);
}

bool virtio_gpu_present(void) {
    return g_vgpu_bound;
}

void virtio_gpu_install_backend(void) {
    if (!g_vgpu_bound) {
        /* No GPU found at probe time. Silent -- gfx_flip stays on the
         * Limine memcpy path, which is exactly what we want. */
        return;
    }

    struct vgpu_dev *d = &g_vgpu;

    /* Three install paths depending on what gfx_layer_init produced:
     *
     *   1. gfx is ready AND its dims match the GPU's preferred
     *      resolution. Hot path: install backend, share the back
     *      buffer the compositor was already drawing into.
     *
     *   2. gfx is ready but dims DIFFER. This happens when the
     *      vgabios option ROM gave Limine one mode and GET_DISPLAY_INFO
     *      reports a different one. Punt: leave the Limine memcpy
     *      backend alone -- pixels still reach the screen via the
     *      vgabios-emulated framebuffer path. (Re-running gfx_init
     *      at the GPU's resolution would reallocate the back buffer
     *      mid-boot and is more disruptive than it's worth.)
     *
     *   3. gfx is NOT ready. Limine never produced a framebuffer
     *      (typical for `-vga none + virtio-gpu` on BIOS, since
     *      Limine has no virtio-gpu driver of its own; also some
     *      headless/cloud HW). Bring gfx up ourselves, pointing it
     *      at our PMM backing as the "framebuffer". The default
     *      Limine backend's flip() would happily memcpy into that
     *      backing, but our backend overrides flip() to add the
     *      mandatory TRANSFER_TO_HOST_2D + RESOURCE_FLUSH afterwards
     *      so the device actually scans it out. */
    if (!gfx_ready()) {
        if (!gfx_init(d->backing_virt, (uint64_t)d->width * 4,
                      d->width, d->height)) {
            kprintf("[virtio-gpu] gfx_init(virtio-gpu backing) failed -- "
                    "backend NOT installed (display unavailable)\n");
            return;
        }
        kprintf("[virtio-gpu] gfx initialised against GPU backing %ux%u "
                "(no Limine FB)\n", d->width, d->height);
    } else if (gfx_width() != d->width || gfx_height() != d->height) {
        kprintf("[virtio-gpu] dim mismatch: gfx=%ux%u, gpu=%ux%u -- "
                "backend NOT installed (Limine FB fallback active)\n",
                gfx_width(), gfx_height(), d->width, d->height);
        return;
    }

    g_vgpu_active = true;
    gfx_set_backend(&g_vgpu_backend);
    kprintf("[virtio-gpu] backend installed -- gfx_flip now uses "
            "TRANSFER+FLUSH on scanout 0 (%ux%u)\n", d->width, d->height);
}
