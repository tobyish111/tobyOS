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

#define RX_DESC_COUNT    32
#define TX_DESC_COUNT    32
#define BUF_SIZE         2048

/* 82540EM exposes up to 128 KiB of MMIO registers. */
#define E1000_MMIO_BYTES  (128u * 1024u)

/* Hardware descriptor layouts (Intel 82540EM datasheet § 3.2.3 / 3.3.3). */

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

/* ----- MMIO helpers ---------------------------------------------- */

static inline void mmio_write32(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(g_mmio + off) = val;
}
static inline uint32_t mmio_read32(uint32_t off) {
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

    mmio_write32(E1000_RCTL,
                 RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);
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

static void e1000_rx_drain_op(struct net_dev *dev) {
    (void)dev;
    uint64_t irqf = cpu_irqsave();
    /* Walk forward from tail+1 (which is the first descriptor the NIC
     * is allowed to write next). For each descriptor that has DD set,
     * dispatch it, clear the status, advance tail, and bump RDT. */
    for (;;) {
        uint16_t i = (uint16_t)((g_rx_tail + 1) % RX_DESC_COUNT);
        if (!(g_rx_ring[i].status & RXD_STAT_DD)) break;
        uint16_t len = g_rx_ring[i].length;
        if (len > 0 && len <= BUF_SIZE) {
            eth_recv(g_rx_bufs[i], len);
        }
        g_rx_ring[i].status = 0;
        g_rx_tail = i;
        mmio_write32(E1000_RDT, g_rx_tail);
    }
    cpu_irqrestore(irqf);
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

    /* Soft reset and wait for it to clear. */
    mmio_write32(E1000_CTRL, mmio_read32(E1000_CTRL) | CTRL_RST);
    for (int i = 0; i < 1000000; i++) {
        if ((mmio_read32(E1000_CTRL) & CTRL_RST) == 0) break;
    }

    /* Force link up + auto-speed detection. */
    mmio_write32(E1000_CTRL,
                 mmio_read32(E1000_CTRL) | CTRL_SLU | CTRL_ASDE);

    /* Mask everything during ring setup. We'll re-arm IMS only after
     * the rings are live AND MSI is wired -- otherwise an early IRQ
     * could fire with g_rx_tail/g_tx_tail pointing nowhere. */
    mmio_write32(E1000_IMC, 0xFFFFFFFF);
    (void)mmio_read32(E1000_ICR);

    /* Clear the multicast filter table. */
    for (int i = 0; i < 128; i++) {
        mmio_write32(E1000_MTA_BASE + i * 4, 0);
    }

    e1000_read_mac(g_e1000_dev.mac);

    if (!e1000_setup_rx() || !e1000_setup_tx()) return -3;

    /* Try MSI. The 82540EM supports MSI but NOT MSI-X (no x-cap in
     * its config space). pci_msi_enable() routes a single vector at
     * the BSP LAPIC; if that fails we leave the chip on its legacy
     * INT_PIN -- which on the IO APIC path would still arrive via
     * GSI matching INT_LINE, but we don't wire that today. Polling
     * via net_poll() in the idle loop remains the reliable fallback. */
    uint8_t vec = irq_alloc_vector(e1000_irq_handler, 0);
    if (vec == 0) {
        kprintf("[e1000] no IDT vectors free -- staying polled\n");
    } else if (!pci_msi_enable(dev, vec, (uint8_t)apic_read_id())) {
        kprintf("[e1000] no MSI cap -- staying polled "
                "(vec 0x%02x is now idle)\n", (unsigned)vec);
    } else {
        g_irq_vector = vec;
        /* Clear ICR (any stale bits from BIOS), then unmask the
         * subset we care about. The chip will MSI on the next RX
         * frame or TX completion. */
        (void)mmio_read32(E1000_ICR);
        mmio_write32(E1000_IMS, IMS_BITS);
        kprintf("[e1000] IRQ live on vec 0x%02x  IMS=0x%02x  RX/TX irq-driven\n",
                (unsigned)vec, IMS_BITS);
    }

    /* Build the registry name "e1000:bb:ss.f". snprintf-equivalent
     * minimal hex formatter so we don't pull printf into this driver. */
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
