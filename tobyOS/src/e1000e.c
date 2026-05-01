/* e1000e.c -- Intel 82574L (PCIe gigabit, "e1000e" family) NIC driver.
 *
 * Bound through the milestone-21 PCI driver registry. Sibling of
 * src/e1000.c -- the 82574L is wire-compatible with the 82540EM at
 * the BAR0 register layout, so most of the body is identical. The
 * deltas:
 *
 *   - PCI device ID is 0x10D3 (82574L). QEMU emulates exactly this
 *     chip with `-device e1000e`. The match table also includes the
 *     handful of 82577/82579/I217/I218/I219 device IDs that share
 *     the same legacy descriptor + register layout for basic
 *     polled-mode RX/TX -- they are the most common e1000e variants
 *     soldered onto Intel motherboards from ~2009 onwards.
 *
 *     Skylake / 100-series PCH laptops and desktops typically expose
 *     I219-LM (156F) or I219-V (1570); Kaby / 200-series often use
 *     15B7/15B8. Compact OEM systems (e.g. HP 260 G2 mini) may instead
 *     ship a Realtek 8168/8161 NIC — that path is rtl8169.c, not here.
 *     Same BAR + ring model as earlier PCH parts for the Intel IDs below.
 *
 *   - 82574L re-arms interrupts via IAM (interrupt auto-mask) in
 *     addition to IMS/IMC. We poll, so we explicitly clear IAM
 *     alongside writing IMC = 0xFFFFFFFF. Hits any chip that left
 *     IAM populated by previous firmware.
 *
 *   - The 82574L's PCI MMIO BAR0 is also 128 KiB and exposes the
 *     same RDBAL/RDBAH/RDLEN/RDH/RDT/RCTL + TDBAL/TDBAH/TDLEN/TDH/
 *     TDT/TCTL set we already drive on the 82540EM. The legacy
 *     16-byte descriptor format works identically (the 82574L's
 *     extended-rx-descriptor mode is OPT-IN via RFCTL.EXSTEN and
 *     we don't enable it).
 *
 *   - RAL0/RAH0 hold the MAC after reset just like on the 82540
 *     (QEMU populates them from `-device e1000e,mac=...`).
 *
 * IRQs are masked at every level. RX is drained from the idle loop
 * via the registered net_dev->rx_drain. Same model as the e1000
 * driver -- they coexist; whichever one's match table fires is the
 * one bound for that PCI function.
 */

#include <tobyos/net.h>
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
/* These match the 82540EM exactly for everything we touch -- the
 * 82574L datasheet (§ 10) is identical at these addresses. The IAM
 * register at 0x00E0 is e1000e-specific (no 82540 equivalent). */

#define E1000E_CTRL       0x0000
#define E1000E_STATUS     0x0008
#define E1000E_ICR        0x00C0
#define E1000E_IMS        0x00D0
#define E1000E_IMC        0x00D8
#define E1000E_IAM        0x00E0          /* interrupt auto-mask (e1000e only) */
#define E1000E_RCTL       0x0100
#define E1000E_TCTL       0x0400
#define E1000E_TIPG       0x0410
#define E1000E_RDBAL      0x2800
#define E1000E_RDBAH      0x2804
#define E1000E_RDLEN      0x2808
#define E1000E_RDH        0x2810
#define E1000E_RDT        0x2818
#define E1000E_TDBAL      0x3800
#define E1000E_TDBAH      0x3804
#define E1000E_TDLEN      0x3808
#define E1000E_TDH        0x3810
#define E1000E_TDT        0x3818
#define E1000E_MTA_BASE   0x5200          /* 128 dwords */
#define E1000E_RAL0       0x5400
#define E1000E_RAH0       0x5404

/* CTRL bits. */
#define CTRL_RST         (1u << 26)
#define CTRL_ASDE        (1u << 5)
#define CTRL_SLU         (1u << 6)
#define CTRL_PHY_RST     (1u << 31)       /* e1000e: cleared after RST */
#define CTRL_LRST        (1u << 3)        /* link reset, must be 0 */

/* RCTL bits. */
#define RCTL_EN          (1u << 1)
#define RCTL_BAM         (1u << 15)       /* broadcast accept */
#define RCTL_BSIZE_2048  0u
#define RCTL_SECRC       (1u << 26)       /* strip CRC */

/* TCTL bits. */
#define TCTL_EN          (1u << 1)
#define TCTL_PSP         (1u << 3)        /* pad short packets */
#define TCTL_CT_SHIFT    4
#define TCTL_COLD_SHIFT  12

/* TX descriptor command bits. */
#define TXD_CMD_EOP      (1u << 0)
#define TXD_CMD_IFCS     (1u << 1)
#define TXD_CMD_RS       (1u << 3)

/* TX/RX descriptor status bits. */
#define TXD_STAT_DD      (1u << 0)
#define RXD_STAT_DD      (1u << 0)
#define RXD_STAT_EOP     (1u << 1)

/* IMS/ICR bits (Intel 82574 datasheet § 13.4.20). Same definitions
 * the legacy e1000 driver uses; they're stable across the family. */
#define IMS_TXDW         (1u << 0)
#define IMS_LSC          (1u << 2)
#define IMS_RXDMT        (1u << 4)
#define IMS_RXT0         (1u << 7)
#define IMS_BITS         (IMS_TXDW | IMS_LSC | IMS_RXDMT | IMS_RXT0)

/* ----- ring sizing ------------------------------------------------ */

#define RX_DESC_COUNT    32
#define TX_DESC_COUNT    32
#define BUF_SIZE         2048
#define E1000E_MMIO_BYTES (128u * 1024u)

/* Hardware descriptor layouts (Intel 82574 datasheet § 7.1 / 7.2 --
 * legacy 16-byte format, identical to the 82540EM layout). */

struct __attribute__((packed)) e1000e_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
};

struct __attribute__((packed)) e1000e_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
};

/* ----- driver state ---------------------------------------------- */

static volatile uint8_t        *g_mmio;
static struct e1000e_rx_desc   *g_rx_ring;
static struct e1000e_tx_desc   *g_tx_ring;
static uint64_t                 g_rx_ring_phys;
static uint64_t                 g_tx_ring_phys;
static uint8_t                 *g_rx_bufs[RX_DESC_COUNT];
static uint8_t                 *g_tx_bufs[TX_DESC_COUNT];
static uint64_t                 g_rx_bufs_phys[RX_DESC_COUNT];
static uint64_t                 g_tx_bufs_phys[TX_DESC_COUNT];
static uint16_t                 g_rx_tail;
static uint16_t                 g_tx_tail;
static char                     g_e1000e_name[32];
static uint8_t                  g_irq_vector;     /* 0 if MSI/MSI-X off */
static volatile uint64_t        g_irq_count;

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

static bool e1000e_setup_rx(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
        kprintf("[e1000e] OOM allocating RX descriptor ring\n");
        return false;
    }
    g_rx_ring_phys = phys;
    g_rx_ring = (struct e1000e_rx_desc *)pmm_phys_to_virt(phys);
    memset(g_rx_ring, 0, PAGE_SIZE);

    for (int i = 0; i < RX_DESC_COUNT; i++) {
        if (!alloc_buf(&g_rx_bufs[i], &g_rx_bufs_phys[i])) {
            kprintf("[e1000e] OOM allocating RX buffer %d\n", i);
            return false;
        }
        g_rx_ring[i].addr   = g_rx_bufs_phys[i];
        g_rx_ring[i].status = 0;
    }

    mmio_write32(E1000E_RDBAL, (uint32_t)(g_rx_ring_phys & 0xFFFFFFFF));
    mmio_write32(E1000E_RDBAH, (uint32_t)(g_rx_ring_phys >> 32));
    mmio_write32(E1000E_RDLEN, RX_DESC_COUNT * (uint32_t)sizeof(struct e1000e_rx_desc));
    mmio_write32(E1000E_RDH,   0);
    mmio_write32(E1000E_RDT,   RX_DESC_COUNT - 1);
    g_rx_tail = RX_DESC_COUNT - 1;

    mmio_write32(E1000E_RCTL,
                 RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048);
    return true;
}

static bool e1000e_setup_tx(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) {
        kprintf("[e1000e] OOM allocating TX descriptor ring\n");
        return false;
    }
    g_tx_ring_phys = phys;
    g_tx_ring = (struct e1000e_tx_desc *)pmm_phys_to_virt(phys);
    memset(g_tx_ring, 0, PAGE_SIZE);

    for (int i = 0; i < TX_DESC_COUNT; i++) {
        if (!alloc_buf(&g_tx_bufs[i], &g_tx_bufs_phys[i])) {
            kprintf("[e1000e] OOM allocating TX buffer %d\n", i);
            return false;
        }
        g_tx_ring[i].addr   = g_tx_bufs_phys[i];
        /* Pre-mark every slot as "done" so e1000e_tx_op can reuse them
         * without having to special-case the very first transmissions. */
        g_tx_ring[i].status = TXD_STAT_DD;
    }

    mmio_write32(E1000E_TDBAL, (uint32_t)(g_tx_ring_phys & 0xFFFFFFFF));
    mmio_write32(E1000E_TDBAH, (uint32_t)(g_tx_ring_phys >> 32));
    mmio_write32(E1000E_TDLEN, TX_DESC_COUNT * (uint32_t)sizeof(struct e1000e_tx_desc));
    mmio_write32(E1000E_TDH,   0);
    mmio_write32(E1000E_TDT,   0);
    g_tx_tail = 0;

    mmio_write32(E1000E_TCTL,
                 TCTL_EN | TCTL_PSP |
                 (0x10u << TCTL_CT_SHIFT) |
                 (0x40u << TCTL_COLD_SHIFT));
    /* IPG (inter-packet gap) per 82574 datasheet § 13.4.34: 8/8/6.
     * Slightly different from 82540 (10/8/6) but both work on either
     * silicon; using the 82574-recommended value is the safer choice
     * on the 82574-and-later chips. */
    mmio_write32(E1000E_TIPG, 8u | (8u << 10) | (6u << 20));
    return true;
}

static void e1000e_read_mac(uint8_t out_mac[ETH_ADDR_LEN]) {
    /* QEMU populates RAL/RAH from `-device e1000e,mac=...`; real
     * silicon copies it out of the on-chip EEPROM during reset.
     * Either way, RAL/RAH is the reliable read path. */
    uint32_t low  = mmio_read32(E1000E_RAL0);
    uint32_t high = mmio_read32(E1000E_RAH0);
    out_mac[0] = (uint8_t)(low       );
    out_mac[1] = (uint8_t)(low  >>  8);
    out_mac[2] = (uint8_t)(low  >> 16);
    out_mac[3] = (uint8_t)(low  >> 24);
    out_mac[4] = (uint8_t)(high      );
    out_mac[5] = (uint8_t)(high >>  8);
}

/* ----- TX / RX (driver-side, called via the net_dev vtable) ------ */

static bool e1000e_tx_op(struct net_dev *dev, const void *frame, size_t len) {
    (void)dev;
    if (len == 0 || len > BUF_SIZE) return false;

    uint16_t i = g_tx_tail;
    for (int spin = 0; spin < 100000; spin++) {
        if (g_tx_ring[i].status & TXD_STAT_DD) break;
    }
    if (!(g_tx_ring[i].status & TXD_STAT_DD)) {
        kprintf("[e1000e] tx: ring full at idx %u\n", i);
        return false;
    }

    memcpy(g_tx_bufs[i], frame, len);
    g_tx_ring[i].length = (uint16_t)len;
    g_tx_ring[i].cmd    = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    g_tx_ring[i].status = 0;

    g_tx_tail = (uint16_t)((i + 1) % TX_DESC_COUNT);
    __asm__ volatile ("" ::: "memory");
    mmio_write32(E1000E_TDT, g_tx_tail);
    return true;
}

static void e1000e_rx_drain_op(struct net_dev *dev) {
    (void)dev;
    uint64_t irqf = cpu_irqsave();
    for (;;) {
        uint16_t i = (uint16_t)((g_rx_tail + 1) % RX_DESC_COUNT);
        if (!(g_rx_ring[i].status & RXD_STAT_DD)) break;
        uint16_t len = g_rx_ring[i].length;
        if (len > 0 && len <= BUF_SIZE) {
            eth_recv(g_rx_bufs[i], len);
        }
        g_rx_ring[i].status = 0;
        g_rx_tail = i;
        mmio_write32(E1000E_RDT, g_rx_tail);
    }
    cpu_irqrestore(irqf);
}

/* MSI / MSI-X handler. ICR is read-to-clear on the 82574L (same as
 * the 82540). After clearing we drain RX and let the TX path reclaim
 * descriptors lazily on the next e1000e_tx_op call. We deliberately
 * do NOT use IAM (auto-mask) here; it adds complexity without buying
 * us anything for a single-vector driver. */
static void e1000e_irq_handler(void *ctx) {
    (void)ctx;
    if (!g_mmio) return;
    g_irq_count++;
    (void)mmio_read32(E1000E_ICR);
    e1000e_rx_drain_op(0);
}

/* ----- net_dev publication --------------------------------------- */

static struct net_dev g_e1000e_dev = {
    .name     = g_e1000e_name,
    .priv     = 0,
    .tx       = e1000e_tx_op,
    .rx_drain = e1000e_rx_drain_op,
};

/* ----- PCI probe ------------------------------------------------- */

static int e1000e_probe(struct pci_dev *dev) {
    if (g_mmio) {
        kprintf("[e1000e] already bound to a NIC -- ignoring %02x:%02x.%x\n",
                dev->bus, dev->slot, dev->fn);
        return -1;
    }

    kprintf("[e1000e] probing %02x:%02x.%x  (vid:did %04x:%04x)\n",
            dev->bus, dev->slot, dev->fn, dev->vendor, dev->device);

    pci_dev_enable(dev, PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);

    void *bar0_virt = pci_map_bar(dev, 0, E1000E_MMIO_BYTES);
    if (!bar0_virt) {
        kprintf("[e1000e] BAR0 map failed (phys=%p)\n", (void *)dev->bar[0]);
        return -2;
    }
    g_mmio = (volatile uint8_t *)bar0_virt;
    kprintf("[e1000e] MMIO BAR0 phys=%p virt=%p (%lu KiB UC)\n",
            (void *)dev->bar[0], (void *)g_mmio,
            (unsigned long)(E1000E_MMIO_BYTES / 1024u));

    /* 1. Mask interrupts BEFORE the reset (some firmware leaves IMS
     * partially enabled). */
    mmio_write32(E1000E_IMC, 0xFFFFFFFF);
    mmio_write32(E1000E_IAM, 0x00000000);

    /* 2. Soft reset and wait for it to clear. The 82574L typically
     * clears CTRL.RST within ~1 us; we still spin generously to
     * cover slower real silicon. */
    mmio_write32(E1000E_CTRL, mmio_read32(E1000E_CTRL) | CTRL_RST);
    for (int i = 0; i < 1000000; i++) {
        if ((mmio_read32(E1000E_CTRL) & CTRL_RST) == 0) break;
    }

    /* 3. After reset, mask interrupts AGAIN -- some 82574 revisions
     * re-enable a few sources. Then clear ICR by reading it (W1C-on-
     * read on this part). */
    mmio_write32(E1000E_IMC, 0xFFFFFFFF);
    mmio_write32(E1000E_IAM, 0x00000000);
    (void)mmio_read32(E1000E_ICR);

    /* 4. Force link up + auto-speed detection. Explicitly clear LRST
     * + PHY_RST: on 82574L these two bits sometimes survive RST and
     * keep the PHY held in reset. */
    uint32_t ctrl = mmio_read32(E1000E_CTRL);
    ctrl &= ~(CTRL_LRST | CTRL_PHY_RST);
    ctrl |= (CTRL_SLU | CTRL_ASDE);
    mmio_write32(E1000E_CTRL, ctrl);

    /* 5. Clear the multicast filter table. */
    for (int i = 0; i < 128; i++) {
        mmio_write32(E1000E_MTA_BASE + i * 4, 0);
    }

    e1000e_read_mac(g_e1000e_dev.mac);

    if (!e1000e_setup_rx() || !e1000e_setup_tx()) return -3;

    /* 6. Try MSI-X first (the 82574L has a 5-vector MSI-X table that
     * QEMU's e1000e correctly emulates), fall back to plain MSI. We
     * route a single vector at the BSP LAPIC; the 82574L delivers all
     * causes via that vector unless we steered them via IVAR, which
     * we don't. On real silicon both paths work; on QEMU MSI-X is
     * the reliable one. */
    uint8_t vec = irq_alloc_vector(e1000e_irq_handler, 0);
    if (vec == 0) {
        kprintf("[e1000e] no IDT vectors free -- staying polled\n");
    } else if (pci_msix_enable(dev, vec, (uint8_t)apic_read_id(), 1u)) {
        g_irq_vector = vec;
    } else if (pci_msi_enable(dev, vec, (uint8_t)apic_read_id())) {
        g_irq_vector = vec;
    } else {
        kprintf("[e1000e] no MSI/MSI-X cap -- staying polled "
                "(vec 0x%02x is now idle)\n", (unsigned)vec);
    }
    if (g_irq_vector) {
        (void)mmio_read32(E1000E_ICR);
        mmio_write32(E1000E_IMS, IMS_BITS);
        kprintf("[e1000e] IRQ live on vec 0x%02x  IMS=0x%02x  RX/TX irq-driven\n",
                (unsigned)g_irq_vector, IMS_BITS);
    }

    static const char hex[] = "0123456789abcdef";
    char *n = g_e1000e_name;
    *n++ = 'e'; *n++ = '1'; *n++ = '0'; *n++ = '0'; *n++ = '0'; *n++ = 'e';
    *n++ = ':';
    *n++ = hex[(dev->bus  >> 4) & 0xF]; *n++ = hex[dev->bus  & 0xF]; *n++ = ':';
    *n++ = hex[(dev->slot >> 4) & 0xF]; *n++ = hex[dev->slot & 0xF]; *n++ = '.';
    *n++ = hex[dev->fn & 0xF];
    *n   = '\0';

    net_register(&g_e1000e_dev);
    dev->driver_data = &g_e1000e_dev;
    return 0;
}

/* Conservative match table -- only chips whose basic register layout
 * we're confident about. Order doesn't matter; the bus loop tries
 * every entry and stops on the first hit. Each comment lists a
 * representative product so it's obvious WHY this device id appears.
 *
 * We deliberately do NOT shadow the 82540EM (0x100E) here -- the
 * legacy `e1000` driver covers it; matching it twice would just
 * shuffle which probe wins on a system with both drivers registered. */
static const struct pci_match g_e1000e_matches[] = {
    /* QEMU's `-device e1000e` -- exactly what `make run-e1000e` boots. */
    { 0x8086, 0x10D3, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },

    /* 82577LM / 82577LC (HM55-era laptops, ~2010). Wire-compatible
     * legacy descriptor mode + RAL/RAH MAC source. */
    { 0x8086, 0x10EA, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x10EB, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },

    /* 82579LM / 82579V (Cougar Point / Panther Point chipsets,
     * ~2011-2012). Same RX/TX descriptor format. */
    { 0x8086, 0x1502, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x1503, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },

    /* I217-LM / I217-V (Lynx Point, ~2013). Vendor recommends the
     * 8254x init recipe for basic operation. */
    { 0x8086, 0x153A, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x153B, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },

    /* I218-V / I218-LM (Wildcat Point / 9-series PCH, ~2014). */
    { 0x8086, 0x1559, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x155A, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },

    /* I219-LM / I219-V (Sunrise Point / 100-series Skylake PCH). */
    { 0x8086, 0x156F, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x1570, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },

    /* I219-LM / I219-V (Union Point / 200-series Kaby Lake PCH). */
    { 0x8086, 0x15B7, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x15B8, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    /* Lewisburg / Xeon PCH onboard (same MAC core as SPT I219). */
    { 0x8086, 0x15B9, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },

    /* I218-LM / I218-V second and third generations (Haswell refresh
     * through Skylake client PCH). Linux e1000e uses the same driver. */
    { 0x8086, 0x15A0, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x15A1, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x15A2, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x15A3, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },

    /* I219 fourth/fifth gen (300-series / Cannon Lake PCH). */
    { 0x8086, 0x15D7, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x15D8, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x15E3, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x15D6, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },

    /* 400-series / Comet Lake PCH (CNP + ICP). */
    { 0x8086, 0x15BD, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x15BE, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x15BB, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x15BC, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x15DF, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x15E0, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x15E1, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x15E2, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },

    /* Tiger Lake / Rocket Lake / Alder Lake / Raptor Lake PCH (TGP/ADP/RPL). */
    { 0x8086, 0x15FB, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x15FC, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x15F9, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x15FA, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x15F4, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x15F5, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x1A1E, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x1A1F, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x1A1C, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x1A1D, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x0DC5, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x0DC6, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x0DC7, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x0DC8, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },

    /* Mobile / low-power PCH (CMP through NVL) — IDs from Linux e1000e/hw.h. */
    { 0x8086, 0x0D4E, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x0D4F, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x0D4C, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x0D4D, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x0D53, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x0D55, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x550A, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x550B, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x550C, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x550D, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x550E, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x550F, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x5510, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x5511, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x57A0, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x57A1, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x57B3, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x57B4, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x57B7, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x57B8, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x57B9, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x57BA, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },

    /* Extra PCIe gigabit parts Linux binds to e1000e (82574 variant). */
    { 0x8086, 0x10F6, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x8086, 0x150C, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },

    PCI_MATCH_END,
};

static struct pci_driver g_e1000e_driver = {
    .name    = "e1000e",
    .matches = g_e1000e_matches,
    .probe   = e1000e_probe,
    .remove  = 0,
};

void e1000e_register(void) {
    pci_register_driver(&g_e1000e_driver);
}
