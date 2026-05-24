/* nic_igb.c -- Intel I210/I219/I225 (igb family) NIC driver.
 *
 * PCI driver-registry bound. Matches vendor 0x8086 with device IDs
 * for I210 (0x1533), I219 (0x15B8), and I225 (0x15F3). BAR0 MMIO,
 * DMA descriptor rings for RX/TX, MSI interrupt support.
 *
 * The init sequence follows the Intel I210 datasheet §4.5 (software
 * init) with conservative delays suitable for all three variants.
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

/* ---- register offsets (BAR0 MMIO) -------------------------------- */

#define IGB_CTRL          0x0000
#define IGB_STATUS        0x0008
#define IGB_CTRL_EXT      0x0018
#define IGB_ICR           0x00C0
#define IGB_IMS           0x00D0
#define IGB_IMC           0x00D8
#define IGB_RCTL          0x0100
#define IGB_TCTL          0x0400
#define IGB_TIPG          0x0410
#define IGB_RDBAL0        0x2800
#define IGB_RDBAH0        0x2804
#define IGB_RDLEN0        0x2808
#define IGB_RDH0          0x2810
#define IGB_RDT0          0x2818
#define IGB_TDBAL0        0x3800
#define IGB_TDBAH0        0x3804
#define IGB_TDLEN0        0x3808
#define IGB_TDH0          0x3810
#define IGB_TDT0          0x3818
#define IGB_RAL0          0x5400
#define IGB_RAH0          0x5404
#define IGB_MTA_BASE      0x5200

#define IGB_CTRL_RST      (1u << 26)
#define IGB_CTRL_SLU      (1u << 6)
#define IGB_CTRL_ASDE     (1u << 5)

#define IGB_RCTL_EN       (1u << 1)
#define IGB_RCTL_BAM      (1u << 15)
#define IGB_RCTL_UPE      (1u << 3)
#define IGB_RCTL_SECRC    (1u << 26)
#define IGB_RCTL_BSIZE_2K 0u

#define IGB_TCTL_EN       (1u << 1)
#define IGB_TCTL_PSP      (1u << 3)
#define IGB_TCTL_CT_SHIFT 4
#define IGB_TCTL_COLD_SHIFT 12

#define IGB_TXD_CMD_EOP   (1u << 0)
#define IGB_TXD_CMD_IFCS  (1u << 1)
#define IGB_TXD_CMD_RS    (1u << 3)
#define IGB_TXD_STAT_DD   (1u << 0)

#define IGB_RXD_STAT_DD   (1u << 0)
#define IGB_RXD_STAT_EOP  (1u << 1)

#define IGB_RAH_AV        (1u << 31)

#define IGB_IMS_TXDW      (1u << 0)
#define IGB_IMS_LSC       (1u << 2)
#define IGB_IMS_RXDMT     (1u << 4)
#define IGB_IMS_RXT0      (1u << 7)
#define IGB_IMS_BITS      (IGB_IMS_TXDW | IGB_IMS_LSC | IGB_IMS_RXDMT | IGB_IMS_RXT0)

#define IGB_STATUS_LU     (1u << 1)

/* ---- ring sizes -------------------------------------------------- */

#define IGB_RX_COUNT      256
#define IGB_TX_COUNT      256
#define IGB_BUF_SIZE      2048
#define IGB_MMIO_SIZE     (128u * 1024u)

/* ---- hardware descriptors ---------------------------------------- */

struct __attribute__((packed)) igb_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
};

struct __attribute__((packed)) igb_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
};

/* ---- driver state ------------------------------------------------ */

static volatile uint8_t     *g_igb_mmio;
static struct igb_rx_desc   *g_igb_rx_ring;
static struct igb_tx_desc   *g_igb_tx_ring;
static uint64_t              g_igb_rx_ring_phys;
static uint64_t              g_igb_tx_ring_phys;
static uint8_t              *g_igb_rx_bufs[IGB_RX_COUNT];
static uint8_t              *g_igb_tx_bufs[IGB_TX_COUNT];
static uint64_t              g_igb_rx_bufs_phys[IGB_RX_COUNT];
static uint64_t              g_igb_tx_bufs_phys[IGB_TX_COUNT];
static uint16_t              g_igb_rx_tail;
static uint16_t              g_igb_tx_tail;
static char                  g_igb_name[32];
static uint8_t               g_igb_irq_vector;
static volatile uint64_t     g_igb_irq_count;
static bool                  g_igb_up;

static spinlock_t g_igb_rx_lock = SPINLOCK_INIT;

/* ---- MMIO helpers ------------------------------------------------ */

static inline void igb_w32(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(g_igb_mmio + off) = val;
}
static inline uint32_t igb_r32(uint32_t off) {
    return *(volatile uint32_t *)(g_igb_mmio + off);
}

/* ---- buffer allocation ------------------------------------------- */

static bool igb_alloc_buf(uint8_t **virt, uint64_t *phys) {
    uint64_t p = pmm_alloc_page();
    if (!p) return false;
    *phys = p;
    *virt = (uint8_t *)pmm_phys_to_virt(p);
    memset(*virt, 0, IGB_BUF_SIZE);
    return true;
}

/* ---- ring setup -------------------------------------------------- */

static bool igb_setup_rx(void) {
    uint64_t pages = (IGB_RX_COUNT * sizeof(struct igb_rx_desc) + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) { kprintf("[igb] OOM: RX ring\n"); return false; }
    g_igb_rx_ring_phys = phys;
    g_igb_rx_ring = (struct igb_rx_desc *)pmm_phys_to_virt(phys);
    memset(g_igb_rx_ring, 0, pages * PAGE_SIZE);

    for (int i = 0; i < IGB_RX_COUNT; i++) {
        if (!igb_alloc_buf(&g_igb_rx_bufs[i], &g_igb_rx_bufs_phys[i])) {
            kprintf("[igb] OOM: RX buf %d\n", i);
            return false;
        }
        g_igb_rx_ring[i].addr   = g_igb_rx_bufs_phys[i];
        g_igb_rx_ring[i].status = 0;
    }

    igb_w32(IGB_RDBAL0, (uint32_t)(g_igb_rx_ring_phys & 0xFFFFFFFF));
    igb_w32(IGB_RDBAH0, (uint32_t)(g_igb_rx_ring_phys >> 32));
    igb_w32(IGB_RDLEN0, IGB_RX_COUNT * (uint32_t)sizeof(struct igb_rx_desc));
    igb_w32(IGB_RDH0, 0);
    igb_w32(IGB_RDT0, IGB_RX_COUNT - 1);
    g_igb_rx_tail = IGB_RX_COUNT - 1;

    igb_w32(IGB_RCTL, IGB_RCTL_EN | IGB_RCTL_BAM | IGB_RCTL_UPE |
                       IGB_RCTL_SECRC | IGB_RCTL_BSIZE_2K);
    return true;
}

static bool igb_setup_tx(void) {
    uint64_t pages = (IGB_TX_COUNT * sizeof(struct igb_tx_desc) + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (!phys) { kprintf("[igb] OOM: TX ring\n"); return false; }
    g_igb_tx_ring_phys = phys;
    g_igb_tx_ring = (struct igb_tx_desc *)pmm_phys_to_virt(phys);
    memset(g_igb_tx_ring, 0, pages * PAGE_SIZE);

    for (int i = 0; i < IGB_TX_COUNT; i++) {
        if (!igb_alloc_buf(&g_igb_tx_bufs[i], &g_igb_tx_bufs_phys[i])) {
            kprintf("[igb] OOM: TX buf %d\n", i);
            return false;
        }
        g_igb_tx_ring[i].addr   = g_igb_tx_bufs_phys[i];
        g_igb_tx_ring[i].status = IGB_TXD_STAT_DD;
    }

    igb_w32(IGB_TDBAL0, (uint32_t)(g_igb_tx_ring_phys & 0xFFFFFFFF));
    igb_w32(IGB_TDBAH0, (uint32_t)(g_igb_tx_ring_phys >> 32));
    igb_w32(IGB_TDLEN0, IGB_TX_COUNT * (uint32_t)sizeof(struct igb_tx_desc));
    igb_w32(IGB_TDH0, 0);
    igb_w32(IGB_TDT0, 0);
    g_igb_tx_tail = 0;

    igb_w32(IGB_TCTL, IGB_TCTL_EN | IGB_TCTL_PSP |
                       (0x10u << IGB_TCTL_CT_SHIFT) |
                       (0x40u << IGB_TCTL_COLD_SHIFT));
    igb_w32(IGB_TIPG, 10u | (8u << 10) | (6u << 20));
    return true;
}

/* ---- MAC address ------------------------------------------------- */

static void igb_read_mac(uint8_t mac[ETH_ADDR_LEN]) {
    uint32_t lo = igb_r32(IGB_RAL0);
    uint32_t hi = igb_r32(IGB_RAH0);
    mac[0] = (uint8_t)(lo);
    mac[1] = (uint8_t)(lo >> 8);
    mac[2] = (uint8_t)(lo >> 16);
    mac[3] = (uint8_t)(lo >> 24);
    mac[4] = (uint8_t)(hi);
    mac[5] = (uint8_t)(hi >> 8);
}

/* ---- TX / RX ----------------------------------------------------- */

static bool igb_tx_op(struct net_dev *dev, const void *frame, size_t len) {
    (void)dev;
    if (!len || len > IGB_BUF_SIZE) return false;
    uint16_t i = g_igb_tx_tail;
    for (int spin = 0; spin < 100000; spin++) {
        if (g_igb_tx_ring[i].status & IGB_TXD_STAT_DD) break;
    }
    if (!(g_igb_tx_ring[i].status & IGB_TXD_STAT_DD)) return false;

    memcpy(g_igb_tx_bufs[i], frame, len);
    g_igb_tx_ring[i].length = (uint16_t)len;
    g_igb_tx_ring[i].cmd    = IGB_TXD_CMD_EOP | IGB_TXD_CMD_IFCS | IGB_TXD_CMD_RS;
    g_igb_tx_ring[i].status = 0;

    g_igb_tx_tail = (i + 1) % IGB_TX_COUNT;
    igb_w32(IGB_TDT0, g_igb_tx_tail);
    return true;
}

static void igb_rx_drain(struct net_dev *dev) {
    (void)dev;
    uint64_t flags = spin_lock_irqsave(&g_igb_rx_lock);
    for (int budget = 64; budget > 0; budget--) {
        uint16_t i = (g_igb_rx_tail + 1) % IGB_RX_COUNT;
        if (!(g_igb_rx_ring[i].status & IGB_RXD_STAT_DD)) break;

        uint16_t len = g_igb_rx_ring[i].length;
        if ((g_igb_rx_ring[i].status & IGB_RXD_STAT_EOP) && len <= IGB_BUF_SIZE) {
            eth_recv(g_igb_rx_bufs[i], len);
        }

        g_igb_rx_ring[i].status = 0;
        g_igb_rx_ring[i].length = 0;
        g_igb_rx_tail = i;
        igb_w32(IGB_RDT0, g_igb_rx_tail);
    }
    spin_unlock_irqrestore(&g_igb_rx_lock, flags);
}

static bool igb_link_up(struct net_dev *dev) {
    (void)dev;
    return g_igb_up && (igb_r32(IGB_STATUS) & IGB_STATUS_LU);
}

/* ---- IRQ handler ------------------------------------------------- */

static void igb_isr(void *ctx) {
    (void)ctx;
    uint32_t icr = igb_r32(IGB_ICR);
    (void)icr;
    g_igb_irq_count++;
}

/* ---- net_dev instance -------------------------------------------- */

static struct net_dev g_igb_ndev;

/* ---- PCI probe --------------------------------------------------- */

static int igb_probe(struct pci_dev *dev) {
    kprintf("[igb] probing %02x:%02x.%x vendor=%04x device=%04x\n",
            dev->bus, dev->slot, dev->fn, dev->vendor, dev->device);

    pci_dev_enable(dev, PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);

    g_igb_mmio = (volatile uint8_t *)pci_map_bar(dev, 0, IGB_MMIO_SIZE);
    if (!g_igb_mmio) {
        kprintf("[igb] failed to map BAR0\n");
        return -1;
    }

    /* Device reset */
    igb_w32(IGB_CTRL, igb_r32(IGB_CTRL) | IGB_CTRL_RST);
    pit_sleep_ms(20);
    for (int i = 0; i < 1000; i++) {
        if (!(igb_r32(IGB_CTRL) & IGB_CTRL_RST)) break;
        pit_sleep_ms(1);
    }

    /* Disable interrupts during init */
    igb_w32(IGB_IMC, 0xFFFFFFFF);
    igb_r32(IGB_ICR);

    /* Link setup */
    igb_w32(IGB_CTRL, igb_r32(IGB_CTRL) | IGB_CTRL_SLU | IGB_CTRL_ASDE);

    /* Clear MTA */
    for (int i = 0; i < 128; i++)
        igb_w32(IGB_MTA_BASE + (uint32_t)i * 4, 0);

    uint8_t mac[ETH_ADDR_LEN];
    igb_read_mac(mac);
    kprintf("[igb] MAC=%02x:%02x:%02x:%02x:%02x:%02x\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    if (!igb_setup_rx()) return -1;
    if (!igb_setup_tx()) return -1;

    /* Try MSI */
    g_igb_irq_vector = irq_alloc_vector(igb_isr, NULL);
    if (g_igb_irq_vector) {
        uint8_t lapic_id = (uint8_t)apic_read_id();
        if (pci_msi_enable(dev, g_igb_irq_vector, lapic_id)) {
            igb_w32(IGB_IMS, IGB_IMS_BITS);
            kprintf("[igb] MSI enabled, vector 0x%02x\n", g_igb_irq_vector);
        } else {
            g_igb_irq_vector = 0;
        }
    }

    ksnprintf(g_igb_name, sizeof(g_igb_name), "igb:%02x:%02x.%x",
              dev->bus, dev->slot, dev->fn);

    memset(&g_igb_ndev, 0, sizeof(g_igb_ndev));
    g_igb_ndev.name     = g_igb_name;
    memcpy(g_igb_ndev.mac, mac, ETH_ADDR_LEN);
    g_igb_ndev.tx       = igb_tx_op;
    g_igb_ndev.rx_drain = igb_rx_drain;
    g_igb_ndev.link_up  = igb_link_up;
    net_register(&g_igb_ndev);

    g_igb_up = true;
    kprintf("[igb] %s ready\n", g_igb_name);
    return 0;
}

/* ---- PCI driver registration ------------------------------------- */

static const struct pci_match igb_match_table[] = {
    { 0x8086, 0x1533, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS }, /* I210 */
    { 0x8086, 0x15B8, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS }, /* I219 */
    { 0x8086, 0x15F3, PCI_ANY_CLASS, PCI_ANY_CLASS, PCI_ANY_CLASS }, /* I225 */
    PCI_MATCH_END
};

static struct pci_driver igb_driver = {
    .name    = "igb",
    .matches = igb_match_table,
    .probe   = igb_probe,
    .remove  = NULL,
};

void igb_register(void) {
    pci_register_driver(&igb_driver);
}
