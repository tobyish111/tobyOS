/* nic_rtl8168.c -- Realtek RTL8168/8111/8136 NIC driver.
 *
 * Separate driver from rtl8169.c -- this handles the PCIe variants
 * with device IDs 0x8168 (RTL8168/8111 gigabit) and 0x8136 (RTL8101E
 * fast ethernet). Uses MMIO BAR, DMA descriptor rings, PHY init.
 *
 * The init follows the RTL8168 datasheet with a conservative approach:
 * full software reset, PHY auto-negotiation restart, TX/RX C+ mode
 * descriptor rings with 256 entries each.
 */

#include <tobyos/net.h>
#include <tobyos/pci.h>
#include <tobyos/pmm.h>
#include <tobyos/eth.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/cpu.h>
#include <tobyos/spinlock.h>
#include <tobyos/irq.h>
#include <tobyos/apic.h>
#include <tobyos/pit.h>

/* ---- register offsets -------------------------------------------- */

#define R8168_IDR0          0x00
#define R8168_IDR4          0x04
#define R8168_MAR0          0x08
#define R8168_TNPDS         0x20
#define R8168_CR            0x37
#define R8168_TPPOLL        0x38
#define R8168_IMR           0x3C
#define R8168_ISR           0x3E
#define R8168_TCR           0x40
#define R8168_RCR           0x44
#define R8168_9346CR        0x50
#define R8168_PHYAR         0x60
#define R8168_PHY_STATUS    0x6C
#define R8168_RMS           0xDA
#define R8168_CPCR          0xE0
#define R8168_RDSAR         0xE4
#define R8168_ETTHR         0xEC

#define CR_RST              (1u << 4)
#define CR_TE               (1u << 2)
#define CR_RE               (1u << 3)

#define R9346_EEM_UNLOCK    0xC0
#define R9346_EEM_LOCK      0x00

#define RCR_AAP             (1u << 0)
#define RCR_APM             (1u << 1)
#define RCR_AM              (1u << 2)
#define RCR_AB              (1u << 3)
#define RCR_MXDMA_UNLIM     (7u << 8)
#define RCR_RXFTH_NONE      (7u << 13)

#define TCR_IFG96           (3u << 24)
#define TCR_MXDMA_UNLIM     (7u << 8)

#define CPCR_RXCHKSUM       (1u << 5)
#define CPCR_MULRW          (1u << 3)

#define PHYAR_RW_FLAG       (1u << 31)

#define PHY_STATUS_LINK     (1u << 1)

/* ---- descriptor layout (C+ mode) -------------------------------- */

#define R8168_DESC_OWN      (1u << 31)
#define R8168_DESC_EOR      (1u << 30)
#define R8168_DESC_FS       (1u << 29)
#define R8168_DESC_LS       (1u << 28)
#define R8168_DESC_LEN_MASK 0x3FFFu

struct __attribute__((packed)) r8168_desc {
    uint32_t opts1;
    uint32_t opts2;
    uint64_t addr;
};

/* ---- sizing ------------------------------------------------------ */

#define R8168_RX_COUNT    256
#define R8168_TX_COUNT    256
#define R8168_BUF_SIZE    2048
#define R8168_MMIO_SIZE   256

/* ---- driver state ------------------------------------------------ */

static volatile uint8_t  *g_r8168_mmio;
static struct r8168_desc *g_r8168_rx_ring;
static struct r8168_desc *g_r8168_tx_ring;
static uint64_t           g_r8168_rx_ring_phys;
static uint64_t           g_r8168_tx_ring_phys;
static uint8_t           *g_r8168_rx_bufs[R8168_RX_COUNT];
static uint8_t           *g_r8168_tx_bufs[R8168_TX_COUNT];
static uint64_t           g_r8168_rx_bufs_phys[R8168_RX_COUNT];
static uint64_t           g_r8168_tx_bufs_phys[R8168_TX_COUNT];
static uint16_t           g_r8168_rx_idx;
static uint16_t           g_r8168_tx_idx;
static char               g_r8168_name[32];
static bool               g_r8168_up;

static spinlock_t g_r8168_rx_lock = SPINLOCK_INIT;

/* ---- MMIO helpers ------------------------------------------------ */

static inline void r8168_w8(uint32_t off, uint8_t val) {
    *(volatile uint8_t *)(g_r8168_mmio + off) = val;
}
static inline void r8168_w16(uint32_t off, uint16_t val) {
    *(volatile uint16_t *)(g_r8168_mmio + off) = val;
}
static inline void r8168_w32(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(g_r8168_mmio + off) = val;
}
static inline uint8_t r8168_r8(uint32_t off) {
    return *(volatile uint8_t *)(g_r8168_mmio + off);
}
static inline uint16_t r8168_r16(uint32_t off) {
    return *(volatile uint16_t *)(g_r8168_mmio + off);
}
static inline uint32_t r8168_r32(uint32_t off) {
    return *(volatile uint32_t *)(g_r8168_mmio + off);
}

/* ---- PHY access -------------------------------------------------- */

static uint16_t __attribute__((unused)) r8168_phy_read(uint8_t reg) {
    r8168_w32(R8168_PHYAR, (uint32_t)reg << 16);
    for (int i = 0; i < 2000; i++) {
        pit_sleep_ms(1);
        uint32_t v = r8168_r32(R8168_PHYAR);
        if (v & PHYAR_RW_FLAG)
            return (uint16_t)(v & 0xFFFF);
    }
    return 0xFFFF;
}

static void r8168_phy_write(uint8_t reg, uint16_t val) {
    r8168_w32(R8168_PHYAR, PHYAR_RW_FLAG | ((uint32_t)reg << 16) | val);
    for (int i = 0; i < 2000; i++) {
        pit_sleep_ms(1);
        if (!(r8168_r32(R8168_PHYAR) & PHYAR_RW_FLAG))
            return;
    }
}

/* ---- buffer allocation ------------------------------------------- */

static bool r8168_alloc_buf(uint8_t **virt, uint64_t *phys) {
    uint64_t p = pmm_alloc_page();
    if (!p) return false;
    *phys = p;
    *virt = (uint8_t *)pmm_phys_to_virt(p);
    memset(*virt, 0, R8168_BUF_SIZE);
    return true;
}

/* ---- ring setup -------------------------------------------------- */

static bool r8168_setup_rx(void) {
    uint64_t pages = (R8168_RX_COUNT * sizeof(struct r8168_desc) + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) { kprintf("[r8168] OOM: RX ring\n"); return false; }
    g_r8168_rx_ring_phys = phys;
    g_r8168_rx_ring = (struct r8168_desc *)pmm_phys_to_virt(phys);
    memset(g_r8168_rx_ring, 0, pages * PAGE_SIZE);

    for (int i = 0; i < R8168_RX_COUNT; i++) {
        if (!r8168_alloc_buf(&g_r8168_rx_bufs[i], &g_r8168_rx_bufs_phys[i])) {
            kprintf("[r8168] OOM: RX buf %d\n", i);
            return false;
        }
        g_r8168_rx_ring[i].addr  = g_r8168_rx_bufs_phys[i];
        g_r8168_rx_ring[i].opts1 = R8168_DESC_OWN | R8168_BUF_SIZE;
        if (i == R8168_RX_COUNT - 1)
            g_r8168_rx_ring[i].opts1 |= R8168_DESC_EOR;
    }
    g_r8168_rx_idx = 0;

    /* Write RX descriptor start address (64-bit) */
    r8168_w32(R8168_RDSAR,     (uint32_t)(g_r8168_rx_ring_phys & 0xFFFFFFFF));
    r8168_w32(R8168_RDSAR + 4, (uint32_t)(g_r8168_rx_ring_phys >> 32));
    r8168_w16(R8168_RMS, R8168_BUF_SIZE);
    return true;
}

static bool r8168_setup_tx(void) {
    uint64_t pages = (R8168_TX_COUNT * sizeof(struct r8168_desc) + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) { kprintf("[r8168] OOM: TX ring\n"); return false; }
    g_r8168_tx_ring_phys = phys;
    g_r8168_tx_ring = (struct r8168_desc *)pmm_phys_to_virt(phys);
    memset(g_r8168_tx_ring, 0, pages * PAGE_SIZE);

    for (int i = 0; i < R8168_TX_COUNT; i++) {
        if (!r8168_alloc_buf(&g_r8168_tx_bufs[i], &g_r8168_tx_bufs_phys[i])) {
            kprintf("[r8168] OOM: TX buf %d\n", i);
            return false;
        }
        g_r8168_tx_ring[i].addr  = g_r8168_tx_bufs_phys[i];
        g_r8168_tx_ring[i].opts1 = 0;
        if (i == R8168_TX_COUNT - 1)
            g_r8168_tx_ring[i].opts1 |= R8168_DESC_EOR;
    }
    g_r8168_tx_idx = 0;

    r8168_w32(R8168_TNPDS,     (uint32_t)(g_r8168_tx_ring_phys & 0xFFFFFFFF));
    r8168_w32(R8168_TNPDS + 4, (uint32_t)(g_r8168_tx_ring_phys >> 32));
    return true;
}

/* ---- MAC --------------------------------------------------------- */

static void r8168_read_mac(uint8_t mac[ETH_ADDR_LEN]) {
    uint32_t lo = r8168_r32(R8168_IDR0);
    uint16_t hi = r8168_r16(R8168_IDR4);
    mac[0] = (uint8_t)(lo);
    mac[1] = (uint8_t)(lo >> 8);
    mac[2] = (uint8_t)(lo >> 16);
    mac[3] = (uint8_t)(lo >> 24);
    mac[4] = (uint8_t)(hi);
    mac[5] = (uint8_t)(hi >> 8);
}

/* ---- TX / RX ----------------------------------------------------- */

static bool r8168_tx_op(struct net_dev *dev, const void *frame, size_t len) {
    (void)dev;
    if (!len || len > R8168_BUF_SIZE) return false;
    uint16_t i = g_r8168_tx_idx;

    if (g_r8168_tx_ring[i].opts1 & R8168_DESC_OWN) return false;

    memcpy(g_r8168_tx_bufs[i], frame, len);
    uint32_t opts = R8168_DESC_OWN | R8168_DESC_FS | R8168_DESC_LS | (uint32_t)len;
    if (i == R8168_TX_COUNT - 1) opts |= R8168_DESC_EOR;
    g_r8168_tx_ring[i].opts1 = opts;

    g_r8168_tx_idx = (i + 1) % R8168_TX_COUNT;
    r8168_w8(R8168_TPPOLL, 0x40);
    return true;
}

static void r8168_rx_drain(struct net_dev *dev) {
    (void)dev;
    uint64_t flags = spin_lock_irqsave(&g_r8168_rx_lock);
    for (int budget = 64; budget > 0; budget--) {
        uint16_t i = g_r8168_rx_idx;
        uint32_t opts1 = g_r8168_rx_ring[i].opts1;
        if (opts1 & R8168_DESC_OWN) break;

        uint16_t len = (uint16_t)(opts1 & R8168_DESC_LEN_MASK);
        bool fs = (opts1 & R8168_DESC_FS) != 0;
        bool ls = (opts1 & R8168_DESC_LS) != 0;
        if (fs && ls && len > 0 && len <= R8168_BUF_SIZE) {
            eth_recv(g_r8168_rx_bufs[i], len);
        }

        uint32_t new_opts = R8168_DESC_OWN | R8168_BUF_SIZE;
        if (i == R8168_RX_COUNT - 1) new_opts |= R8168_DESC_EOR;
        g_r8168_rx_ring[i].opts1 = new_opts;

        g_r8168_rx_idx = (i + 1) % R8168_RX_COUNT;
    }
    spin_unlock_irqrestore(&g_r8168_rx_lock, flags);
}

static bool r8168_link_up(struct net_dev *dev) {
    (void)dev;
    return g_r8168_up && (r8168_r8(R8168_PHY_STATUS) & PHY_STATUS_LINK);
}

/* ---- net_dev instance -------------------------------------------- */

static struct net_dev g_r8168_ndev;

/* ---- PCI probe --------------------------------------------------- */

static int r8168_probe(struct pci_dev *dev) {
    kprintf("[r8168] probing %02x:%02x.%x vendor=%04x device=%04x\n",
            dev->bus, dev->slot, dev->fn, dev->vendor, dev->device);

    pci_dev_enable(dev, PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);

    g_r8168_mmio = (volatile uint8_t *)pci_map_bar(dev, 2, R8168_MMIO_SIZE);
    if (!g_r8168_mmio) {
        /* Fall back to BAR0 for some variants */
        g_r8168_mmio = (volatile uint8_t *)pci_map_bar(dev, 0, R8168_MMIO_SIZE);
    }
    if (!g_r8168_mmio) {
        kprintf("[r8168] failed to map MMIO BAR\n");
        return -1;
    }

    /* Software reset */
    r8168_w8(R8168_CR, CR_RST);
    for (int i = 0; i < 1000; i++) {
        if (!(r8168_r8(R8168_CR) & CR_RST)) break;
        pit_sleep_ms(1);
    }

    /* Unlock config registers */
    r8168_w8(R8168_9346CR, R9346_EEM_UNLOCK);

    /* Disable interrupts */
    r8168_w16(R8168_IMR, 0);
    r8168_w16(R8168_ISR, 0xFFFF);

    /* C+ mode command */
    r8168_w16(R8168_CPCR, CPCR_MULRW);

    uint8_t mac[ETH_ADDR_LEN];
    r8168_read_mac(mac);
    kprintf("[r8168] MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    if (!r8168_setup_rx()) return -1;
    if (!r8168_setup_tx()) return -1;

    /* RX config: accept broadcast, multicast, physical match, unlimited DMA */
    r8168_w32(R8168_RCR, RCR_APM | RCR_AM | RCR_AB | RCR_MXDMA_UNLIM | RCR_RXFTH_NONE);
    /* TX config */
    r8168_w32(R8168_TCR, TCR_IFG96 | TCR_MXDMA_UNLIM);
    /* Early TX threshold */
    r8168_w8(R8168_ETTHR, 0x3F);

    /* PHY init: restart auto-negotiation */
    r8168_phy_write(0x00, 0x9000);
    pit_sleep_ms(100);

    /* Enable TX + RX */
    r8168_w8(R8168_CR, CR_TE | CR_RE);

    /* Lock config registers */
    r8168_w8(R8168_9346CR, R9346_EEM_LOCK);

    ksnprintf(g_r8168_name, sizeof(g_r8168_name), "r8168:%02x:%02x.%x",
              dev->bus, dev->slot, dev->fn);

    memset(&g_r8168_ndev, 0, sizeof(g_r8168_ndev));
    g_r8168_ndev.name     = g_r8168_name;
    memcpy(g_r8168_ndev.mac, mac, ETH_ADDR_LEN);
    g_r8168_ndev.tx       = r8168_tx_op;
    g_r8168_ndev.rx_drain = r8168_rx_drain;
    g_r8168_ndev.link_up  = r8168_link_up;
    net_register(&g_r8168_ndev);

    g_r8168_up = true;
    kprintf("[r8168] %s ready (link %s)\n", g_r8168_name,
            (r8168_r8(R8168_PHY_STATUS) & PHY_STATUS_LINK) ? "up" : "down");
    return 0;
}

/* ---- PCI driver registration ------------------------------------- */

static const struct pci_match r8168_match_table[] = {
    { 0x10EC, 0x8168, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    { 0x10EC, 0x8136, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS },
    PCI_MATCH_END
};

static struct pci_driver r8168_driver = {
    .name    = "r8168",
    .matches = r8168_match_table,
    .probe   = r8168_probe,
    .remove  = NULL,
};

void r8168_register(void) {
    pci_register_driver(&r8168_driver);
}
