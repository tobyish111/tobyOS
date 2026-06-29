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
#include <tobyos/spinlock.h>
#include <tobyos/irq.h>
#include <tobyos/apic.h>
#include <tobyos/pit.h>

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
#define E1000E_TXDCTL     0x3828          /* TX descriptor control, queue 0 */
#define TXDCTL_ENABLE     (1u << 25)      /* per-queue enable (82575+/PCH-LOM) */
#define E1000E_MTA_BASE   0x5200          /* 128 dwords */
#define E1000E_RAL0       0x5400
#define E1000E_RAH0       0x5404

/* Statistics registers (clear-on-read). Used by e1000e_dump_stats to tell
 * a TX failure (GPTC/TPT stay 0 -> nothing left the MAC) apart from an RX
 * failure (GPTC>0 but GPRC/TPR==0 -> we transmit but receive nothing) when
 * DHCP can't get a lease on real hardware. */
#define E1000E_CRCERRS    0x4000          /* CRC error count        */
#define E1000E_MPC        0x4010          /* missed packets (RX no buf) */
#define E1000E_GPRC       0x4074          /* good packets received  */
#define E1000E_GPTC       0x4080          /* good packets transmitted */
#define E1000E_TPR        0x40D0          /* total packets received */
#define E1000E_TPT        0x40D4          /* total packets transmitted */

/* CTRL bits. */
#define CTRL_RST         (1u << 26)
#define CTRL_ASDE        (1u << 5)
#define CTRL_SLU         (1u << 6)
#define CTRL_PHY_RST     (1u << 31)       /* e1000e: cleared after RST */
#define CTRL_LRST        (1u << 3)        /* link reset, must be 0 */
#define CTRL_GIO_MASTER_DISABLE (1u << 2) /* quiesce PCIe master before reset */

/* STATUS bits (offset 0x0008) -- used to report link state at probe so a
 * failed DHCP can be told apart from a down PHY link (esp. on PCH-LOM
 * parts where CTRL.SLU does not by itself guarantee copper link). */
#define STATUS_FD          (1u << 0)       /* full duplex */
#define STATUS_LU          (1u << 1)       /* link up */
#define STATUS_GIO_MASTER_EN (1u << 19)    /* PCIe master enabled (DMA in flight) */
#define STATUS_SPEED_SHIFT 6
#define STATUS_SPEED_MASK  (3u << 6)       /* 00=10 01=100 10/11=1000 Mb/s */

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

static spinlock_t g_e1000e_rx_lock = SPINLOCK_INIT;

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

/* After this many consecutive ring-full failures we latch the TX engine
 * "presumed dead" and stop spinning/logging on every frame. */
#define E1000E_TX_DEAD_THRESH 8

static bool e1000e_tx_op(struct net_dev *dev, const void *frame, size_t len) {
    (void)dev;
    if (len == 0 || len > BUF_SIZE) return false;

    /* Dead-TX backoff. On hardware where the TX engine never completes a
     * descriptor (e.g. the I217-LM PCH LOM on the EliteDesk: TXDCTL.ENABLE is
     * set but GPTC/TPT stay 0 -- see real-hardware-elitedesk-bringup), every
     * send would otherwise spin 100000 iterations and then log "ring full",
     * flooding the serial log and burning the BKL on each frame. After a run
     * of consecutive ring-full failures we latch the engine "presumed dead":
     * log ONCE, then fail-fast (no spin) on every subsequent send. Observing
     * any TXD_STAT_DD completion clears the latch, so a merely transient stall
     * is never permanently masked. This is purely a log/back-off change -- it
     * does not attempt to fix the I217 TX silicon problem. */
    static int  s_txfail_run;       /* consecutive ring-full failures */
    static bool s_tx_dead;          /* latched: skip the spin entirely */

    uint16_t i = g_tx_tail;
    bool dd;

    if (s_tx_dead) {
        /* Fail-fast: a single cheap probe instead of the 100000-iter spin. */
        dd = (g_tx_ring[i].status & TXD_STAT_DD) != 0;
        if (!dd) return false;
        kprintf("[e1000e] tx: engine recovered, resuming\n");
    } else {
        for (int spin = 0; spin < 100000; spin++) {
            if (g_tx_ring[i].status & TXD_STAT_DD) break;
        }
        dd = (g_tx_ring[i].status & TXD_STAT_DD) != 0;
        if (!dd) {
            if (++s_txfail_run >= E1000E_TX_DEAD_THRESH) {
                s_tx_dead = true;
                kprintf("[e1000e] tx: engine presumed dead, backing off\n");
            } else {
                kprintf("[e1000e] tx: ring full at idx %u\n", i);
            }
            return false;
        }
    }

    /* A descriptor completed -> the TX engine is alive; clear the latch. */
    s_tx_dead = false;
    s_txfail_run = 0;

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
    uint64_t irqf = spin_lock_irqsave(&g_e1000e_rx_lock);
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
    spin_unlock_irqrestore(&g_e1000e_rx_lock, irqf);
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

/* Dump the MAC's hardware packet counters. Called after the boot-time
 * DHCP window so a failed lease can be diagnosed at the silicon level:
 *   - link DOWN              -> PHY/cable problem
 *   - link UP, TX good == 0  -> our transmit path never put a frame on the
 *                               wire (descriptor/TCTL/doorbell issue)
 *   - TX good > 0, RX good 0 -> we transmit but receive nothing (RX ring /
 *                               filter / RCTL issue, or replies not arriving)
 *   - both > 0 but no lease  -> higher-level (ARP/UDP/DHCP) problem
 * The counters are clear-on-read, so this reflects traffic since the last
 * call. Safe no-op if the driver never bound. */
void e1000e_dump_stats(const char *when) {
    if (!g_mmio) return;
    uint32_t status = mmio_read32(E1000E_STATUS);
    uint32_t gptc = mmio_read32(E1000E_GPTC);
    uint32_t gprc = mmio_read32(E1000E_GPRC);
    uint32_t tpt  = mmio_read32(E1000E_TPT);
    uint32_t tpr  = mmio_read32(E1000E_TPR);
    uint32_t crc  = mmio_read32(E1000E_CRCERRS);
    uint32_t mpc  = mmio_read32(E1000E_MPC);
    kprintf("[e1000e] stats(%s): link=%s TX(good=%u tot=%u) RX(good=%u tot=%u) "
            "err(crc=%u miss=%u) irqs=%lu\n",
            when ? when : "?",
            (status & STATUS_LU) ? "UP" : "DOWN",
            (unsigned)gptc, (unsigned)tpt, (unsigned)gprc, (unsigned)tpr,
            (unsigned)crc, (unsigned)mpc, (unsigned long)g_irq_count);

    /* TX-engine register snapshot. With TX good/tot == 0 above, this pins
     * down WHY the MAC fetches no descriptors: is TCTL.EN actually set? is
     * the queue enabled (TXDCTL bit 25)? is TDH stuck at 0 while software
     * advanced TDT (HW not consuming the ring)? CTRL.SLU/STATUS.LU/TXOFF
     * (CTRL bit 22) reveal a link/transmit-paused gate. Pure reads. */
    kprintf("[e1000e]   regs: CTRL=0x%08x STATUS=0x%08x RCTL=0x%08x TCTL=0x%08x "
            "TDH=%u TDT=%u TXDCTL=0x%08x\n",
            (unsigned)mmio_read32(E1000E_CTRL), (unsigned)status,
            (unsigned)mmio_read32(E1000E_RCTL), (unsigned)mmio_read32(E1000E_TCTL),
            (unsigned)mmio_read32(E1000E_TDH), (unsigned)mmio_read32(E1000E_TDT),
            (unsigned)mmio_read32(E1000E_TXDCTL));
}

/* ----- net_dev publication --------------------------------------- */

static struct net_dev g_e1000e_dev = {
    .name     = g_e1000e_name,
    .priv     = 0,
    .tx       = e1000e_tx_op,
    .rx_drain = e1000e_rx_drain_op,
};

/* ----- PCH-integrated LAN detection ------------------------------ */

/* Intel's PCH-integrated LAN parts (the "ich8lan" family in Linux
 * terms -- 82577/82579 and every I217/I218/I219 onward) share the
 * 8254x register layout for basic RX/TX, but a software-issued device
 * reset (CTRL.RST) is NOT safe on them the way it is on the discrete
 * 82573/82574/82583 PCIe cards. On these LOM parts the MAC sits behind
 * the Management Engine and is wired to the PHY over an internal link
 * that the ME also drives; issuing CTRL.RST without first taking the
 * SW/FW semaphore (EXTCNF_CTRL.SWFLAG) and walking the PCIe-master-
 * disable + ULP/K1 exit sequence wedges that internal MAC<->PHY bus,
 * and the very next CSR read then stalls the CPU forever -- the read
 * completion never arrives, so it is NOT a master-abort that returns
 * 0xFFFFFFFF, it is a hard hang.
 *
 * That is exactly the failure on an HP EliteDesk 800 G1's onboard
 * I217-LM (8086:153A): the boot stops right after BAR0 is mapped, at
 * the first post-reset register read inside the reset-wait loop.
 *
 * The BIOS has already fully initialized these MACs by the time we
 * probe, so we simply DON'T reset them -- we read the MAC out of
 * RAL/RAH and program the rings on top of the firmware-configured MAC.
 * Only the discrete parts (and QEMU's emulated 82574L, did 0x10D3,
 * which `make run-e1000e` boots) take the original reset path, so the
 * emulated test target is byte-for-byte unchanged. */
static bool e1000e_is_pch_lan(uint16_t did) {
    switch (did) {
    case 0x10D3:   /* 82574L -- discrete, QEMU `-device e1000e`        */
    case 0x10F6:   /* 82573 family discrete PCIe                       */
    case 0x150C:   /* 82583 family discrete PCIe                       */
        return false;
    default:       /* 82577/82579 + all I217/I218/I219 LOM: no reset   */
        return true;
    }
}

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

    /* 2. Reset the MAC.
     *
     * Discrete parts (82574L/QEMU): a bare CTRL.RST is fine.
     *
     * PCH-integrated LOM parts (I217/I218/I219): a BARE CTRL.RST hangs the
     * CPU -- the wait-loop MMIO read never returns -- because it is issued
     * while the PCIe master is still enabled with DMA in flight. We
     * previously worked around that by SKIPPING the reset, but that left
     * the transmit DMA engine dead (TDH stuck at 0 while TDT advanced, TX
     * good=0, even with TCTL.EN + TXDCTL.ENABLE set), so DHCP never got a
     * frame onto the wire. The documented safe sequence fixes BOTH: quiesce
     * RX/TX, disable the GIO master and wait for STATUS.GIO_MASTER_EN to
     * clear (drains in-flight DMA so the reset can't wedge the internal
     * bus), THEN issue CTRL.RST. This is exactly what Linux's e1000e does
     * on these parts. Bounded, PIT-timed waits throughout. */
    if (e1000e_is_pch_lan(dev->device)) {
        mmio_write32(E1000E_RCTL, 0);
        mmio_write32(E1000E_TCTL, 0);
        mmio_write32(E1000E_CTRL,
                     mmio_read32(E1000E_CTRL) | CTRL_GIO_MASTER_DISABLE);
        for (int i = 0; i < 1000; i++) {
            if ((mmio_read32(E1000E_STATUS) & STATUS_GIO_MASTER_EN) == 0) break;
            pit_sleep_ms(1);
        }
        mmio_write32(E1000E_CTRL, mmio_read32(E1000E_CTRL) | CTRL_RST);
        pit_sleep_ms(5);
        for (int i = 0; i < 1000; i++) {
            if ((mmio_read32(E1000E_CTRL) & CTRL_RST) == 0) break;
            pit_sleep_ms(1);
        }
        kprintf("[e1000e] PCH safe reset done (did %04x, CTRL=0x%08x "
                "STATUS=0x%08x)\n", (unsigned)dev->device,
                (unsigned)mmio_read32(E1000E_CTRL),
                (unsigned)mmio_read32(E1000E_STATUS));
    } else {
        mmio_write32(E1000E_CTRL, mmio_read32(E1000E_CTRL) | CTRL_RST);
        for (int i = 0; i < 1000000; i++) {
            if ((mmio_read32(E1000E_CTRL) & CTRL_RST) == 0) break;
        }
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

    /* On PCH-integrated LOM parts (I217/I218/I219) the legacy TCTL.EN bit
     * is NOT sufficient to start the transmit DMA queue -- the per-queue
     * TXDCTL.ENABLE (bit 25, the 82575+/igb-style enable) must also be set,
     * or the MAC silently fetches no TX descriptors: GPTC/TPT stay 0 while
     * RX works fine. That is exactly the e1000e_dump_stats signature seen on
     * the EliteDesk I217 (link UP, RX good=106, TX good=0), which made every
     * DHCP DISCOVER die before reaching the wire. The discrete 82574 (QEMU)
     * does not have/need this gate, so it stays on the plain TCTL.EN path and
     * the emulated target is unchanged. */
    if (e1000e_is_pch_lan(dev->device)) {
        mmio_write32(E1000E_TXDCTL, mmio_read32(E1000E_TXDCTL) | TXDCTL_ENABLE);
        for (int i = 0; i < 1000; i++) {
            if (mmio_read32(E1000E_TXDCTL) & TXDCTL_ENABLE) break;
        }
        kprintf("[e1000e] PCH TX queue enabled (TXDCTL=0x%08x)\n",
                mmio_read32(E1000E_TXDCTL));
    }

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

    /* 7. Wait (bounded) for the PHY to bring the copper link up, then
     * report it. DHCP runs shortly after this probe, so giving the link a
     * moment to settle makes the boot-time lease more reliable; logging the
     * final state means a DHCP failure can be diagnosed (link DOWN => PHY
     * problem; link UP => look at TX/RX) instead of guessed at. ~2s cap so
     * an unplugged port only costs a brief delay. */
    uint32_t status = mmio_read32(E1000E_STATUS);
    for (int i = 0; i < 200 && !(status & STATUS_LU); i++) {
        pit_sleep_ms(10);
        status = mmio_read32(E1000E_STATUS);
    }
    static const char *const spd[4] = { "10Mb/s", "100Mb/s", "1Gb/s", "1Gb/s" };
    kprintf("[e1000e] link %s (STATUS=0x%08x speed=%s duplex=%s)\n",
            (status & STATUS_LU) ? "UP" : "DOWN", status,
            spd[(status & STATUS_SPEED_MASK) >> STATUS_SPEED_SHIFT],
            (status & STATUS_FD) ? "full" : "half");

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
