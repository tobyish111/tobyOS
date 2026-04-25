/* rtl8169.c -- Realtek RTL8169/8168/8111 PCIe gigabit NIC driver.
 *
 * Bound through the milestone-21 PCI driver registry. Polled, no
 * IRQs, descriptor-ring DMA -- same architecture as e1000 / e1000e
 * but a completely different register layout because Realtek and
 * Intel agree on essentially nothing.
 *
 * QEMU does NOT emulate this chip family (it only emulates the
 * earlier RTL8139 100M part via `-device rtl8139`, which advertises
 * vendor 0x10EC device 0x8139 -- our match table excludes 0x8139
 * deliberately so the bus loop ignores 8139s rather than binding our
 * gigabit driver to a fast-ethernet chip). The driver is therefore
 * code-review-and-datasheet-validated; functional verification has
 * to happen on real silicon. We follow the Linux r8169 driver's
 * conservative init recipe (mostly the bits that work across every
 * 8168 revision A through H) so the failure mode on an exotic chip
 * is "link comes up but TX/RX silently underperform" rather than a
 * controller hang.
 *
 * What we do NOT support, deliberately:
 *   - chip-revision-specific magic-poke tables (Linux's
 *     rtl_init_rxcfg / rtl_hw_start_8168xx mountain). Most consumer
 *     boards from ~2007 onward boot fine without these tweaks; the
 *     packets just won't be coalesced as aggressively.
 *   - PHY firmware uploads (needed for EEE / Master-1G negotiation
 *     on some 8168E+ revs; auto-neg drops to 100M without them on a
 *     fresh boot, which is good enough to get an IP).
 *   - Hardware checksum / TSO / LRO offload (CPCR bits cleared).
 *   - 8125 (2.5 GbE) and 8136 (100M-only RTL8101) -- different MAC
 *     layers, separate drivers.
 *
 * Memory layout: same as e1000 -- one page per descriptor ring (we
 * use 32 entries × 16 bytes = 512 B, fits trivially in a page) and
 * one page per per-descriptor RX/TX buffer.
 */

#include <tobyos/net.h>
#include <tobyos/pci.h>
#include <tobyos/pmm.h>
#include <tobyos/eth.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/cpu.h>
#include <tobyos/irq.h>
#include <tobyos/apic.h>

/* ----- register offsets (relative to MMIO BAR base) -------------- */
/* All offsets are from the RTL8168 datasheet rev 1.5 § 9. The
 * RTL8169 datasheet uses the same map; only the BAR location moves
 * (BAR1 on the original PCI 8169, BAR2 on every PCIe variant). */

#define RTL_IDR0          0x00       /* MAC bytes 0..3 (32-bit RO after reset) */
#define RTL_IDR4          0x04       /* MAC bytes 4..5 (16-bit RO after reset) */
#define RTL_MAR0          0x08       /* multicast filter, 8 bytes total */
#define RTL_TNPDS         0x20       /* TX normal-prio desc base (64-bit) */
#define RTL_THPDS         0x28       /* TX high-prio desc base (unused) */
#define RTL_CR            0x37       /* command (8-bit): RST/RX_EN/TX_EN */
#define RTL_TPPOLL        0x38       /* TX poll (8-bit): NPQ bit kicks NIC */
#define RTL_IMR           0x3C       /* interrupt mask (16-bit) */
#define RTL_ISR           0x3E       /* interrupt status (16-bit, W1C) */
#define RTL_TCR           0x40       /* TX config (32-bit) */
#define RTL_RCR           0x44       /* RX config (32-bit) */
#define RTL_9346CR        0x50       /* EEPROM cmd (8-bit): config WREN */
#define RTL_CONFIG0       0x51
#define RTL_CONFIG1       0x52
#define RTL_PHYAR         0x60       /* PHY access (32-bit) */
#define RTL_PHY_STATUS    0x6C       /* PHY status (8-bit) */
#define RTL_RMS           0xDA       /* RX max packet size (16-bit) */
#define RTL_CPCR          0xE0       /* C+ command register (16-bit) */
#define RTL_RDSAR         0xE4       /* RX desc base (64-bit) */
#define RTL_ETTHR         0xEC       /* Early TX threshold (8-bit) */

/* CR (0x37) bits. */
#define CR_TE             (1u << 2)  /* TX enable */
#define CR_RE             (1u << 3)  /* RX enable */
#define CR_RST            (1u << 4)  /* soft reset; self-clears */

/* TPPoll (0x38) bits. */
#define TPPOLL_NPQ        (1u << 6)  /* notify normal-prio queue */

/* 9346CR (0x50) values. */
#define EEM_NORMAL        0x00       /* normal mode (config write disabled) */
#define EEM_CONFIG_WRITE  0xC0       /* unlock config registers */

/* TCR (0x40) -- TX config. We keep IFG = 96-bit time and DMA burst
 * unlimited (same numbers Linux r8169 picks for 8168 family). */
#define TCR_IFG_96BIT     (3u << 24)
#define TCR_DMA_UNLIMITED (7u << 8)

/* RCR (0x44) -- RX config. AcceptBroadcast + AcceptMyMac (unicast
 * to our MAC) + AcceptMulticast (we'll accept any since the multicast
 * filter table is permissive); no AcceptAllPhys ("promiscuous").
 * Multi-Read also enabled at bit 14 for PCIe efficiency. */
#define RCR_AAP           (1u << 0)  /* accept all phys (promisc) -- OFF */
#define RCR_APM           (1u << 1)  /* accept physical match (my MAC) */
#define RCR_AM            (1u << 2)  /* accept multicast */
#define RCR_AB            (1u << 3)  /* accept broadcast */
#define RCR_AR            (1u << 4)  /* accept runts */
#define RCR_AER           (1u << 5)  /* accept errors */
#define RCR_DMA_UNLIMITED (7u << 8)
#define RCR_RXFTH_NONE    (7u << 13) /* no defer threshold */
#define RCR_9356SEL       (1u << 6)  /* 93C56 EEPROM select (read-only on 8168) */

/* CPCR (0xE0) -- C+ command register. The bits we care about:
 *   bit 3  Mul_RW          PCIe multi-cycle read/write enable
 *   bit 13 PktCntrDisable  ignore the per-packet counter -- we poll
 * Everything else (RX/TX checksum offload, VLAN insert/strip) is
 * left disabled so descriptors carry a frame's exact length and the
 * eth/arp/ip stack sees raw on-wire bytes. */
#define CPCR_MULRW        (1u << 3)
#define CPCR_PKTCNTR_DIS  (1u << 13)

/* IMR / ISR bits (RTL8168 datasheet § 9.4.7-8). The minimum set we
 * unmask once MSI is wired:
 *   ROK   bit 0 -- one or more RX descriptors completed
 *   RER   bit 1 -- RX error (bumps the recv counter; we just drain)
 *   FOVW  bit 6 -- RX FIFO overflow (also a "drain me NOW" hint)
 *   RDU   bit 4 -- RX descriptor unavailable (we ran out, drain + repost)
 *   LinkChg bit 5 -- diagnostic
 * ISR is W1C; one read+write to ack each bit we observed. */
#define ISR_ROK           (1u <<  0)
#define ISR_RER           (1u <<  1)
#define ISR_TOK           (1u <<  2)
#define ISR_TER           (1u <<  3)
#define ISR_RDU           (1u <<  4)
#define ISR_LINKCHG       (1u <<  5)
#define ISR_FOVW          (1u <<  6)
#define ISR_BITS          (ISR_ROK | ISR_RER | ISR_RDU | ISR_LINKCHG | ISR_FOVW)

/* Descriptor `cmd` field bits (TX + RX share the same word layout). */
#define DESC_OWN          (1u << 31) /* 1 = NIC owns; 0 = driver owns */
#define DESC_EOR          (1u << 30) /* end of ring -- NIC wraps to base */
#define DESC_FS           (1u << 29) /* TX: first segment of packet */
#define DESC_LS           (1u << 28) /* TX: last segment of packet */
#define DESC_LENGTH_MASK  0x00003FFFu

/* ----- ring sizing ------------------------------------------------ */

#define RX_DESC_COUNT     32u
#define TX_DESC_COUNT     32u
#define BUF_SIZE          2048u      /* per-descriptor data buffer */
/* RX max packet size we tell the NIC -- slightly larger than the
 * Ethernet MTU + headers to cover VLAN tags plus a generous slack;
 * the NIC will still cap actual writes at the descriptor's length
 * field. */
#define RX_MAX_PKT_SIZE   1536u

/* ----- on-wire descriptor (Realtek "C+" 16-byte format) ---------- */

struct __attribute__((packed)) rtl_desc {
    uint32_t cmd;          /* OWN, EOR, FS, LS, length, ... */
    uint32_t vlan;         /* VLAN tag (we never set or read) */
    uint32_t addr_lo;      /* buffer phys low 32 */
    uint32_t addr_hi;      /* buffer phys high 32 */
};
_Static_assert(sizeof(struct rtl_desc) == 16, "rtl_desc must be 16 bytes");

/* ----- driver state ---------------------------------------------- */

static volatile uint8_t *g_mmio;

static struct rtl_desc  *g_rx_ring;
static struct rtl_desc  *g_tx_ring;
static uint64_t          g_rx_ring_phys;
static uint64_t          g_tx_ring_phys;

static uint8_t          *g_rx_bufs[RX_DESC_COUNT];
static uint8_t          *g_tx_bufs[TX_DESC_COUNT];
static uint64_t          g_rx_bufs_phys[RX_DESC_COUNT];
static uint64_t          g_tx_bufs_phys[TX_DESC_COUNT];

static uint16_t          g_rx_idx;       /* next desc the NIC will fill */
static uint16_t          g_tx_idx;       /* next desc the driver will use */

static char              g_rtl_name[32];

static uint8_t           g_irq_vector;
static volatile uint64_t g_irq_count;

/* ----- MMIO helpers ---------------------------------------------- */

static inline void  mmio_w8 (uint32_t off, uint8_t  v) {
    *(volatile uint8_t  *)(g_mmio + off) = v;
}
static inline void  mmio_w16(uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(g_mmio + off) = v;
}
static inline void  mmio_w32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(g_mmio + off) = v;
}
static inline uint8_t  mmio_r8 (uint32_t off) {
    return *(volatile uint8_t  *)(g_mmio + off);
}
static inline uint16_t mmio_r16(uint32_t off) {
    return *(volatile uint16_t *)(g_mmio + off);
}
static inline uint32_t mmio_r32(uint32_t off) {
    return *(volatile uint32_t *)(g_mmio + off);
}

/* ----- helpers --------------------------------------------------- */

static bool alloc_buf(uint8_t **out_virt, uint64_t *out_phys) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return false;
    *out_phys = phys;
    *out_virt = (uint8_t *)pmm_phys_to_virt(phys);
    memset(*out_virt, 0, BUF_SIZE);
    return true;
}

/* Pick the MMIO BAR. On PCIe variants (every 8168 / 8111 / 8161
 * revision) the controller exposes its registers at BAR2. The
 * original PCI 8169 used BAR1. We try BAR2 first; if that BAR is
 * empty (real BAR0 might be I/O) fall back to BAR1. */
static int find_mmio_bar(struct pci_dev *dev) {
    if (!dev->bar_is_io[2] && dev->bar[2] != 0) return 2;
    if (!dev->bar_is_io[1] && dev->bar[1] != 0) return 1;
    return -1;
}

/* ----- ring setup ------------------------------------------------ */

static bool rtl_setup_rx(void) {
    g_rx_ring_phys = pmm_alloc_page();
    if (!g_rx_ring_phys) {
        kprintf("[rtl8169] OOM allocating RX descriptor ring\n");
        return false;
    }
    g_rx_ring = (struct rtl_desc *)pmm_phys_to_virt(g_rx_ring_phys);
    memset(g_rx_ring, 0, PAGE_SIZE);

    for (uint16_t i = 0; i < RX_DESC_COUNT; i++) {
        if (!alloc_buf(&g_rx_bufs[i], &g_rx_bufs_phys[i])) {
            kprintf("[rtl8169] OOM allocating RX buffer %u\n", i);
            return false;
        }
        g_rx_ring[i].addr_lo = (uint32_t)(g_rx_bufs_phys[i] & 0xFFFFFFFFu);
        g_rx_ring[i].addr_hi = (uint32_t)(g_rx_bufs_phys[i] >> 32);
        g_rx_ring[i].vlan    = 0;
        /* OWN=NIC, length=BUF_SIZE; EOR set on the last descriptor. */
        uint32_t cmd = DESC_OWN | (RX_MAX_PKT_SIZE & DESC_LENGTH_MASK);
        if (i == RX_DESC_COUNT - 1) cmd |= DESC_EOR;
        g_rx_ring[i].cmd = cmd;
    }
    g_rx_idx = 0;
    return true;
}

static bool rtl_setup_tx(void) {
    g_tx_ring_phys = pmm_alloc_page();
    if (!g_tx_ring_phys) {
        kprintf("[rtl8169] OOM allocating TX descriptor ring\n");
        return false;
    }
    g_tx_ring = (struct rtl_desc *)pmm_phys_to_virt(g_tx_ring_phys);
    memset(g_tx_ring, 0, PAGE_SIZE);

    for (uint16_t i = 0; i < TX_DESC_COUNT; i++) {
        if (!alloc_buf(&g_tx_bufs[i], &g_tx_bufs_phys[i])) {
            kprintf("[rtl8169] OOM allocating TX buffer %u\n", i);
            return false;
        }
        g_tx_ring[i].addr_lo = (uint32_t)(g_tx_bufs_phys[i] & 0xFFFFFFFFu);
        g_tx_ring[i].addr_hi = (uint32_t)(g_tx_bufs_phys[i] >> 32);
        g_tx_ring[i].vlan    = 0;
        /* OWN=driver (cleared) so tx_op can fill the slot immediately;
         * EOR pre-set on the last descriptor so the NIC wraps cleanly
         * even if we never touch its `cmd` again. */
        g_tx_ring[i].cmd = (i == TX_DESC_COUNT - 1) ? DESC_EOR : 0;
    }
    g_tx_idx = 0;
    return true;
}

static void rtl_read_mac(uint8_t out_mac[ETH_ADDR_LEN]) {
    /* IDR0/IDR1 hold the MAC after reset (NIC copies it from EEPROM
     * during power-up; QEMU's rtl8139 does the same trick but at
     * different offsets, which is why our match table excludes the
     * 8139 and we never end up here for that chip). */
    uint32_t low  = mmio_r32(RTL_IDR0);
    uint16_t high = mmio_r16(RTL_IDR4);
    out_mac[0] = (uint8_t)(low       );
    out_mac[1] = (uint8_t)(low  >>  8);
    out_mac[2] = (uint8_t)(low  >> 16);
    out_mac[3] = (uint8_t)(low  >> 24);
    out_mac[4] = (uint8_t)(high      );
    out_mac[5] = (uint8_t)(high >>  8);
}

/* ----- TX / RX (driver-side, called via the net_dev vtable) ------ */

static bool rtl_tx_op(struct net_dev *dev, const void *frame, size_t len) {
    (void)dev;
    if (len == 0 || len > BUF_SIZE) return false;

    uint16_t i = g_tx_idx;
    /* Wait briefly for the slot to be reclaimed (NIC clears OWN when
     * it's done transmitting). For typical traffic patterns the slot
     * is already free; the spin only matters under load. */
    for (int spin = 0; spin < 100000; spin++) {
        if (!(g_tx_ring[i].cmd & DESC_OWN)) break;
    }
    if (g_tx_ring[i].cmd & DESC_OWN) {
        kprintf("[rtl8169] tx: ring full at idx %u\n", i);
        return false;
    }

    memcpy(g_tx_bufs[i], frame, len);

    /* Build the cmd word. Preserve EOR on whichever descriptor is the
     * last in the ring; the NIC needs that to wrap correctly. FS+LS
     * together mean "complete packet in one descriptor". */
    uint32_t cmd = DESC_OWN | DESC_FS | DESC_LS |
                   ((uint32_t)len & DESC_LENGTH_MASK);
    if (i == TX_DESC_COUNT - 1) cmd |= DESC_EOR;
    g_tx_ring[i].cmd = cmd;

    g_tx_idx = (uint16_t)((i + 1u) % TX_DESC_COUNT);

    /* Tell the NIC to look at the normal-priority TX queue. The
     * NPQ bit is self-clearing -- one write per outgoing batch. */
    mmio_w8(RTL_TPPOLL, TPPOLL_NPQ);
    return true;
}

static void rtl_rx_drain_op(struct net_dev *dev) {
    (void)dev;
    /* Walk descriptors starting from where the NIC will write next.
     * For each one whose OWN bit is clear (NIC has filled it) take
     * the frame out, hand it to eth_recv, then re-arm the descriptor
     * by setting OWN + length again. */
    for (;;) {
        struct rtl_desc *d = &g_rx_ring[g_rx_idx];
        uint32_t cmd = d->cmd;
        if (cmd & DESC_OWN) break;     /* nothing new */

        uint32_t len = cmd & DESC_LENGTH_MASK;
        if (len > 4 && len <= BUF_SIZE) {
            /* RTL writes the FCS into the buffer and includes it in
             * `length`. Strip the trailing 4 bytes before handing
             * the frame to the stack. */
            eth_recv(g_rx_bufs[g_rx_idx], len - 4u);
        }

        /* Re-arm: OWN back to NIC, length reset, EOR preserved on
         * the last slot. addr_lo/addr_hi are pinned so we leave them. */
        uint32_t new_cmd = DESC_OWN | (RX_MAX_PKT_SIZE & DESC_LENGTH_MASK);
        if (g_rx_idx == RX_DESC_COUNT - 1) new_cmd |= DESC_EOR;
        d->cmd = new_cmd;

        g_rx_idx = (uint16_t)((g_rx_idx + 1u) % RX_DESC_COUNT);
    }
}

/* MSI handler. ISR is W1C; we read the latched bits, write them
 * straight back to ack, then drain RX. TX completions are handled
 * lazily inside rtl_tx_op (it spins on DESC_OWN), so TOK doesn't need
 * to do anything beyond the ack. */
static void rtl_irq_handler(void *ctx) {
    (void)ctx;
    if (!g_mmio) return;
    g_irq_count++;
    uint16_t isr = mmio_r16(RTL_ISR);
    if (isr) mmio_w16(RTL_ISR, isr);
    if (isr & (ISR_ROK | ISR_RDU | ISR_FOVW)) {
        rtl_rx_drain_op(0);
    }
}

/* ----- net_dev publication --------------------------------------- */

static struct net_dev g_rtl_dev = {
    .name     = g_rtl_name,
    .priv     = 0,
    .tx       = rtl_tx_op,
    .rx_drain = rtl_rx_drain_op,
};

/* ----- PCI probe ------------------------------------------------- */

static int rtl8169_probe(struct pci_dev *dev) {
    if (g_mmio) {
        kprintf("[rtl8169] already bound to a NIC -- ignoring %02x:%02x.%x\n",
                dev->bus, dev->slot, dev->fn);
        return -1;
    }

    kprintf("[rtl8169] probing %02x:%02x.%x  (vid:did %04x:%04x)\n",
            dev->bus, dev->slot, dev->fn, dev->vendor, dev->device);

    pci_dev_enable(dev, PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);

    int bar_idx = find_mmio_bar(dev);
    if (bar_idx < 0) {
        kprintf("[rtl8169] no MMIO BAR found (BAR1=%p BAR2=%p)\n",
                (void *)dev->bar[1], (void *)dev->bar[2]);
        return -2;
    }
    void *bar_virt = pci_map_bar(dev, bar_idx, 0);
    if (!bar_virt) {
        kprintf("[rtl8169] BAR%d map failed (phys=%p)\n",
                bar_idx, (void *)dev->bar[bar_idx]);
        return -3;
    }
    g_mmio = (volatile uint8_t *)bar_virt;
    kprintf("[rtl8169] MMIO BAR%d phys=%p virt=%p\n",
            bar_idx, (void *)dev->bar[bar_idx], (void *)g_mmio);

    /* 1. Mask interrupts before the reset so a stale ISR latch can't
     * fire as soon as we enable the IDT later. We poll throughout. */
    mmio_w16(RTL_IMR, 0x0000);
    mmio_w16(RTL_ISR, 0xFFFF);          /* clear any latched bits */

    /* 2. Soft reset. CR.RST is self-clearing once the controller
     * finishes its internal teardown -- typically <100 us on the
     * 8168 series. We give it ~1 second of polling slack to cover
     * pathological revs. */
    mmio_w8(RTL_CR, CR_RST);
    int ok = 0;
    for (int i = 0; i < 1000000; i++) {
        if ((mmio_r8(RTL_CR) & CR_RST) == 0) { ok = 1; break; }
    }
    if (!ok) {
        kprintf("[rtl8169] reset did not clear CR.RST -- aborting\n");
        return -4;
    }

    /* 3. Read the MAC NOW, before any further writes -- IDR0/IDR1
     * latch the EEPROM value at reset. */
    rtl_read_mac(g_rtl_dev.mac);

    /* 4. Configure the C+ command register. Multi-Read enables PCIe
     * burst reads (mandatory for any reasonable throughput on PCIe);
     * PktCntrDisable shuts off the per-packet IRQ moderation counter
     * (we poll, so we don't want any of the moderation registers
     * influencing descriptor visibility). */
    mmio_w16(RTL_CPCR, CPCR_MULRW | CPCR_PKTCNTR_DIS);

    /* 5. Configure TX. IFG = 96-bit time (standard), DMA burst =
     * unlimited. Other TCR bits (HWVERID, LBK, IFG_EXT) we leave at
     * their reset defaults. */
    mmio_w32(RTL_TCR, TCR_IFG_96BIT | TCR_DMA_UNLIMITED);

    /* 6. Configure RX. Accept broadcast, multicast, our own unicast;
     * DMA burst unlimited; no early-receive threshold. */
    mmio_w32(RTL_RCR, RCR_AB | RCR_AM | RCR_APM | RCR_DMA_UNLIMITED);

    /* 7. RX max packet size. RTL spec § 9.4.20 says values in
     * [64..16383]; we pick a generous 1536 to cover any VLAN-tagged
     * frame plus headers. */
    mmio_w16(RTL_RMS, RX_MAX_PKT_SIZE);

    /* 8. Build the descriptor rings + buffers and tell the NIC where
     * they live. RDSAR / TNPDS are 64-bit and require 256-byte
     * alignment; pmm_alloc_page returns 4 KiB-aligned pages so we're
     * trivially fine. */
    if (!rtl_setup_rx() || !rtl_setup_tx()) return -5;

    mmio_w32(RTL_RDSAR + 0, (uint32_t)(g_rx_ring_phys & 0xFFFFFFFFu));
    mmio_w32(RTL_RDSAR + 4, (uint32_t)(g_rx_ring_phys >> 32));
    mmio_w32(RTL_TNPDS + 0, (uint32_t)(g_tx_ring_phys & 0xFFFFFFFFu));
    mmio_w32(RTL_TNPDS + 4, (uint32_t)(g_tx_ring_phys >> 32));
    /* High-priority TX queue unused; clear it so the controller
     * doesn't accidentally chase a stale phys address from prior
     * firmware. */
    mmio_w32(RTL_THPDS + 0, 0);
    mmio_w32(RTL_THPDS + 4, 0);

    /* 9. Multicast filter (MAR0..MAR7 -- a 64-bit hash table; the NIC
     * hashes each incoming multicast destination MAC into 6 bits and
     * accepts the frame iff the corresponding bit here is set). All
     * 1s = "accept every multicast group", which together with RCR.AM
     * gives us the same wide-open multicast policy as our other NIC
     * drivers. RCR.AB independently covers broadcast (which is what
     * the ARP path actually needs). */
    mmio_w32(RTL_MAR0 + 0, 0xFFFFFFFFu);
    mmio_w32(RTL_MAR0 + 4, 0xFFFFFFFFu);

    /* 10. Light up TX + RX. Order matters per the datasheet: enable
     * RX before driver code can transmit, otherwise an immediately-
     * arriving packet is dropped on the floor. */
    mmio_w8(RTL_CR, CR_RE | CR_TE);

    /* 11. Try to enable MSI (the 8168 family supports MSI; the older
     * PCI 8169 does NOT). MSI-X first for the few revisions that
     * advertise it, plain MSI otherwise. On any failure we leave IMR
     * masked and fall back to net_poll() in the idle loop -- the
     * existing M21 path. QEMU does not emulate this chip family, so
     * this code is exercised only on real silicon. */
    uint8_t vec = irq_alloc_vector(rtl_irq_handler, 0);
    if (vec == 0) {
        kprintf("[rtl8169] no IDT vectors free -- staying polled\n");
    } else if (pci_msix_enable(dev, vec, (uint8_t)apic_read_id(), 1u)) {
        g_irq_vector = vec;
    } else if (pci_msi_enable(dev, vec, (uint8_t)apic_read_id())) {
        g_irq_vector = vec;
    } else {
        kprintf("[rtl8169] no MSI/MSI-X cap -- staying polled "
                "(vec 0x%02x is now idle)\n", (unsigned)vec);
    }
    if (g_irq_vector) {
        /* Clear any latched ISR bits BEFORE unmasking, otherwise the
         * very first MSI fires immediately for a stale event. */
        mmio_w16(RTL_ISR, 0xFFFF);
        mmio_w16(RTL_IMR, ISR_BITS);
        kprintf("[rtl8169] IRQ live on vec 0x%02x  IMR=0x%04x  RX irq-driven\n",
                (unsigned)g_irq_vector, ISR_BITS);
    } else {
        /* No MSI -- re-confirm interrupts stay masked (some revs un-mask
         * on CR write through a side-channel timer). */
        mmio_w16(RTL_IMR, 0x0000);
    }

    /* 12. Build the registry name "rtl8169:bb:ss.f". Shared format
     * with the other NIC drivers so net_dump prints align. */
    static const char hex[] = "0123456789abcdef";
    char *n = g_rtl_name;
    const char *prefix = "rtl8169:";
    while (*prefix) *n++ = *prefix++;
    *n++ = hex[(dev->bus  >> 4) & 0xF]; *n++ = hex[dev->bus  & 0xF]; *n++ = ':';
    *n++ = hex[(dev->slot >> 4) & 0xF]; *n++ = hex[dev->slot & 0xF]; *n++ = '.';
    *n++ = hex[dev->fn & 0xF];
    *n   = '\0';

    net_register(&g_rtl_dev);
    dev->driver_data = &g_rtl_dev;
    return 0;
}

/* Match table -- gigabit Realtek parts only. We deliberately do NOT
 * include 0x10EC:0x8139 (RTL8139 / 100M FastEthernet -- different MAC
 * layer entirely; QEMU emulates exactly this chip via -device rtl8139,
 * and we want our PCI bus loop to skip it cleanly) or 0x10EC:0x8125
 * (RTL8125 / 2.5 GbE -- different programming model). */
static const struct pci_match g_rtl_matches[] = {
    /* RTL8168 / RTL8111 family -- by far the most common gigabit
     * Realtek on consumer boards from ~2007 onwards. Every revision
     * (8168A through 8168H, 8111B through 8111H) advertises this
     * exact ID; the chip rev lives in the upper byte of TCR. */
    { 0x10EC, 0x8168, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },

    /* Original RTL8169 / RTL8169S (PCI gigabit, ~2003). Largely
     * obsolete; included for completeness. */
    { 0x10EC, 0x8169, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },

    /* Some 8111F revisions report 0x8161 instead of 0x8168 due to
     * a strap-pin-based ID switch on certain board layouts. Same
     * silicon, same register map -- we treat them identically. */
    { 0x10EC, 0x8161, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },

    PCI_MATCH_END,
};

static struct pci_driver g_rtl_driver = {
    .name    = "rtl8169",
    .matches = g_rtl_matches,
    .probe   = rtl8169_probe,
    .remove  = 0,
};

void rtl8169_register(void) {
    pci_register_driver(&g_rtl_driver);
}
