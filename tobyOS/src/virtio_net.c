/* virtio_net.c -- virtio-net-pci 1.0+ (modern transport) NIC driver.
 *
 * Bound through the milestone-21 PCI driver registry. Matches the
 * Red Hat virtio vendor (0x1AF4) at either device 0x1000 (legacy /
 * transitional virtio-net) or 0x1041 (modern non-transitional
 * virtio-net). Either way we drive the MODERN (V1) interface --
 * legacy I/O-port virtio is deliberately not implemented. If a
 * transitional device doesn't expose the modern caps, we decline
 * cleanly.
 *
 * Modern virtio-pci layout
 * ------------------------
 *
 *   The device exposes a chain of vendor-specific PCI capabilities
 *   (cap_id = 0x09). Each one points at a region inside one of the
 *   device's BARs:
 *
 *      cfg_type 1 = COMMON_CFG    -- generic device + per-vq config
 *      cfg_type 2 = NOTIFY_CFG    -- queue notification doorbells
 *      cfg_type 3 = ISR_CFG       -- interrupt status (we poll, so unused)
 *      cfg_type 4 = DEVICE_CFG    -- device-specific config (MAC etc.)
 *      cfg_type 5 = PCI_CFG       -- alternate access via cfg space (unused)
 *
 *   The NOTIFY_CFG cap is bigger than the others: it carries an extra
 *   4-byte notify_off_multiplier at offset 16. Per-queue notification
 *   address = NOTIFY_BASE + queue_notify_off * notify_off_multiplier.
 *
 *   We pci_map_bar() each unique BAR referenced by the caps (NOCACHE),
 *   then save raw pointers to each region.
 *
 * Driver init
 * -----------
 *
 *   1. Walk caps, fill { common, notify (+mult), isr, device } pointers.
 *   2. Reset: write 0 to common.device_status; spin until reads back 0.
 *   3. status |= ACKNOWLEDGE
 *   4. status |= DRIVER
 *   5. Read device_features (low + high 32 bits via _select), AND with
 *      our supported set, write driver_features. We accept exactly two
 *      bits: VIRTIO_F_VERSION_1 (mandatory for modern transport) and
 *      VIRTIO_NET_F_MAC (read MAC from device_cfg). Anything else is
 *      explicitly NOT negotiated -- in particular MRG_RXBUF (which
 *      changes the per-packet header layout) and MQ.
 *   6. status |= FEATURES_OK; re-read; abort if cleared.
 *   7. Build RX (queue 0) + TX (queue 1):
 *        - Pick a queue size <= queue_size_max, power of 2 (we use 32).
 *        - Allocate one page per queue holding desc / avail / used
 *          (packed at fixed offsets 0 / 512 / 1024 -- all three fit
 *          comfortably for QSIZE=32).
 *        - Allocate QSIZE single-page buffers for each queue. desc[i]
 *          is permanently bound to buf[i]; only the per-submit fields
 *          (len, flags) get rewritten on TX.
 *        - Write queue_desc / queue_driver / queue_device, then
 *          queue_enable = 1, and stash the per-queue notification
 *          address.
 *   8. RX queue gets ALL QSIZE descriptors posted to avail before
 *      DRIVER_OK so the device has buffers ready immediately.
 *   9. status |= DRIVER_OK -- the device is now live.
 *  10. net_register() under the name "virtio-net:bb:ss.f".
 *
 * I/O paths
 * ---------
 *
 *   tx(frame, len):
 *     - Reclaim TX descs by walking the used ring up to used.idx.
 *     - Find a free TX desc (we maintain a "next free" cursor + a
 *       per-desc in_use bit; for 1-outstanding-at-a-time use the
 *       cursor alone is enough, but the scaffold is here for future
 *       multi-outstanding).
 *     - memset() the 12-byte virtio_net_hdr to 0; memcpy() the frame
 *       after it. desc.len = 12 + frame_len, desc.flags = 0 (read by
 *       device).
 *     - avail.ring[avail.idx % QSIZE] = desc_idx; avail.idx++.
 *     - On x86, MMIO writes are ordered after prior memory writes,
 *       so no mfence is needed before ringing the notify doorbell.
 *     - Write 16-bit queue index to NOTIFY_BASE + queue_notify_off *
 *       notify_off_multiplier.
 *
 *   rx_drain():
 *     - While used.idx != local_used_idx:
 *         - elem = used.ring[local_used_idx % QSIZE]
 *         - desc_idx = elem.id, len_written = elem.len
 *         - eth_recv(buf[desc_idx] + 12, len_written - 12)
 *         - Re-post desc[desc_idx]: avail.ring[avail.idx % QSIZE] =
 *           desc_idx; avail.idx++
 *         - local_used_idx++
 *     - If we re-posted anything, ring the RX notify doorbell.
 *
 * IRQs are masked at every level (we never set msix_config; we never
 * read ISR). Same model as e1000 / blk_ata / blk_ahci / blk_nvme.
 */

#include <tobyos/net.h>
#include <tobyos/eth.h>
#include <tobyos/pci.h>
#include <tobyos/pmm.h>
#include <tobyos/vmm.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/cpu.h>
#include <tobyos/irq.h>
#include <tobyos/apic.h>

/* ---- PCI vendor / device --------------------------------------- */

#define VIRTIO_VENDOR           0x1AF4
#define VIRTIO_NET_DEV_LEGACY   0x1000   /* transitional + legacy id */
#define VIRTIO_NET_DEV_MODERN   0x1041   /* modern non-transitional */

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

#define VIRTIO_NET_F_MAC          5     /* device MAC is in device_cfg */
#define VIRTIO_F_VERSION_1        32    /* mandatory for modern transport */

/* virtio MSI-X vector sentinel: write this to MSIX_CONFIG /
 * QUEUE_MSIX_VECTOR to opt that source out of MSI-X delivery. */
#define VIRTIO_MSI_NO_VECTOR      0xFFFFu

/* ---- split virtqueue layout ------------------------------------ */

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
    uint32_t id;        /* descriptor index head of used chain */
    uint32_t len;       /* total bytes the device wrote        */
};

/* The avail and used rings have variable-length tails. We treat them
 * as raw byte arrays at fixed offsets within the queue page; the
 * accessor helpers below do the index arithmetic. Layout (modern, no
 * VIRTIO_F_EVENT_IDX negotiated, but we leave room for the optional
 * trailing event field per spec recommendation):
 *
 *   avail @ off 512: u16 flags; u16 idx; u16 ring[QSIZE]; u16 used_event;
 *   used  @ off 1024: u16 flags; u16 idx; struct used_elem ring[QSIZE]; u16 avail_event;
 */

#define VNET_QSIZE          32u
#define VNET_BUF_SIZE       PAGE_SIZE         /* hdr + frame fits in 4 KiB */
#define VNET_HDR_SIZE       12u               /* virtio_net_hdr (V1) */

#define VQ_DESC_OFF         0u
#define VQ_AVAIL_OFF        512u   /* desc table is 32*16 = 512 B */
#define VQ_USED_OFF         1024u  /* avail ring is 4 + 32*2 + 2 = 70 B; pad to 1024 */

/* virtio-net packet header. With VIRTIO_F_VERSION_1 the header is
 * always 12 bytes regardless of MRG_RXBUF (num_buffers is always
 * present). We zero everything for outgoing frames; for incoming we
 * just step past the header. */
struct __attribute__((packed)) virtio_net_hdr {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
};
_Static_assert(sizeof(struct virtio_net_hdr) == 12,
               "virtio_net_hdr (V1) must be 12 bytes");

/* virtio-net device-config layout (only the first 6 bytes -- MAC --
 * are read in this driver). */
#define VNET_CFG_MAC_OFFSET 0

/* ---- driver state ---------------------------------------------- */

struct vnet_queue {
    uint16_t           qid;          /* 0 = RX, 1 = TX */
    uint16_t           qsize;
    uint16_t           avail_idx;    /* SW shadow; matches *avail.idx */
    uint16_t           used_idx;     /* SW shadow; last used we processed */

    uint64_t           ring_phys;    /* phys of the queue page */
    uint8_t           *ring;         /* HHDM virt of the queue page */

    struct virtq_desc *desc;
    volatile uint16_t *avail_flags;
    volatile uint16_t *avail_idx_ptr;
    volatile uint16_t *avail_ring;
    volatile uint16_t *used_flags;
    volatile uint16_t *used_idx_ptr;
    struct virtq_used_elem *used_ring;

    /* Per-descriptor buffer (permanent 1:1 binding). */
    uint64_t           buf_phys[VNET_QSIZE];
    uint8_t           *buf_virt[VNET_QSIZE];

    /* Notification doorbell (in NOTIFY_CFG region). */
    volatile uint16_t *notify;
};

struct vnet_dev {
    /* Pointers into BAR-mapped MMIO. */
    volatile uint8_t  *common;
    volatile uint8_t  *device_cfg;
    volatile uint8_t  *notify_base;
    uint32_t           notify_mult;

    struct vnet_queue  rx;
    struct vnet_queue  tx;

    /* MSI-X bring-up state. irq_enabled is set if pci_msix_enable +
     * irq_alloc_vector both succeeded AND the device actually accepted
     * MSI-X for the RX queue (some virtio backends -- notably old
     * legacy QEMU -- silently drop the QUEUE_MSIX_VECTOR write). */
    uint8_t            irq_vector;
    bool               irq_enabled;
    volatile uint64_t  irq_count;

    char               name[32];
    struct net_dev     net;
};

/* Single-NIC scope. The first probe wins; subsequent virtio-net
 * devices on the same machine are accepted via PCI but politely
 * declined here (mirrors what e1000.c does). */
static struct vnet_dev g_vnet;
static bool            g_vnet_bound;

/* ---- MMIO helpers --------------------------------------------- */

static inline uint8_t  cfg_r8 (struct vnet_dev *d, uint32_t off) {
    return *(volatile uint8_t  *)(d->common + off);
}
static inline uint16_t cfg_r16(struct vnet_dev *d, uint32_t off) {
    return *(volatile uint16_t *)(d->common + off);
}
static inline uint32_t cfg_r32(struct vnet_dev *d, uint32_t off) {
    return *(volatile uint32_t *)(d->common + off);
}
static inline void cfg_w8 (struct vnet_dev *d, uint32_t off, uint8_t  v) {
    *(volatile uint8_t  *)(d->common + off) = v;
}
static inline void cfg_w16(struct vnet_dev *d, uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(d->common + off) = v;
}
static inline void cfg_w32(struct vnet_dev *d, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(d->common + off) = v;
}
/* common cfg has two 64-bit registers (queue_desc, queue_driver,
 * queue_device). Use two 32-bit writes -- safer across hosts. */
static inline void cfg_w64(struct vnet_dev *d, uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(d->common + off)     = (uint32_t)(v & 0xFFFFFFFFu);
    *(volatile uint32_t *)(d->common + off + 4) = (uint32_t)(v >> 32);
}

/* ---- queue setup ---------------------------------------------- */

/* Carve the queue page into desc / avail / used at our chosen fixed
 * offsets, and stash convenience pointers. Caller must have already
 * pmm_alloc_page'd ring_phys + filled ring_virt. */
static void queue_layout(struct vnet_queue *q) {
    q->desc = (struct virtq_desc *)(q->ring + VQ_DESC_OFF);

    uint8_t *avail = q->ring + VQ_AVAIL_OFF;
    q->avail_flags   = (volatile uint16_t *)(avail + 0);
    q->avail_idx_ptr = (volatile uint16_t *)(avail + 2);
    q->avail_ring    = (volatile uint16_t *)(avail + 4);

    uint8_t *used = q->ring + VQ_USED_OFF;
    q->used_flags   = (volatile uint16_t *)(used + 0);
    q->used_idx_ptr = (volatile uint16_t *)(used + 2);
    q->used_ring    = (struct virtq_used_elem *)(used + 4);
}

/* Bring up one virtqueue: allocate the ring page + QSIZE buffers,
 * bind desc[i] to buf[i] permanently, fill in the per-queue MMIO
 * registers, latch the notification doorbell address, enable. The
 * caller is responsible for posting RX descriptors to avail (we don't
 * do that here so init order can stay obvious). */
static void vnet_rx_drain_op(struct net_dev *dev);   /* fwd decl */

static bool vnet_setup_queue(struct vnet_dev *d, struct vnet_queue *q,
                             uint16_t qid, uint16_t msix_vec) {
    q->qid       = qid;
    q->qsize     = VNET_QSIZE;
    q->avail_idx = 0;
    q->used_idx  = 0;

    /* Tell the device which queue we're configuring + ask it for its
     * max queue size. We refuse anything below VNET_QSIZE -- in
     * practice every vhost backend supports at least 256. */
    cfg_w16(d, VIRTIO_PCI_QUEUE_SELECT, qid);
    uint16_t max_qs = cfg_r16(d, VIRTIO_PCI_QUEUE_SIZE);
    if (max_qs < VNET_QSIZE) {
        kprintf("[virtio-net] queue %u max_size=%u < %u (unsupported)\n",
                qid, max_qs, VNET_QSIZE);
        return false;
    }
    cfg_w16(d, VIRTIO_PCI_QUEUE_SIZE, VNET_QSIZE);

    q->ring_phys = pmm_alloc_page();
    if (!q->ring_phys) {
        kprintf("[virtio-net] OOM allocating queue %u ring\n", qid);
        return false;
    }
    q->ring = (uint8_t *)pmm_phys_to_virt(q->ring_phys);
    memset(q->ring, 0, PAGE_SIZE);
    queue_layout(q);

    for (uint16_t i = 0; i < q->qsize; i++) {
        uint64_t bp = pmm_alloc_page();
        if (!bp) {
            kprintf("[virtio-net] OOM allocating queue %u buffer %u\n", qid, i);
            return false;
        }
        q->buf_phys[i] = bp;
        q->buf_virt[i] = (uint8_t *)pmm_phys_to_virt(bp);
        memset(q->buf_virt[i], 0, PAGE_SIZE);

        /* Permanent 1:1 desc <-> buffer binding. RX descs are
         * device-writable; TX descs are device-readable. */
        q->desc[i].addr  = bp;
        q->desc[i].len   = VNET_BUF_SIZE;
        q->desc[i].flags = (qid == 0) ? VQ_DESC_F_WRITE : 0;
        q->desc[i].next  = 0;
    }

    cfg_w64(d, VIRTIO_PCI_QUEUE_DESC,   q->ring_phys + VQ_DESC_OFF);
    cfg_w64(d, VIRTIO_PCI_QUEUE_DRIVER, q->ring_phys + VQ_AVAIL_OFF);
    cfg_w64(d, VIRTIO_PCI_QUEUE_DEVICE, q->ring_phys + VQ_USED_OFF);

    /* Latch the per-queue notification doorbell address NOW, while
     * QUEUE_SELECT still points at us. (Re-selecting later would work
     * too, but doing it once at init keeps the hot path branchless.) */
    uint16_t qoff = cfg_r16(d, VIRTIO_PCI_QUEUE_NOTIFY_OFF);
    q->notify = (volatile uint16_t *)
                (d->notify_base + (uint32_t)qoff * d->notify_mult);

    /* Bind this queue to an MSI-X table entry (or VIRTIO_MSI_NO_VECTOR
     * to leave it polled / INTx). Per the virtio spec the host MAY
     * silently reject the bind by returning VIRTIO_MSI_NO_VECTOR on
     * read-back, in which case we fall back to polling for this queue. */
    cfg_w16(d, VIRTIO_PCI_QUEUE_MSIX_VECTOR, msix_vec);
    if (msix_vec != VIRTIO_MSI_NO_VECTOR) {
        uint16_t got = cfg_r16(d, VIRTIO_PCI_QUEUE_MSIX_VECTOR);
        if (got != msix_vec) {
            kprintf("[virtio-net] queue %u: MSI-X bind rejected "
                    "(asked %u, got 0x%04x) -- queue stays polled\n",
                    qid, msix_vec, got);
        }
    }

    cfg_w16(d, VIRTIO_PCI_QUEUE_ENABLE, 1);
    return true;
}

/* MSI handler: drains any RX packets that landed since the last poll
 * and, if anything was reposted, the drain function rings the device
 * doorbell to invite more frames. apic_eoi is sent by the dyn-vector
 * trampoline after we return. */
static void vnet_irq_handler(void *ctx) {
    struct vnet_dev *d = (struct vnet_dev *)ctx;
    if (!d) return;
    d->irq_count++;
    vnet_rx_drain_op(&d->net);
}

/* Post every RX descriptor to the avail ring up-front, then ring the
 * doorbell once. After this the device has VNET_QSIZE buffers ready
 * to receive into. */
static void vnet_rx_post_all(struct vnet_dev *d) {
    struct vnet_queue *q = &d->rx;
    for (uint16_t i = 0; i < q->qsize; i++) {
        q->avail_ring[q->avail_idx % q->qsize] = i;
        q->avail_idx++;
    }
    *q->avail_idx_ptr = q->avail_idx;
    *q->notify = q->qid;
}

/* ---- TX / RX (driver-side, called via the net_dev vtable) ----- */

static bool vnet_tx_op(struct net_dev *dev, const void *frame, size_t len) {
    (void)dev;
    if (len == 0 || len > VNET_BUF_SIZE - VNET_HDR_SIZE) return false;

    struct vnet_dev   *d = &g_vnet;
    struct vnet_queue *q = &d->tx;

    /* Reclaim any completed TX descriptors so we know which slots are
     * free. With 1 outstanding at a time we always have headroom; the
     * loop is here so future multi-outstanding usage Just Works. */
    while (q->used_idx != *q->used_idx_ptr) {
        q->used_idx++;
    }

    /* Pick the next slot. avail_idx mod qsize gives the descriptor
     * index because we use the same slot ordering as desc indices. */
    uint16_t slot = q->avail_idx % q->qsize;

    /* If we have qsize outstanding submissions without a corresponding
     * used update we'd be about to overwrite an in-flight buffer.
     * For our 1-at-a-time use this can never trigger; defensive. */
    if ((uint16_t)(q->avail_idx - q->used_idx) >= q->qsize) {
        kprintf("[virtio-net] tx ring full (avail=%u used=%u)\n",
                q->avail_idx, q->used_idx);
        return false;
    }

    /* Compose the buffer in-place: 12 zero bytes of header, then the
     * caller's frame. desc[slot].addr was pinned at init -- we only
     * rewrite len + flags here. */
    uint8_t *buf = q->buf_virt[slot];
    memset(buf, 0, VNET_HDR_SIZE);
    memcpy(buf + VNET_HDR_SIZE, frame, len);

    q->desc[slot].len   = (uint32_t)(VNET_HDR_SIZE + len);
    q->desc[slot].flags = 0;            /* device-readable, last in chain */
    q->desc[slot].next  = 0;

    q->avail_ring[q->avail_idx % q->qsize] = slot;
    q->avail_idx++;
    *q->avail_idx_ptr = q->avail_idx;

    /* MMIO writes on x86 are ordered after prior memory stores (UC
     * stores are fully ordered), so no explicit mfence is required
     * before the doorbell. */
    *q->notify = q->qid;
    return true;
}

static void vnet_rx_drain_op(struct net_dev *dev) {
    (void)dev;
    struct vnet_dev   *d = &g_vnet;
    struct vnet_queue *q = &d->rx;

    bool reposted = false;
    while (q->used_idx != *q->used_idx_ptr) {
        struct virtq_used_elem elem = q->used_ring[q->used_idx % q->qsize];
        uint16_t slot = (uint16_t)(elem.id & 0xFFFFu);

        if (slot < q->qsize && elem.len >= VNET_HDR_SIZE) {
            uint32_t frame_len = elem.len - VNET_HDR_SIZE;
            if (frame_len > 0 && frame_len <= VNET_BUF_SIZE - VNET_HDR_SIZE) {
                eth_recv(q->buf_virt[slot] + VNET_HDR_SIZE, frame_len);
            }
        }

        /* Re-post the descriptor. addr / len / flags were pinned at
         * init and are still correct -- the device writes only the
         * data, not the descriptor entry itself. */
        q->avail_ring[q->avail_idx % q->qsize] = slot;
        q->avail_idx++;
        q->used_idx++;
        reposted = true;
    }

    if (reposted) {
        *q->avail_idx_ptr = q->avail_idx;
        *q->notify = q->qid;
    }
}

/* ---- capability walk ------------------------------------------ */

/* Scan the PCI capability list and remember the BAR + offset of each
 * virtio cfg_type we care about. Returns true iff we found at least
 * COMMON_CFG, NOTIFY_CFG, and DEVICE_CFG -- the minimum needed to
 * drive a modern virtio device. */
struct vnet_cap {
    bool     present;
    uint8_t  bar;
    uint32_t offset;
    uint32_t length;
};

static bool find_virtio_caps(struct pci_dev *dev,
                             struct vnet_cap caps[6],
                             uint32_t *out_notify_mult) {
    bool got_common = false, got_notify = false, got_device = false;
    *out_notify_mult = 0;

    for (uint8_t off = pci_cap_first(dev); off; off = pci_cap_next(dev, off)) {
        uint8_t id = pci_cfg_read8(dev->bus, dev->slot, dev->fn, off);
        if (id != PCI_CAP_ID_VENDOR) continue;

        /* virtio cap layout starts at the cap header:
         *   off+0  cap_vndr  (= 0x09)
         *   off+1  cap_next
         *   off+2  cap_len
         *   off+3  cfg_type
         *   off+4  bar
         *   off+5..7  padding
         *   off+8  offset (uint32)
         *   off+12 length (uint32)
         *   off+16 notify_off_multiplier (uint32, NOTIFY_CFG only)
         */
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

static int virtio_net_probe(struct pci_dev *dev) {
    if (g_vnet_bound) {
        kprintf("[virtio-net] already bound -- ignoring %02x:%02x.%x\n",
                dev->bus, dev->slot, dev->fn);
        return -1;
    }

    kprintf("[virtio-net] probing %02x:%02x.%x  (vid:did %04x:%04x)\n",
            dev->bus, dev->slot, dev->fn, dev->vendor, dev->device);

    pci_dev_enable(dev, PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);

    /* 1. Find the modern caps. A transitional device exposes BOTH
     * legacy I/O and modern caps; if there's no modern COMMON_CFG we
     * decline (we don't speak legacy I/O virtio). */
    struct vnet_cap caps[6];
    memset(caps, 0, sizeof(caps));
    uint32_t notify_mult = 0;
    if (!find_virtio_caps(dev, caps, &notify_mult)) {
        kprintf("[virtio-net] %02x:%02x.%x: no modern virtio caps "
                "(legacy-only device?) -- declining\n",
                dev->bus, dev->slot, dev->fn);
        return -2;
    }

    /* 2. Map ONLY the BARs referenced by the caps we actually drive:
     * COMMON_CFG, NOTIFY_CFG, DEVICE_CFG. We deliberately do NOT
     * touch ISR_CFG (we poll) or PCI_CFG (the cap-window alternate-
     * access mechanism, whose `bar` field can legally point at an
     * unallocated BAR -- e.g. QEMU's modern virtio with
     * disable-legacy=on routes PCI_CFG through BAR0 even though
     * BAR0 is empty in that mode).
     *
     * Multiple caps may share a single BAR -- pci_map_bar caches
     * its mapping per (dev, idx), so calling it twice is harmless. */
    struct vnet_dev *d = &g_vnet;
    memset(d, 0, sizeof(*d));
    d->notify_mult = notify_mult;

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
            kprintf("[virtio-net] cfg_type %d: bogus BAR index %u\n", t, bi);
            return -3;
        }
        if (!bars[bi]) {
            bars[bi] = pci_map_bar(dev, bi, 0);
            if (!bars[bi]) {
                kprintf("[virtio-net] BAR%u map failed (phys=%p)\n",
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

    kprintf("[virtio-net] common=%p notify=%p (mult=%u) device=%p\n",
            (void *)d->common, (void *)d->notify_base,
            d->notify_mult, (void *)d->device_cfg);

    /* 3. Reset, ACK, DRIVER. */
    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS, 0);
    for (int i = 0; i < 100000; i++) {
        if (cfg_r8(d, VIRTIO_PCI_DEVICE_STATUS) == 0) break;
    }
    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS,
           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    /* 4. Negotiate features. We accept VIRTIO_F_VERSION_1 (mandatory
     * for modern transport) + VIRTIO_NET_F_MAC. Everything else is
     * left off so QEMU exposes the simplest possible header layout. */
    cfg_w32(d, VIRTIO_PCI_DEVICE_FEATURE_SELECT, 0);
    uint32_t devf_lo = cfg_r32(d, VIRTIO_PCI_DEVICE_FEATURE);
    cfg_w32(d, VIRTIO_PCI_DEVICE_FEATURE_SELECT, 1);
    uint32_t devf_hi = cfg_r32(d, VIRTIO_PCI_DEVICE_FEATURE);

    uint32_t want_lo = 0;
    uint32_t want_hi = 0;
    if (devf_lo & (1u << VIRTIO_NET_F_MAC))           want_lo |= (1u << VIRTIO_NET_F_MAC);
    if (devf_hi & (1u << (VIRTIO_F_VERSION_1 - 32))) {
        want_hi |= (1u << (VIRTIO_F_VERSION_1 - 32));
    } else {
        kprintf("[virtio-net] device does not advertise VIRTIO_F_VERSION_1 "
                "(modern transport mandatory) -- aborting\n");
        cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -5;
    }

    cfg_w32(d, VIRTIO_PCI_DRIVER_FEATURE_SELECT, 0);
    cfg_w32(d, VIRTIO_PCI_DRIVER_FEATURE,        want_lo);
    cfg_w32(d, VIRTIO_PCI_DRIVER_FEATURE_SELECT, 1);
    cfg_w32(d, VIRTIO_PCI_DRIVER_FEATURE,        want_hi);

    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS,
           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
           VIRTIO_STATUS_FEATURES_OK);
    if (!(cfg_r8(d, VIRTIO_PCI_DEVICE_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        kprintf("[virtio-net] device cleared FEATURES_OK -- driver "
                "feature subset rejected\n");
        cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS, VIRTIO_STATUS_FAILED);
        return -6;
    }

    kprintf("[virtio-net] features: device=0x%08x_%08x  "
            "driver=0x%08x_%08x\n", devf_hi, devf_lo, want_hi, want_lo);

    /* 5. Read MAC. With VIRTIO_NET_F_MAC the device_cfg's first 6
     * bytes hold the MAC (network byte order). If the feature wasn't
     * available we fall back to a fabricated 02:xx:xx... locally-
     * administered address. QEMU always offers the MAC feature. */
    if (want_lo & (1u << VIRTIO_NET_F_MAC)) {
        for (int i = 0; i < ETH_ADDR_LEN; i++) {
            d->net.mac[i] = *(volatile uint8_t *)(d->device_cfg + VNET_CFG_MAC_OFFSET + i);
        }
    } else {
        d->net.mac[0] = 0x02;        /* locally-administered */
        d->net.mac[1] = 0x54;
        d->net.mac[2] = 0x00;
        d->net.mac[3] = 0xCA;
        d->net.mac[4] = 0xFE;
        d->net.mac[5] = 0x01;
        kprintf("[virtio-net] MAC feature not negotiated -- using "
                "fabricated %02x:%02x:%02x:%02x:%02x:%02x\n",
                d->net.mac[0], d->net.mac[1], d->net.mac[2],
                d->net.mac[3], d->net.mac[4], d->net.mac[5]);
    }

    /* 6. MSI-X bring-up. We ask for ONE vector and route it at RX vq;
     * TX is reclaimed lazily inside vnet_tx_op so it doesn't need an
     * IRQ. Config-change events are also routed nowhere -- we don't
     * react to them. The driver_data passes our vnet_dev pointer to
     * the trampoline so the handler can find both rx queue + net_dev.
     *
     * On failure we leave irq_enabled=false; QUEUE_MSIX_VECTOR for the
     * RX queue stays at VIRTIO_MSI_NO_VECTOR (the device's default
     * after FEATURES_OK), so the device falls back to legacy INTx --
     * which is also silent because we never wire INTx. The RX side
     * therefore remains driven by the existing net_poll() in the
     * idle loop, exactly as in M21. */
    uint16_t rx_vec = VIRTIO_MSI_NO_VECTOR;
    uint8_t  vec    = irq_alloc_vector(vnet_irq_handler, d);
    if (vec == 0) {
        kprintf("[virtio-net] no IDT vectors free -- staying polled\n");
    } else if (!pci_msix_enable(dev, vec, (uint8_t)apic_read_id(), 1u)) {
        kprintf("[virtio-net] no MSI-X cap -- staying polled "
                "(vec 0x%02x is now idle)\n", (unsigned)vec);
    } else {
        d->irq_vector  = vec;
        d->irq_enabled = true;
        rx_vec         = 0;
        /* Tell the device that config-change events are NOT routed via
         * MSI-X (we don't handle them). Per spec this must be set before
         * each queue's vector or the device may reject the bind. */
        cfg_w16(d, VIRTIO_PCI_MSIX_CONFIG, VIRTIO_MSI_NO_VECTOR);
    }

    /* 7. Two virtqueues: 0 = RX (gets the MSI-X vector if available),
     * 1 = TX (always polled-on-submit, no MSI-X bind). */
    if (!vnet_setup_queue(d, &d->rx, 0, rx_vec))                return -7;
    if (!vnet_setup_queue(d, &d->tx, 1, VIRTIO_MSI_NO_VECTOR))  return -8;

    if (d->irq_enabled) {
        kprintf("[virtio-net] IRQ live on vec 0x%02x  RX=msix0  TX=polled\n",
                (unsigned)d->irq_vector);
    }

    /* 8. Hand RX buffers to the device, then declare the driver live. */
    vnet_rx_post_all(d);
    cfg_w8(d, VIRTIO_PCI_DEVICE_STATUS,
           VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
           VIRTIO_STATUS_FEATURES_OK | VIRTIO_STATUS_DRIVER_OK);

    /* 9. Build the registry name "virtio-net:bb:ss.f" and register. */
    static const char hex[] = "0123456789abcdef";
    char *n = d->name;
    const char *prefix = "virtio-net:";
    while (*prefix) *n++ = *prefix++;
    *n++ = hex[(dev->bus  >> 4) & 0xF]; *n++ = hex[dev->bus  & 0xF]; *n++ = ':';
    *n++ = hex[(dev->slot >> 4) & 0xF]; *n++ = hex[dev->slot & 0xF]; *n++ = '.';
    *n++ = hex[dev->fn & 0xF];
    *n   = '\0';

    d->net.name     = d->name;
    d->net.priv     = d;
    d->net.tx       = vnet_tx_op;
    d->net.rx_drain = vnet_rx_drain_op;
    net_register(&d->net);

    g_vnet_bound      = true;
    dev->driver_data  = d;
    return 0;
}

static const struct pci_match g_vnet_matches[] = {
    /* Red Hat virtio vendor. We accept both the legacy/transitional
     * device id 0x1000 (the QEMU default) and the modern non-
     * transitional id 0x1041. The probe walks the cap list and
     * declines if no modern caps are present. */
    { VIRTIO_VENDOR, VIRTIO_NET_DEV_LEGACY,
      PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { VIRTIO_VENDOR, VIRTIO_NET_DEV_MODERN,
      PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    PCI_MATCH_END,
};

static struct pci_driver g_vnet_driver = {
    .name    = "virtio-net",
    .matches = g_vnet_matches,
    .probe   = virtio_net_probe,
    .remove  = 0,
};

void virtio_net_register(void) {
    pci_register_driver(&g_vnet_driver);
}
