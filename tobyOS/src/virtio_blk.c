/* virtio_blk.c -- modern virtio-blk-pci block driver (Milestone 35B).
 *
 * Bound through the milestone-21 PCI driver registry. Matches Red
 * Hat's virtio vendor (0x1AF4) at either device 0x1001 (legacy /
 * transitional virtio-blk) or 0x1042 (modern non-transitional
 * virtio-blk). Either way we drive the MODERN (V1) transport:
 * legacy I/O-port virtio is intentionally not implemented (mirrors
 * virtio_net.c). If a transitional device doesn't expose the
 * modern caps we decline cleanly.
 *
 * Goals (M35B):
 *   - Make `qemu -drive ...,if=virtio` produce a usable block device.
 *   - Register that device with the blk subsystem so partition_scan
 *     + the existing /data mount logic work unchanged.
 *   - Stay simple: single virtqueue, polled completion, one in-flight
 *     request at a time. A future milestone can add MSI-X /
 *     pipelining once the basics are paying off.
 *
 * Layout of every request: three descriptors chained head -> next ->
 * next, all referring to driver-owned DMA buffers:
 *
 *   desc[0]  outhdr  (16 B, device-READABLE)
 *   desc[1]  data    (n*512 B, device-WRITABLE for READ / READABLE for WRITE)
 *   desc[2]  status  (1  B, device-WRITABLE)
 *
 * We cap n at VBLK_MAX_SECTORS_PER_REQ; the read/write wrapper loops
 * for larger transfers. This bounds the data DMA buffer to one page,
 * keeps the per-request setup branch-free, and matches what most
 * vhost-blk backends prefer.
 */

#include <tobyos/blk.h>
#include <tobyos/pci.h>
#include <tobyos/pmm.h>
#include <tobyos/vmm.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/slog.h>

/* tobyOS does not export a cpu_pause() helper; every poll-loop in the
 * tree open-codes the PAUSE hint inline. Match the local convention. */
static inline void vblk_pause(void) {
    __asm__ volatile ("pause" ::: "memory");
}

/* ---- PCI vendor / device --------------------------------------- */

#define VIRTIO_VENDOR              0x1AF4
#define VIRTIO_BLK_DEV_LEGACY      0x1001
#define VIRTIO_BLK_DEV_MODERN      0x1042

/* ---- virtio cap cfg_type values -------------------------------- */

#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_ISR_CFG     3
#define VIRTIO_PCI_CAP_DEVICE_CFG  4
#define VIRTIO_PCI_CAP_PCI_CFG     5

/* ---- common config offsets ------------------------------------- */

#define VIRTIO_PCI_DEVICE_FEATURE_SELECT  0x00
#define VIRTIO_PCI_DEVICE_FEATURE         0x04
#define VIRTIO_PCI_DRIVER_FEATURE_SELECT  0x08
#define VIRTIO_PCI_DRIVER_FEATURE         0x0C
#define VIRTIO_PCI_MSIX_CONFIG            0x10
#define VIRTIO_PCI_NUM_QUEUES             0x12
#define VIRTIO_PCI_DEVICE_STATUS          0x14
#define VIRTIO_PCI_QUEUE_SELECT           0x16
#define VIRTIO_PCI_QUEUE_SIZE             0x18
#define VIRTIO_PCI_QUEUE_MSIX_VECTOR      0x1A
#define VIRTIO_PCI_QUEUE_ENABLE           0x1C
#define VIRTIO_PCI_QUEUE_NOTIFY_OFF       0x1E
#define VIRTIO_PCI_QUEUE_DESC             0x20   /* 64-bit */
#define VIRTIO_PCI_QUEUE_DRIVER           0x28   /* 64-bit */
#define VIRTIO_PCI_QUEUE_DEVICE           0x30   /* 64-bit */

#define VIRTIO_STATUS_ACKNOWLEDGE   1
#define VIRTIO_STATUS_DRIVER        2
#define VIRTIO_STATUS_DRIVER_OK     4
#define VIRTIO_STATUS_FEATURES_OK   8
#define VIRTIO_STATUS_FAILED      128

#define VIRTIO_F_VERSION_1          32
#define VIRTIO_BLK_F_BLK_SIZE        6  /* device_cfg has block_size */
#define VIRTIO_BLK_F_RO              5  /* read-only device          */

#define VIRTIO_MSI_NO_VECTOR    0xFFFFu

#define VQ_DESC_F_NEXT   1
#define VQ_DESC_F_WRITE  2

/* virtio-blk request types. */
#define VIRTIO_BLK_T_IN          0
#define VIRTIO_BLK_T_OUT         1
#define VIRTIO_BLK_T_FLUSH       4

/* status byte returned by the device. */
#define VIRTIO_BLK_S_OK          0
#define VIRTIO_BLK_S_IOERR       1
#define VIRTIO_BLK_S_UNSUPP      2

struct __attribute__((packed)) virtio_blk_outhdr {
    uint32_t type;
    uint32_t ioprio;
    uint64_t sector;
};
_Static_assert(sizeof(struct virtio_blk_outhdr) == 16, "virtio_blk_outhdr");

struct __attribute__((packed)) virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct __attribute__((packed)) virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

/* device_cfg layout (modern). We only read capacity + block_size. */
#define VBLK_CFG_CAPACITY    0  /* uint64, in 512-B sectors          */
#define VBLK_CFG_BLK_SIZE    20 /* uint32, optional, gated on F_BLK_SIZE */

#define VBLK_QSIZE                 16u
#define VBLK_MAX_SECTORS_PER_REQ   8u   /* 4 KiB data DMA per request */

/* Queue page layout: desc table at 0, avail at 256, used at 1024.
 * desc table is 16 entries * 16 bytes = 256 B; avail = 4 + 32 + 2 = 38 B
 * (round to 1024 for alignment). One 4K page is plenty. */
#define VQ_DESC_OFF   0u
#define VQ_AVAIL_OFF  256u
#define VQ_USED_OFF   1024u

struct vblk_dev {
    volatile uint8_t  *common;
    volatile uint8_t  *device_cfg;
    volatile uint8_t  *notify_base;
    uint32_t           notify_mult;

    /* Single virtqueue (queue 0). */
    uint16_t           qsize;
    uint16_t           avail_idx;
    uint16_t           used_idx;

    uint64_t           ring_phys;
    uint8_t           *ring;

    struct virtq_desc *desc;
    volatile uint16_t *avail_idx_ptr;
    volatile uint16_t *avail_ring;
    volatile uint16_t *used_idx_ptr;
    struct virtq_used_elem *used_ring;
    volatile uint16_t *notify;

    /* Pre-allocated DMA scratch (one page each, only the first few
     * bytes of hdr/status are used). */
    uint64_t           hdr_phys;
    struct virtio_blk_outhdr *hdr;
    uint64_t           data_phys;
    uint8_t           *data;
    uint64_t           status_phys;
    volatile uint8_t  *status;

    uint64_t           capacity_lba;
    uint32_t           negotiated_blk_size;   /* 0 if not negotiated */
    bool               read_only;

    char               name[32];
    struct blk_dev     blk;
    struct blk_ops     ops;
};

static struct vblk_dev g_vblk;
static bool            g_vblk_bound;
static unsigned        g_vblk_index;

/* ---- MMIO helpers --------------------------------------------- */

static inline uint16_t cfg_r16(struct vblk_dev *d, uint32_t off) {
    return *(volatile uint16_t *)(d->common + off);
}
static inline uint32_t cfg_r32(struct vblk_dev *d, uint32_t off) {
    return *(volatile uint32_t *)(d->common + off);
}
static inline uint8_t cfg_r8(struct vblk_dev *d, uint32_t off) {
    return *(volatile uint8_t *)(d->common + off);
}
static inline void cfg_w8(struct vblk_dev *d, uint32_t off, uint8_t v) {
    *(volatile uint8_t *)(d->common + off) = v;
}
static inline void cfg_w16(struct vblk_dev *d, uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(d->common + off) = v;
}
static inline void cfg_w32(struct vblk_dev *d, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(d->common + off) = v;
}
static inline void cfg_w64(struct vblk_dev *d, uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(d->common + off)     = (uint32_t)(v & 0xFFFFFFFFu);
    *(volatile uint32_t *)(d->common + off + 4) = (uint32_t)(v >> 32);
}

/* ---- capability walk (mirrors virtio_net.c) ------------------- */

struct vblk_cap {
    bool     present;
    uint8_t  bar;
    uint32_t offset;
    uint32_t length;
};

static bool find_virtio_caps(struct pci_dev *dev,
                             struct vblk_cap caps[6],
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
            *out_notify_mult =
                pci_cfg_read32(dev->bus, dev->slot, dev->fn, off + 16);
        }
    }
    return got_common && got_notify && got_device;
}

static volatile uint8_t *map_cap_region(struct pci_dev *dev,
                                        struct vblk_cap *cap) {
    if (!cap->present) return NULL;
    uint64_t bar_phys = dev->bar[cap->bar];
    if (!bar_phys || dev->bar_is_io[cap->bar]) {
        kprintf("[virtio-blk] cap on BAR%u: phys=%lx io=%d -- declining\n",
                cap->bar, (unsigned long)bar_phys,
                (int)dev->bar_is_io[cap->bar]);
        return NULL;
    }
    /* Use the high-level pci_map_bar helper -- it caches per-BAR maps
     * and applies VMM_NOCACHE for us. */
    void *base = pci_map_bar(dev, cap->bar, 0);
    if (!base) {
        kprintf("[virtio-blk] cap BAR%u map failed\n", cap->bar);
        return NULL;
    }
    return (volatile uint8_t *)((uintptr_t)base + cap->offset);
}

/* ---- queue setup ---------------------------------------------- */

static bool setup_queue(struct vblk_dev *d) {
    cfg_w16(d, VIRTIO_PCI_QUEUE_SELECT, 0);
    uint16_t max_qs = cfg_r16(d, VIRTIO_PCI_QUEUE_SIZE);
    if (max_qs < VBLK_QSIZE) {
        kprintf("[virtio-blk] queue 0 max_size=%u < %u (unsupported)\n",
                max_qs, VBLK_QSIZE);
        return false;
    }
    d->qsize     = VBLK_QSIZE;
    d->avail_idx = 0;
    d->used_idx  = 0;
    cfg_w16(d, VIRTIO_PCI_QUEUE_SIZE, VBLK_QSIZE);

    d->ring_phys = pmm_alloc_page();
    if (!d->ring_phys) return false;
    d->ring = (uint8_t *)pmm_phys_to_virt(d->ring_phys);
    memset(d->ring, 0, PAGE_SIZE);

    d->desc          = (struct virtq_desc *)(d->ring + VQ_DESC_OFF);
    d->avail_idx_ptr = (volatile uint16_t *)(d->ring + VQ_AVAIL_OFF + 2);
    d->avail_ring    = (volatile uint16_t *)(d->ring + VQ_AVAIL_OFF + 4);
    d->used_idx_ptr  = (volatile uint16_t *)(d->ring + VQ_USED_OFF + 2);
    d->used_ring     = (struct virtq_used_elem *)(d->ring + VQ_USED_OFF + 4);

    cfg_w64(d, VIRTIO_PCI_QUEUE_DESC,   d->ring_phys + VQ_DESC_OFF);
    cfg_w64(d, VIRTIO_PCI_QUEUE_DRIVER, d->ring_phys + VQ_AVAIL_OFF);
    cfg_w64(d, VIRTIO_PCI_QUEUE_DEVICE, d->ring_phys + VQ_USED_OFF);

    uint16_t qoff = cfg_r16(d, VIRTIO_PCI_QUEUE_NOTIFY_OFF);
    d->notify = (volatile uint16_t *)
                (d->notify_base + (uint32_t)qoff * d->notify_mult);

    cfg_w16(d, VIRTIO_PCI_QUEUE_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);
    cfg_w16(d, VIRTIO_PCI_QUEUE_ENABLE, 1);

    /* Pre-allocate the per-request DMA scratch buffers. */
    d->hdr_phys = pmm_alloc_page();
    d->data_phys = pmm_alloc_page();
    d->status_phys = pmm_alloc_page();
    if (!d->hdr_phys || !d->data_phys || !d->status_phys) return false;
    d->hdr    = (struct virtio_blk_outhdr *)pmm_phys_to_virt(d->hdr_phys);
    d->data   = (uint8_t *)pmm_phys_to_virt(d->data_phys);
    d->status = (volatile uint8_t *)pmm_phys_to_virt(d->status_phys);
    memset(d->hdr, 0, PAGE_SIZE);
    memset(d->data, 0, PAGE_SIZE);
    *d->status = 0xFF;

    return true;
}

/* ---- single request ------------------------------------------- */

static int submit_request(struct vblk_dev *d, uint32_t type,
                          uint64_t sector, uint16_t nsec, bool is_write,
                          const void *write_src, void *read_dst) {
    if (nsec > VBLK_MAX_SECTORS_PER_REQ) return -1;
    if (nsec == 0 && type != VIRTIO_BLK_T_FLUSH) return 0;
    if (is_write && d->read_only) return -1;

    uint32_t data_len = (uint32_t)nsec * BLK_SECTOR_SIZE;

    d->hdr->type   = type;
    d->hdr->ioprio = 0;
    d->hdr->sector = sector;

    if (is_write) {
        if (data_len > 0 && write_src) memcpy(d->data, write_src, data_len);
    }

    d->desc[0].addr  = d->hdr_phys;
    d->desc[0].len   = sizeof(struct virtio_blk_outhdr);
    d->desc[0].flags = VQ_DESC_F_NEXT;
    d->desc[0].next  = 1;

    d->desc[1].addr  = d->data_phys;
    d->desc[1].len   = data_len;
    /* read = device WRITES into data buffer; write = device READS from it */
    d->desc[1].flags = VQ_DESC_F_NEXT | (is_write ? 0 : VQ_DESC_F_WRITE);
    d->desc[1].next  = 2;

    d->desc[2].addr  = d->status_phys;
    d->desc[2].len   = 1;
    d->desc[2].flags = VQ_DESC_F_WRITE;
    d->desc[2].next  = 0;

    *d->status = 0xFF;

    d->avail_ring[d->avail_idx % d->qsize] = 0; /* head desc index = 0 */
    d->avail_idx++;
    *d->avail_idx_ptr = d->avail_idx;

    /* Doorbell. */
    *d->notify = 0;

    /* Poll for completion. We deliberately do NOT have a timeout here:
     * a hung virtio-blk device is almost certainly a host configuration
     * bug, and quietly continuing past it would corrupt the FS. */
    while (d->used_idx == *d->used_idx_ptr) {
        vblk_pause();
    }
    d->used_idx++;

    uint8_t st = *d->status;
    if (st != VIRTIO_BLK_S_OK) {
        kprintf("[virtio-blk] request failed: type=%u sector=%lu "
                "nsec=%u status=%u\n",
                (unsigned)type, (unsigned long)sector, (unsigned)nsec,
                (unsigned)st);
        return -1;
    }
    if (!is_write && data_len > 0 && read_dst) {
        memcpy(read_dst, d->data, data_len);
    }
    return 0;
}

static int vblk_read_op(struct blk_dev *bd, uint64_t lba, uint32_t count,
                        void *buf) {
    struct vblk_dev *d = (struct vblk_dev *)bd->priv;
    if (!d || count == 0) return 0;
    if (lba + count > d->capacity_lba) return -1;
    uint8_t *p = (uint8_t *)buf;
    while (count > 0) {
        uint16_t n = (count > VBLK_MAX_SECTORS_PER_REQ)
                         ? (uint16_t)VBLK_MAX_SECTORS_PER_REQ
                         : (uint16_t)count;
        int rc = submit_request(d, VIRTIO_BLK_T_IN, lba, n,
                                false, NULL, p);
        if (rc != 0) return rc;
        lba   += n;
        count -= n;
        p     += (uint32_t)n * BLK_SECTOR_SIZE;
    }
    return 0;
}

static int vblk_write_op(struct blk_dev *bd, uint64_t lba, uint32_t count,
                         const void *buf) {
    struct vblk_dev *d = (struct vblk_dev *)bd->priv;
    if (!d || count == 0) return 0;
    if (d->read_only) return -1;
    if (lba + count > d->capacity_lba) return -1;
    const uint8_t *p = (const uint8_t *)buf;
    while (count > 0) {
        uint16_t n = (count > VBLK_MAX_SECTORS_PER_REQ)
                         ? (uint16_t)VBLK_MAX_SECTORS_PER_REQ
                         : (uint16_t)count;
        int rc = submit_request(d, VIRTIO_BLK_T_OUT, lba, n,
                                true, p, NULL);
        if (rc != 0) return rc;
        lba   += n;
        count -= n;
        p     += (uint32_t)n * BLK_SECTOR_SIZE;
    }
    return 0;
}

/* ---- PCI probe ----------------------------------------------- */

static int virtio_blk_probe(struct pci_dev *dev) {
    if (g_vblk_bound) {
        kprintf("[virtio-blk] already bound -- ignoring %02x:%02x.%x\n",
                dev->bus, dev->slot, dev->fn);
        return -1;
    }

    kprintf("[virtio-blk] probing %02x:%02x.%x  (vid:did %04x:%04x)\n",
            dev->bus, dev->slot, dev->fn, dev->vendor, dev->device);
    pci_dev_enable(dev, PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);

    struct vblk_cap caps[6];
    memset(caps, 0, sizeof(caps));
    uint32_t notify_mult = 0;
    if (!find_virtio_caps(dev, caps, &notify_mult)) {
        kprintf("[virtio-blk] %02x:%02x.%x: no modern caps -- declining\n",
                dev->bus, dev->slot, dev->fn);
        return -2;
    }

    struct vblk_dev *d = &g_vblk;
    memset(d, 0, sizeof(*d));
    d->notify_mult = notify_mult;

    d->common     = map_cap_region(dev, &caps[VIRTIO_PCI_CAP_COMMON_CFG]);
    d->notify_base = map_cap_region(dev, &caps[VIRTIO_PCI_CAP_NOTIFY_CFG]);
    d->device_cfg = map_cap_region(dev, &caps[VIRTIO_PCI_CAP_DEVICE_CFG]);
    if (!d->common || !d->notify_base || !d->device_cfg) {
        kprintf("[virtio-blk] BAR mapping failed\n");
        return -3;
    }

    /* Reset and step through the modern handshake. */
    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS, 0);
    while (cfg_r8(d, VIRTIO_PCI_DEVICE_STATUS) != 0) vblk_pause();
    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS,
           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* Read device features (low 32 + high 32 separately). */
    cfg_w32(d, VIRTIO_PCI_DEVICE_FEATURE_SELECT, 0);
    uint32_t devf_lo = cfg_r32(d, VIRTIO_PCI_DEVICE_FEATURE);
    cfg_w32(d, VIRTIO_PCI_DEVICE_FEATURE_SELECT, 1);
    uint32_t devf_hi = cfg_r32(d, VIRTIO_PCI_DEVICE_FEATURE);

    /* We MUST negotiate VIRTIO_F_VERSION_1; we accept BLK_F_BLK_SIZE
     * if offered (just for diagnostics) and BLK_F_RO so a read-only
     * disk reports as such. Anything else stays unset, which is the
     * safe default for our minimal driver. */
    uint32_t want_lo = 0;
    if (devf_lo & (1u << VIRTIO_BLK_F_BLK_SIZE)) want_lo |= 1u << VIRTIO_BLK_F_BLK_SIZE;
    if (devf_lo & (1u << VIRTIO_BLK_F_RO))       want_lo |= 1u << VIRTIO_BLK_F_RO;
    uint32_t want_hi = 0;
    if (devf_hi & (1u << (VIRTIO_F_VERSION_1 - 32))) {
        want_hi |= 1u << (VIRTIO_F_VERSION_1 - 32);
    } else {
        kprintf("[virtio-blk] device does not offer VERSION_1 -- declining\n");
        cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -4;
    }
    cfg_w32(d, VIRTIO_PCI_DRIVER_FEATURE_SELECT, 0);
    cfg_w32(d, VIRTIO_PCI_DRIVER_FEATURE,        want_lo);
    cfg_w32(d, VIRTIO_PCI_DRIVER_FEATURE_SELECT, 1);
    cfg_w32(d, VIRTIO_PCI_DRIVER_FEATURE,        want_hi);

    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS,
           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
           VIRTIO_STATUS_FEATURES_OK);
    if (!(cfg_r8(d, VIRTIO_PCI_DEVICE_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        kprintf("[virtio-blk] FEATURES_OK rejected by device\n");
        cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -5;
    }

    cfg_w16(d, VIRTIO_PCI_MSIX_CONFIG, VIRTIO_MSI_NO_VECTOR);

    if (!setup_queue(d)) {
        cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -6;
    }

    /* Read device-specific config: capacity (always present) and
     * blk_size (only if F_BLK_SIZE was negotiated). The spec guarantees
     * 8-byte alignment for capacity; on x86 a single 64-bit read works,
     * but split into two 32-bit reads for portability. */
    uint32_t cap_lo = *(volatile uint32_t *)(d->device_cfg + VBLK_CFG_CAPACITY);
    uint32_t cap_hi = *(volatile uint32_t *)(d->device_cfg + VBLK_CFG_CAPACITY + 4);
    d->capacity_lba = ((uint64_t)cap_hi << 32) | cap_lo;

    if (want_lo & (1u << VIRTIO_BLK_F_BLK_SIZE)) {
        d->negotiated_blk_size =
            *(volatile uint32_t *)(d->device_cfg + VBLK_CFG_BLK_SIZE);
    }
    d->read_only = (want_lo & (1u << VIRTIO_BLK_F_RO)) != 0;

    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS,
           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
           VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    /* Register with the blk subsystem. The name pattern matches what
     * other block drivers use ("vblk0", "vblk1", ...). */
    ksnprintf(d->name, sizeof(d->name), "vblk%u", g_vblk_index++);
    d->ops.read    = vblk_read_op;
    d->ops.write   = d->read_only ? NULL : vblk_write_op;
    d->blk.name         = d->name;
    d->blk.ops          = &d->ops;
    d->blk.sector_count = d->capacity_lba;
    d->blk.priv         = d;
    d->blk.class        = BLK_CLASS_DISK;
    blk_register(&d->blk);

    g_vblk_bound = true;

    kprintf("[virtio-blk] %s: capacity=%lu sectors (%lu MiB) "
            "blk_size=%u%s\n",
            d->name,
            (unsigned long)d->capacity_lba,
            (unsigned long)((d->capacity_lba * BLK_SECTOR_SIZE) >> 20),
            (unsigned)(d->negotiated_blk_size ? d->negotiated_blk_size
                                              : BLK_SECTOR_SIZE),
            d->read_only ? " (RO)" : "");
    slog_emit(ABI_SLOG_LEVEL_INFO, SLOG_SUB_DRIVER,
              "virtio-blk %s capacity=%lu blk_size=%u%s",
              d->name, (unsigned long)d->capacity_lba,
              (unsigned)(d->negotiated_blk_size ? d->negotiated_blk_size
                                                : BLK_SECTOR_SIZE),
              d->read_only ? " RO" : "");
    return 0;
}

static const struct pci_match virtio_blk_matches[] = {
    { VIRTIO_VENDOR, VIRTIO_BLK_DEV_LEGACY,
      PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { VIRTIO_VENDOR, VIRTIO_BLK_DEV_MODERN,
      PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    PCI_MATCH_END,
};

static struct pci_driver virtio_blk_driver = {
    .name    = "virtio-blk",
    .matches = virtio_blk_matches,
    .probe   = virtio_blk_probe,
};

void virtio_blk_register(void) {
    pci_register_driver(&virtio_blk_driver);
}

/* Lightweight introspection used by hwinfo / m35b_selftest. NULL if
 * no virtio-blk has been bound. */
const char *virtio_blk_name(void) {
    return g_vblk_bound ? g_vblk.name : NULL;
}
uint64_t virtio_blk_capacity_lba(void) {
    return g_vblk_bound ? g_vblk.capacity_lba : 0;
}
bool virtio_blk_present(void) {
    return g_vblk_bound;
}
