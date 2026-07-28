/* e1000.c -- Intel 82540EM (QEMU's `-device e1000`) NIC driver.
 *
 * Milestone-21 update: this driver is now bound through the PCI
 * driver registry. The probe receives a struct pci_dev*, maps BAR0
 * via pci_map_bar, runs the existing reset / mac-read / ring setup,
 * and registers itself as a struct net_dev so the eth stack's
 * net_default()->tx / ->rx_drain pair routes traffic through us.
 *
 * What the probe does:
 *   1. Bus-master + memory-space enable in the PCI command register.
 *   2. Map BAR0 (MMIO regs) at HHDM+phys with VMM_NOCACHE -- the
 *      Limine memmap marks the PCI aperture as RESERVED so vmm_init
 *      skips it during the bulk HHDM mirror; pci_map_bar does the
 *      explicit map for us.
 *   3. Reset (CTRL.RST), wait for clear.
 *   4. Read the MAC out of RAL/RAH (QEMU has already fed it in from
 *      `-device e1000,mac=...`).
 *   5. Zero the multicast filter table (MTA[0..127]).
 *   6. Allocate + populate RX descriptors and 2 KiB RX buffers; write
 *      RDBAL/RDBAH/RDLEN/RDH/RDT/RCTL.
 *   7. Allocate TX descriptors + 2 KiB TX buffers; write
 *      TDBAL/TDBAH/TDLEN/TDH/TDT/TCTL/TIPG.
 *   8. Fill out g_e1000_dev (struct net_dev) and net_register().
 *
 * IRQs are deliberately left masked. RX is drained from the idle loop
 * via the registered net_dev->rx_drain, which scans descriptors
 * [RDH..tail] for the DD (descriptor done) bit. This costs ~one MMIO
 * read + one descriptor touch per quiet poll.
 *
 * Memory layout for descriptor rings:
 *   - one 4 KiB physical page per ring (32 * 16 bytes = 512 bytes;
 *     lots of headroom, makes the code simple)
 *   - one 4 KiB physical page per buffer (waste, but trivially
 *     correct; the e1000 needs the buffer to lie inside one page
 *     anyway)
 */

#include <tobyos/e1000.h>
#include <tobyos/pci.h>
#include <tobyos/pmm.h>
#include <tobyos/heap.h>
#include <tobyos/eth.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/cpu.h>
#include <tobyos/spinlock.h>
#include <tobyos/irq.h>
#include <tobyos/apic.h>

/* ----- register offsets ------------------------------------------ */

#define E1000_CTRL       0x0000
#define E1000_STATUS     0x0008
#define E1000_EERD       0x0014
#define E1000_ICR        0x00C0
#define E1000_IMS        0x00D0
#define E1000_IMC        0x00D8
#define E1000_RCTL       0x0100
#define E1000_TCTL       0x0400
#define E1000_TIPG       0x0410
#define E1000_RDBAL      0x2800
#define E1000_RDBAH      0x2804
#define E1000_RDLEN      0x2808
#define E1000_RDH        0x2810
#define E1000_RDT        0x2818
#define E1000_TDBAL      0x3800
#define E1000_TDBAH      0x3804
#define E1000_TDLEN      0x3808
#define E1000_TDH        0x3810
#define E1000_TDT        0x3818
#define E1000_MTA_BASE   0x5200          /* 128 dwords */
#define E1000_RAL0       0x5400
#define E1000_RAH0       0x5404

#define RCTL_UPE          (1u << 3)   /* unicast promiscuous; debug only */
#define RAH_AV            (1u << 31)

/* CTRL bits. */
#define CTRL_RST         (1u << 26)
#define CTRL_ASDE        (1u << 5)
#define CTRL_SLU         (1u << 6)

/* RCTL bits. */
#define RCTL_EN          (1u << 1)
#define RCTL_BAM         (1u << 15)      /* broadcast accept */
#define RCTL_BSIZE_2048  0u
#define RCTL_SECRC       (1u << 26)      /* strip CRC */

/* TCTL bits. */
#define TCTL_EN          (1u << 1)
#define TCTL_PSP         (1u << 3)       /* pad short packets */
#define TCTL_CT_SHIFT    4
#define TCTL_COLD_SHIFT  12

/* TX descriptor command bits. */
#define TXD_CMD_EOP      (1u << 0)       /* end of packet */
#define TXD_CMD_IFCS     (1u << 1)       /* insert FCS    */
#define TXD_CMD_RS       (1u << 3)       /* report status */

/* TX descriptor status bits. */
#define TXD_STAT_DD      (1u << 0)       /* descriptor done */

/* RX descriptor status bits. */
#define RXD_STAT_DD      (1u << 0)
#define RXD_STAT_EOP     (1u << 1)

/* IMS/ICR bits (82540EM datasheet § 13.4.20). We unmask only the
 * minimum we actually act on:
 *   TXDW  bit 0  -- TX descriptor written back
 *   LSC   bit 2  -- link status changed (for diagnostics)
 *   RXDMT bit 4  -- RX min threshold (head-near-tail; nudge to drain)
 *   RXT0  bit 7  -- RX timer fired (the actual "RX done" hint)
 * Reading ICR is W1C (write-1-to-clear) on read for any bit set,
 * which conveniently acks every source we just observed. */
#define IMS_TXDW         (1u << 0)
#define IMS_LSC          (1u << 2)
#define IMS_RXDMT        (1u << 4)
#define IMS_RXT0         (1u << 7)
#define IMS_BITS         (IMS_TXDW | IMS_LSC | IMS_RXDMT | IMS_RXT0)

/* ----- ring sizing ------------------------------------------------ */

/* Slice 55: 32 -> 256. RX is drained from poll/idle paths with the NIC's IRQs
 * masked (see the header note), so a burst that arrives while nothing polls has
 * only the ring to land in. MEASURED on a YouTube watch page: 302 "LATE drain"
 * events and repeated batch=32 drains -- i.e. the ring was found COMPLETELY
 * FULL, which means the NIC had already started DROPPING packets. Each drop
 * costs a TCP retransmit, and with no fast-retransmit on these streams recovery
 * runs on RTO backoff -- exactly the 20-100s of total network silence that made
 * the watch page take ~100s to issue its media request and never buffer video.
 * 256 descriptors x 16 bytes = 4096 = still ONE page for the ring (a perfect
 * fit, no allocator change; RDLEN stays 128-byte aligned as the chip requires);
 * the cost is 256 RX buffers instead of 32. */
#define RX_DESC_COUNT    256
#define TX_DESC_COUNT    32
#define BUF_SIZE         2048

/* 82540EM exposes up to 128 KiB of MMIO registers. */
#define E1000_MMIO_BYTES  (128u * 1024u)

/* Hardware descriptor layouts (Intel 82540EM datasheet § 3.2.3 / 3.3.3). */

//helper 

struct __attribute__((packed)) e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
};

struct __attribute__((packed)) e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
};

/* ----- driver state ---------------------------------------------- */

static volatile uint8_t       *g_mmio;          /* HHDM-mapped BAR0 */
static struct e1000_rx_desc   *g_rx_ring;
static struct e1000_tx_desc   *g_tx_ring;
static uint64_t                g_rx_ring_phys;
static uint64_t                g_tx_ring_phys;
static uint8_t                *g_rx_bufs[RX_DESC_COUNT];
static uint8_t                *g_tx_bufs[TX_DESC_COUNT];
static uint64_t                g_rx_bufs_phys[RX_DESC_COUNT];
static uint64_t                g_tx_bufs_phys[TX_DESC_COUNT];
static uint16_t                g_rx_tail;       /* next desc to refill */
static uint16_t                g_tx_tail;       /* next desc to fill   */
static char                    g_e1000_name[32];
static uint8_t                 g_irq_vector;    /* 0 if MSI not active */
static volatile uint64_t       g_irq_count;     /* diag: ISR invocations */

static spinlock_t g_e1000_rx_lock = SPINLOCK_INIT;

/* ----- MMIO helpers ---------------------------------------------- */

static inline void mmio_write32(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(g_mmio + off) = val;
}
static inline uint32_t mmio_read32(uint32_t off) {
    return *(volatile uint32_t *)(g_mmio + off);
}

static void e1000_program_rar0(const uint8_t mac[ETH_ADDR_LEN]) {
    uint32_t ral =
        ((uint32_t)mac[0]) |
        ((uint32_t)mac[1] << 8) |
        ((uint32_t)mac[2] << 16) |
        ((uint32_t)mac[3] << 24);

    uint32_t rah =
        ((uint32_t)mac[4]) |
        ((uint32_t)mac[5] << 8) |
        RAH_AV;

    mmio_write32(E1000_RAL0, ral);
    mmio_write32(E1000_RAH0, rah);
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

/* ----- ring setup ------------------------------------------------ */

static bool e1000_setup_rx(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
        kprintf("[e1000] OOM allocating RX descriptor ring\n");
        return false;
    }
    g_rx_ring_phys = phys;
    g_rx_ring = (struct e1000_rx_desc *)pmm_phys_to_virt(phys);
    memset(g_rx_ring, 0, PAGE_SIZE);

    for (int i = 0; i < RX_DESC_COUNT; i++) {
        if (!alloc_buf(&g_rx_bufs[i], &g_rx_bufs_phys[i])) {
            kprintf("[e1000] OOM allocating RX buffer %d\n", i);
            return false;
        }
        g_rx_ring[i].addr   = g_rx_bufs_phys[i];
        g_rx_ring[i].status = 0;
    }

    mmio_write32(E1000_RDBAL, (uint32_t)(g_rx_ring_phys & 0xFFFFFFFF));
    mmio_write32(E1000_RDBAH, (uint32_t)(g_rx_ring_phys >> 32));
    mmio_write32(E1000_RDLEN, RX_DESC_COUNT * (uint32_t)sizeof(struct e1000_rx_desc));
    mmio_write32(E1000_RDH,   0);
    mmio_write32(E1000_RDT,   RX_DESC_COUNT - 1);
    g_rx_tail = RX_DESC_COUNT - 1;

    uint32_t rctl =
        RCTL_EN |
        RCTL_BAM |
        RCTL_UPE |        /* temporary debug: accept all unicast frames */
        RCTL_SECRC |
        RCTL_BSIZE_2048;

    mmio_write32(E1000_RCTL, rctl);

    kprintf("[e1000] RX live RDBAL=0x%08x RDLEN=%u RDT=%u RCTL=0x%08x\n",
            (uint32_t)(g_rx_ring_phys & 0xFFFFFFFF),
            RX_DESC_COUNT * (uint32_t)sizeof(struct e1000_rx_desc),
            (unsigned)g_rx_tail,
            rctl);

    return true;
}

static bool e1000_setup_tx(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
        kprintf("[e1000] OOM allocating TX descriptor ring\n");
        return false;
    }
    g_tx_ring_phys = phys;
    g_tx_ring = (struct e1000_tx_desc *)pmm_phys_to_virt(phys);
    memset(g_tx_ring, 0, PAGE_SIZE);

    for (int i = 0; i < TX_DESC_COUNT; i++) {
        if (!alloc_buf(&g_tx_bufs[i], &g_tx_bufs_phys[i])) {
            kprintf("[e1000] OOM allocating TX buffer %d\n", i);
            return false;
        }
        g_tx_ring[i].addr   = g_tx_bufs_phys[i];
        /* Pre-mark every slot as "done" so e1000_tx() can reuse them
         * without having to special-case the very first transmissions. */
        g_tx_ring[i].status = TXD_STAT_DD;
    }

    mmio_write32(E1000_TDBAL, (uint32_t)(g_tx_ring_phys & 0xFFFFFFFF));
    mmio_write32(E1000_TDBAH, (uint32_t)(g_tx_ring_phys >> 32));
    mmio_write32(E1000_TDLEN, TX_DESC_COUNT * (uint32_t)sizeof(struct e1000_tx_desc));
    mmio_write32(E1000_TDH,   0);
    mmio_write32(E1000_TDT,   0);
    g_tx_tail = 0;

    mmio_write32(E1000_TCTL,
                 TCTL_EN | TCTL_PSP |
                 (0x10u << TCTL_CT_SHIFT) |
                 (0x40u << TCTL_COLD_SHIFT));
    /* IPG (inter-packet gap) per 82540 manual table 13-77: 10/8/6. */
    mmio_write32(E1000_TIPG, 10u | (8u << 10) | (6u << 20));
    return true;
}

static void e1000_read_mac(uint8_t out_mac[ETH_ADDR_LEN]) {
    /* QEMU populates RAL/RAH from `-device e1000,mac=...` (or its
     * built-in default 52:54:00:12:34:56). EEPROM emulation is
     * incomplete on some QEMU versions; reading RAL/RAH is reliable. */
    uint32_t low  = mmio_read32(E1000_RAL0);
    uint32_t high = mmio_read32(E1000_RAH0);
    out_mac[0] = (uint8_t)(low       );
    out_mac[1] = (uint8_t)(low  >>  8);
    out_mac[2] = (uint8_t)(low  >> 16);
    out_mac[3] = (uint8_t)(low  >> 24);
    out_mac[4] = (uint8_t)(high      );
    out_mac[5] = (uint8_t)(high >>  8);
}

/* ----- TX / RX (driver-side, called via the net_dev vtable) ------ */

static bool e1000_tx_op(struct net_dev *dev, const void *frame, size_t len) {
    (void)dev;
    if (len == 0 || len > BUF_SIZE) return false;

    uint16_t i = g_tx_tail;
    /* Wait briefly for the descriptor to be reclaimed. We don't yield
     * (might be called from a syscall path); a few thousand reads is
     * plenty for a quiet emulated NIC. */
    for (int spin = 0; spin < 100000; spin++) {
        if (g_tx_ring[i].status & TXD_STAT_DD) break;
    }
    if (!(g_tx_ring[i].status & TXD_STAT_DD)) {
        kprintf("[e1000] tx: ring full at idx %u\n", i);
        return false;
    }

    memcpy(g_tx_bufs[i], frame, len);
    g_tx_ring[i].length = (uint16_t)len;
    g_tx_ring[i].cmd    = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    g_tx_ring[i].status = 0;

    g_tx_tail = (uint16_t)((i + 1) % TX_DESC_COUNT);
    __asm__ volatile ("" ::: "memory");
    mmio_write32(E1000_TDT, g_tx_tail);
    return true;
}

#ifdef CHROMIUM_BOOT
/* Slice 55: RX-SERVICING AUTOPSY. A YouTube watch page delivers data in bursts
 * separated by 20-100s of TOTAL network silence, during which the box is idle
 * (1-4 ring-3 samples/interval), chrome's threads are blocked, no TCP RX buffer
 * ever fills and nothing retransmits. Two possibilities remain and they need
 * opposite fixes: either packets ARE arriving and we are not DRAINING them
 * (RX is drained from poll/idle paths, IRQs are masked -- see the header note),
 * or the peer genuinely sends nothing. This counts drains, packets, the biggest
 * batch found in one drain (a large batch == we were LATE), and the gap since
 * the previous packet. Reported every ~5s and on any batch >= 8. */
static uint64_t g_dbg_drains, g_dbg_pkts, g_dbg_last_pkt_ms, g_dbg_report_ms;
static uint32_t g_dbg_max_batch;
extern uint64_t klog_ms(void);
#endif

static void e1000_rx_drain_op(struct net_dev *dev) {
    (void)dev;
#ifdef CHROMIUM_BOOT
    uint32_t batch = 0;
#endif
    uint64_t irqf = spin_lock_irqsave(&g_e1000_rx_lock);
    /* Walk forward from tail+1 (which is the first descriptor the NIC
     * is allowed to write next). For each descriptor that has DD set,
     * dispatch it, clear the status, advance tail, and bump RDT. */
    for (;;) {
        uint16_t i = (uint16_t)((g_rx_tail + 1) % RX_DESC_COUNT);
        if (!(g_rx_ring[i].status & RXD_STAT_DD)) break;
        uint16_t len = g_rx_ring[i].length;
        if (len > 0 && len <= BUF_SIZE) {
            const uint8_t *f = g_rx_bufs[i];
        
            if (len >= 14) {
                uint16_t et =
                    ((uint16_t)f[12] << 8) |
                    ((uint16_t)f[13]);
        
                if (et == 0x0800 && len >= 42) {
                    const uint8_t *ip = f + 14;
                    uint8_t proto = ip[9];
        
                    if (proto == 17) {
                        uint8_t ihl = (uint8_t)((ip[0] & 0x0F) * 4);
                        if (ihl >= 20 && len >= 14 + ihl + 8) {
                            const uint8_t *udp = ip + ihl;
                            uint16_t sport = ((uint16_t)udp[0] << 8) | udp[1];
                            uint16_t dport = ((uint16_t)udp[2] << 8) | udp[3];
        
                            if ((sport == 67 && dport == 68) ||
                                (sport == 68 && dport == 67)) {
                                kprintf("[e1000] RX DHCP frame len=%u udp %u -> %u dst=%02x:%02x:%02x:%02x:%02x:%02x\n",
                                        (unsigned)len,
                                        (unsigned)sport,
                                        (unsigned)dport,
                                        f[0], f[1], f[2], f[3], f[4], f[5]);
                            }
                        }
                    }
                }
            }
        
            eth_recv(g_rx_bufs[i], len);
#ifdef CHROMIUM_BOOT
            batch++;
#endif
        }
        g_rx_ring[i].status = 0;
        g_rx_tail = i;
        mmio_write32(E1000_RDT, g_rx_tail);
    }
    spin_unlock_irqrestore(&g_e1000_rx_lock, irqf);

#ifdef CHROMIUM_BOOT
    {
        uint64_t now = klog_ms();
        g_dbg_drains++;
        if (batch) {
            uint64_t gap = g_dbg_last_pkt_ms ? (now - g_dbg_last_pkt_ms) : 0;
            g_dbg_pkts += batch;
            g_dbg_last_pkt_ms = now;
            if (batch > g_dbg_max_batch) g_dbg_max_batch = batch;
            /* A big batch means packets had piled up while nobody drained. */
            if (batch >= 8)
                kprintf("[rxdbg] LATE drain: batch=%u pkts (gap=%lums since "
                        "last packet)\n", (unsigned)batch, (unsigned long)gap);
        }
        if (now - g_dbg_report_ms >= 5000) {
            g_dbg_report_ms = now;
            kprintf("[rxdbg] %lums drains=%lu pkts=%lu maxbatch=%u "
                    "quiet=%lums\n", (unsigned long)now,
                    (unsigned long)g_dbg_drains, (unsigned long)g_dbg_pkts,
                    (unsigned)g_dbg_max_batch,
                    (unsigned long)(g_dbg_last_pkt_ms ? now - g_dbg_last_pkt_ms : 0));
        }
    }
#endif
}

/* MSI handler. Reading ICR clears every cause bit it returns, so a
 * second IRQ won't re-fire for sources we just acked. We don't bother
 * to demux causes -- if the chip raised an IRQ, the cheapest correct
 * action is to drain RX and let the TX path naturally reclaim slots
 * via TXD_STAT_DD on the next e1000_tx_op call. */
static void e1000_irq_handler(void *ctx) {
    (void)ctx;
    if (!g_mmio) return;
    g_irq_count++;
    (void)mmio_read32(E1000_ICR);
    e1000_rx_drain_op(0);
}

/* ----- net_dev publication --------------------------------------- */

static struct net_dev g_e1000_dev = {
    .name     = g_e1000_name,
    .priv     = 0,
    .tx       = e1000_tx_op,
    .rx_drain = e1000_rx_drain_op,
};

/* ----- PCI probe ------------------------------------------------- */

static int e1000_probe(struct pci_dev *dev) {
    if (g_mmio) {
        kprintf("[e1000] already bound to a NIC -- ignoring %02x:%02x.%x\n",
                dev->bus, dev->slot, dev->fn);
        return -1;
    }

    kprintf("[e1000] probing %02x:%02x.%x  (vid:did %04x:%04x)\n",
            dev->bus, dev->slot, dev->fn, dev->vendor, dev->device);

    pci_dev_enable(dev, PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);

    void *bar0_virt = pci_map_bar(dev, 0, E1000_MMIO_BYTES);
    if (!bar0_virt) {
        kprintf("[e1000] BAR0 map failed (phys=%p)\n", (void *)dev->bar[0]);
        return -2;
    }

    g_mmio = (volatile uint8_t *)bar0_virt;
    kprintf("[e1000] MMIO BAR0 phys=%p virt=%p (%lu KiB UC)\n",
            (void *)dev->bar[0], (void *)g_mmio,
            (unsigned long)(E1000_MMIO_BYTES / 1024u));

    /*
     * Soft reset.
     * After this, many device registers return to defaults, so anything
     * important must be programmed after reset, not before it.
     */
    mmio_write32(E1000_CTRL, mmio_read32(E1000_CTRL) | CTRL_RST);
    for (int i = 0; i < 1000000; i++) {
        if ((mmio_read32(E1000_CTRL) & CTRL_RST) == 0) {
            break;
        }
    }

    /*
     * Force link up + auto-speed detection.
     */
    mmio_write32(E1000_CTRL,
                 mmio_read32(E1000_CTRL) | CTRL_SLU | CTRL_ASDE);

    /*
     * Mask interrupts during ring setup.
     */
    mmio_write32(E1000_IMC, 0xFFFFFFFF);
    (void)mmio_read32(E1000_ICR);

    /*
     * Clear multicast filter table.
     */
    for (int i = 0; i < 128; i++) {
        mmio_write32(E1000_MTA_BASE + i * 4, 0);
    }

    /*
     * Read MAC and explicitly program receive address slot 0.
     * This is important for unicast DHCP OFFER/ACK reception.
     */
    e1000_read_mac(g_e1000_dev.mac);
    e1000_program_rar0(g_e1000_dev.mac);

    kprintf("[e1000] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            g_e1000_dev.mac[0], g_e1000_dev.mac[1],
            g_e1000_dev.mac[2], g_e1000_dev.mac[3],
            g_e1000_dev.mac[4], g_e1000_dev.mac[5]);

    /*
     * Now set up rings. RX setup should enable RCTL after RDBAL/RDBAH/RDLEN
     * and RDH/RDT are programmed.
     */
    if (!e1000_setup_rx() || !e1000_setup_tx()) {
        return -3;
    }

    uint8_t vec = irq_alloc_vector(e1000_irq_handler, 0);
    if (vec == 0) {
        kprintf("[e1000] no IDT vectors free -- staying polled\n");
    } else if (!pci_msi_enable(dev, vec, (uint8_t)apic_read_id())) {
        kprintf("[e1000] no MSI cap -- staying polled "
                "(vec 0x%02x is now idle)\n", (unsigned)vec);
    } else {
        g_irq_vector = vec;
        (void)mmio_read32(E1000_ICR);
        mmio_write32(E1000_IMS, IMS_BITS);
        kprintf("[e1000] IRQ live on vec 0x%02x  IMS=0x%02x  RX/TX irq-driven\n",
                (unsigned)vec, IMS_BITS);
    }

    static const char hex[] = "0123456789abcdef";
    char *n = g_e1000_name;
    *n++ = 'e'; *n++ = '1'; *n++ = '0'; *n++ = '0'; *n++ = '0'; *n++ = ':';
    *n++ = hex[(dev->bus >> 4) & 0xF];  *n++ = hex[dev->bus & 0xF]; *n++ = ':';
    *n++ = hex[(dev->slot >> 4) & 0xF]; *n++ = hex[dev->slot & 0xF]; *n++ = '.';
    *n++ = hex[dev->fn & 0xF];
    *n   = '\0';

    net_register(&g_e1000_dev);
    dev->driver_data = &g_e1000_dev;

    return 0;
}

static const struct pci_match g_e1000_matches[] = {
    /* QEMU's default e1000 (Intel 82540EM). We'll add 82574L (e1000e)
     * as its OWN driver in step 4 -- the register set is similar but
     * the ring layout has small differences. */
    { E1000_VENDOR, E1000_DEVICE,
      PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    PCI_MATCH_END,
};

static struct pci_driver g_e1000_driver = {
    .name    = "e1000",
    .matches = g_e1000_matches,
    .probe   = e1000_probe,
    .remove  = 0,
};

void e1000_register(void) {
    pci_register_driver(&g_e1000_driver);
}
