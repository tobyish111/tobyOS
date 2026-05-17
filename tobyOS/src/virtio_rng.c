/* virtio_rng.c -- modern virtio-rng entropy source.
 *
 * This is intentionally tiny: one virtqueue, one writable DMA buffer,
 * polled completion during PCI probe. Its only job is to mix host-provided
 * random bytes into rng.c early enough for SSH host-key generation.
 */

#include <tobyos/rng.h>
#include <tobyos/pci.h>
#include <tobyos/pmm.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

#define VIRTIO_VENDOR              0x1AF4
#define VIRTIO_RNG_DEV_LEGACY      0x1005
#define VIRTIO_RNG_DEV_MODERN      0x1044

#define VIRTIO_PCI_CAP_COMMON_CFG  1
#define VIRTIO_PCI_CAP_NOTIFY_CFG  2
#define VIRTIO_PCI_CAP_DEVICE_CFG  4

#define VIRTIO_PCI_DEVICE_FEATURE_SELECT  0x00
#define VIRTIO_PCI_DEVICE_FEATURE         0x04
#define VIRTIO_PCI_DRIVER_FEATURE_SELECT  0x08
#define VIRTIO_PCI_DRIVER_FEATURE         0x0C
#define VIRTIO_PCI_MSIX_CONFIG            0x10
#define VIRTIO_PCI_DEVICE_STATUS          0x14
#define VIRTIO_PCI_QUEUE_SELECT           0x16
#define VIRTIO_PCI_QUEUE_SIZE             0x18
#define VIRTIO_PCI_QUEUE_MSIX_VECTOR      0x1A
#define VIRTIO_PCI_QUEUE_ENABLE           0x1C
#define VIRTIO_PCI_QUEUE_NOTIFY_OFF       0x1E
#define VIRTIO_PCI_QUEUE_DESC             0x20
#define VIRTIO_PCI_QUEUE_DRIVER           0x28
#define VIRTIO_PCI_QUEUE_DEVICE           0x30

#define VIRTIO_STATUS_ACKNOWLEDGE   1
#define VIRTIO_STATUS_DRIVER        2
#define VIRTIO_STATUS_DRIVER_OK     4
#define VIRTIO_STATUS_FEATURES_OK   8
#define VIRTIO_STATUS_FAILED      128

#define VIRTIO_F_VERSION_1          32
#define VIRTIO_MSI_NO_VECTOR    0xFFFFu

#define VQ_DESC_F_WRITE  2

#define VRNG_QSIZE       8u
#define VRNG_SEED_BYTES  64u

#define VQ_DESC_OFF   0u
#define VQ_AVAIL_OFF  256u
#define VQ_USED_OFF   1024u

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

struct vrng_cap {
    bool     present;
    uint8_t  bar;
    uint32_t offset;
    uint32_t length;
};

struct vrng_dev {
    volatile uint8_t  *common;
    volatile uint8_t  *notify_base;
    uint32_t           notify_mult;

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

    uint64_t           seed_phys;
    uint8_t           *seed;
};

static struct vrng_dev g_vrng;
static bool            g_vrng_bound;

static inline void vrng_pause(void) {
    __asm__ volatile ("pause" ::: "memory");
}

static inline uint16_t cfg_r16(struct vrng_dev *d, uint32_t off) {
    return *(volatile uint16_t *)(d->common + off);
}
static inline uint32_t cfg_r32(struct vrng_dev *d, uint32_t off) {
    return *(volatile uint32_t *)(d->common + off);
}
static inline uint8_t cfg_r8(struct vrng_dev *d, uint32_t off) {
    return *(volatile uint8_t *)(d->common + off);
}
static inline void cfg_w8(struct vrng_dev *d, uint32_t off, uint8_t v) {
    *(volatile uint8_t *)(d->common + off) = v;
}
static inline void cfg_w16(struct vrng_dev *d, uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(d->common + off) = v;
}
static inline void cfg_w32(struct vrng_dev *d, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(d->common + off) = v;
}
static inline void cfg_w64(struct vrng_dev *d, uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(d->common + off)     = (uint32_t)v;
    *(volatile uint32_t *)(d->common + off + 4) = (uint32_t)(v >> 32);
}

static bool find_virtio_caps(struct pci_dev *dev,
                             struct vrng_cap caps[6],
                             uint32_t *out_notify_mult) {
    bool got_common = false, got_notify = false;
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
        if (cfg_type == VIRTIO_PCI_CAP_NOTIFY_CFG) {
            got_notify = true;
            *out_notify_mult =
                pci_cfg_read32(dev->bus, dev->slot, dev->fn, off + 16);
        }
    }
    return got_common && got_notify;
}

static bool map_needed_caps(struct pci_dev *dev, struct vrng_cap caps[6],
                            void *bars[PCI_BAR_COUNT]) {
    static const int needed[] = {
        VIRTIO_PCI_CAP_COMMON_CFG,
        VIRTIO_PCI_CAP_NOTIFY_CFG,
    };
    for (size_t k = 0; k < sizeof(needed) / sizeof(needed[0]); k++) {
        int t = needed[k];
        uint8_t bi = caps[t].bar;
        if (bi >= PCI_BAR_COUNT || dev->bar_is_io[bi]) return false;
        if (!bars[bi]) {
            bars[bi] = pci_map_bar(dev, bi, 0);
            if (!bars[bi]) return false;
        }
    }
    return true;
}

static bool setup_queue(struct vrng_dev *d) {
    cfg_w16(d, VIRTIO_PCI_QUEUE_SELECT, 0);
    uint16_t max_qs = cfg_r16(d, VIRTIO_PCI_QUEUE_SIZE);
    if (max_qs < 1u) return false;
    d->qsize = max_qs < VRNG_QSIZE ? max_qs : VRNG_QSIZE;
    d->avail_idx = 0;
    d->used_idx = 0;
    cfg_w16(d, VIRTIO_PCI_QUEUE_SIZE, d->qsize);

    d->ring_phys = pmm_alloc_page();
    d->seed_phys = pmm_alloc_page();
    if (!d->ring_phys || !d->seed_phys) return false;

    d->ring = (uint8_t *)pmm_phys_to_virt(d->ring_phys);
    d->seed = (uint8_t *)pmm_phys_to_virt(d->seed_phys);
    memset(d->ring, 0, PAGE_SIZE);
    memset(d->seed, 0, PAGE_SIZE);

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

    cfg_w16(d, VIRTIO_PCI_MSIX_CONFIG, VIRTIO_MSI_NO_VECTOR);
    cfg_w16(d, VIRTIO_PCI_QUEUE_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);
    cfg_w16(d, VIRTIO_PCI_QUEUE_ENABLE, 1);
    return true;
}

static bool read_seed(struct vrng_dev *d) {
    d->desc[0].addr  = d->seed_phys;
    d->desc[0].len   = VRNG_SEED_BYTES;
    d->desc[0].flags = VQ_DESC_F_WRITE;
    d->desc[0].next  = 0;

    d->avail_ring[d->avail_idx % d->qsize] = 0;
    d->avail_idx++;
    *d->avail_idx_ptr = d->avail_idx;
    *d->notify = 0;

    for (uint32_t i = 0; i < 1000000u; i++) {
        if (d->used_idx != *d->used_idx_ptr) {
            struct virtq_used_elem elem =
                d->used_ring[d->used_idx % d->qsize];
            d->used_idx++;
            if (elem.len == 0) return false;
            size_t n = elem.len < VRNG_SEED_BYTES ? elem.len : VRNG_SEED_BYTES;
            rng_mix_hardware(d->seed, n, "virtio-rng");
            return true;
        }
        vrng_pause();
    }
    return false;
}

static int virtio_rng_probe(struct pci_dev *dev) {
    if (g_vrng_bound) return -1;

    kprintf("[virtio-rng] probing %02x:%02x.%x (vid:did %04x:%04x)\n",
            dev->bus, dev->slot, dev->fn, dev->vendor, dev->device);

    pci_dev_enable(dev, PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);

    struct vrng_cap caps[6];
    memset(caps, 0, sizeof(caps));
    uint32_t notify_mult = 0;
    if (!find_virtio_caps(dev, caps, &notify_mult)) {
        kprintf("[virtio-rng] no modern virtio caps -- declining\n");
        return -2;
    }

    void *bars[PCI_BAR_COUNT] = {0};
    if (!map_needed_caps(dev, caps, bars)) {
        kprintf("[virtio-rng] BAR map failed -- declining\n");
        return -3;
    }

    struct vrng_dev *d = &g_vrng;
    memset(d, 0, sizeof(*d));
    d->notify_mult = notify_mult;
    d->common = (volatile uint8_t *)bars[caps[VIRTIO_PCI_CAP_COMMON_CFG].bar]
              + caps[VIRTIO_PCI_CAP_COMMON_CFG].offset;
    d->notify_base = (volatile uint8_t *)bars[caps[VIRTIO_PCI_CAP_NOTIFY_CFG].bar]
                   + caps[VIRTIO_PCI_CAP_NOTIFY_CFG].offset;

    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS, 0);
    for (int i = 0; i < 100000; i++) {
        if (cfg_r8(d, VIRTIO_PCI_DEVICE_STATUS) == 0) break;
    }
    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS,
           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    cfg_w32(d, VIRTIO_PCI_DEVICE_FEATURE_SELECT, 1);
    uint32_t devf_hi = cfg_r32(d, VIRTIO_PCI_DEVICE_FEATURE);
    uint32_t want_hi = 0;
    if (devf_hi & (1u << (VIRTIO_F_VERSION_1 - 32))) {
        want_hi = 1u << (VIRTIO_F_VERSION_1 - 32);
    } else {
        kprintf("[virtio-rng] VIRTIO_F_VERSION_1 missing -- aborting\n");
        cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -4;
    }

    cfg_w32(d, VIRTIO_PCI_DRIVER_FEATURE_SELECT, 0);
    cfg_w32(d, VIRTIO_PCI_DRIVER_FEATURE, 0);
    cfg_w32(d, VIRTIO_PCI_DRIVER_FEATURE_SELECT, 1);
    cfg_w32(d, VIRTIO_PCI_DRIVER_FEATURE, want_hi);

    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS,
           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
           VIRTIO_STATUS_FEATURES_OK);
    if (!(cfg_r8(d, VIRTIO_PCI_DEVICE_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        kprintf("[virtio-rng] FEATURES_OK rejected\n");
        cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -5;
    }

    if (!setup_queue(d)) {
        kprintf("[virtio-rng] queue setup failed\n");
        cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -6;
    }

    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS,
           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
           VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    if (!read_seed(d)) {
        kprintf("[virtio-rng] seed request timed out or returned no data\n");
        cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -7;
    }

    g_vrng_bound = true;
    dev->driver_data = d;
    kprintf("[virtio-rng] mixed initial entropy\n");
    return 0;
}

static const struct pci_match g_vrng_matches[] = {
    { VIRTIO_VENDOR, VIRTIO_RNG_DEV_LEGACY,
      PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { VIRTIO_VENDOR, VIRTIO_RNG_DEV_MODERN,
      PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    PCI_MATCH_END,
};

static struct pci_driver g_vrng_driver = {
    .name    = "virtio-rng",
    .matches = g_vrng_matches,
    .probe   = virtio_rng_probe,
};

void virtio_rng_register(void) {
    pci_register_driver(&g_vrng_driver);
}
