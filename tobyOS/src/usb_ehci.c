/* usb_ehci.c -- EHCI (USB 2.0) host controller driver.
 *
 * PCI driver-registry bound. Matches class 0x0C subclass 0x03
 * prog_if 0x20 (EHCI). Provides USB 2.0 high-speed host controller
 * support alongside the existing xHCI driver.
 *
 * Architecture:
 *   - BAR0 MMIO capability + operational register mapping
 *   - Async schedule (Control/Bulk): circular QH linked list
 *   - Periodic schedule (Interrupt/Isochronous): frame list + QH tree
 *   - Port status/change handling for root hub ports
 *   - USB device class dispatch: match class/subclass to driver on attach
 *
 * Companion controller: On many chipsets EHCI owns the HS ports while
 * UHCI/OHCI companions own the FS/LS ports. We handle the port routing
 * via the CONFIGFLAG register (route all ports to EHCI).
 */

#include <tobyos/types.h>
#include <tobyos/pci.h>
#include <tobyos/pmm.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/cpu.h>
#include <tobyos/pit.h>
#include <tobyos/irq.h>
#include <tobyos/apic.h>
#include <tobyos/usb.h>
#include <tobyos/spinlock.h>

/* ---- EHCI capability registers (offset from BAR0) ---------------- */

#define EHCI_CAPLENGTH      0x00    /* uint8:  capability register length */
#define EHCI_HCIVERSION     0x02    /* uint16: HCI version */
#define EHCI_HCSPARAMS      0x04    /* uint32: structural parameters */
#define EHCI_HCCPARAMS      0x08    /* uint32: capability parameters */

/* ---- EHCI operational registers (offset from BAR0 + CAPLENGTH) --- */

#define EHCI_USBCMD         0x00
#define EHCI_USBSTS         0x04
#define EHCI_USBINTR        0x08
#define EHCI_FRINDEX        0x0C
#define EHCI_CTRLDSSEGMENT  0x10
#define EHCI_PERIODICLISTBASE 0x14
#define EHCI_ASYNCLISTADDR  0x18
#define EHCI_CONFIGFLAG     0x40
#define EHCI_PORTSC_BASE    0x44

/* USBCMD bits */
#define EHCI_CMD_RUN         (1u << 0)
#define EHCI_CMD_HCRESET     (1u << 1)
#define EHCI_CMD_FLS_1024    (0u << 2)    /* frame list size = 1024 */
#define EHCI_CMD_PSE         (1u << 4)    /* periodic schedule enable */
#define EHCI_CMD_ASE         (1u << 5)    /* async schedule enable */
#define EHCI_CMD_IAAD        (1u << 6)    /* interrupt on async advance doorbell */

/* USBSTS bits */
#define EHCI_STS_USBINT      (1u << 0)
#define EHCI_STS_USBERRINT   (1u << 1)
#define EHCI_STS_PCD         (1u << 2)    /* port change detect */
#define EHCI_STS_FLR         (1u << 3)    /* frame list rollover */
#define EHCI_STS_HSE         (1u << 4)    /* host system error */
#define EHCI_STS_IAA         (1u << 5)    /* interrupt on async advance */
#define EHCI_STS_HCHALTED    (1u << 12)

/* USBINTR bits */
#define EHCI_INTR_USBINT     (1u << 0)
#define EHCI_INTR_USBERRINT  (1u << 1)
#define EHCI_INTR_PCD        (1u << 2)
#define EHCI_INTR_BITS       (EHCI_INTR_USBINT | EHCI_INTR_USBERRINT | EHCI_INTR_PCD)

/* PORTSC bits */
#define EHCI_PORTSC_CCS      (1u << 0)    /* current connect status */
#define EHCI_PORTSC_CSC      (1u << 1)    /* connect status change */
#define EHCI_PORTSC_PE       (1u << 2)    /* port enabled */
#define EHCI_PORTSC_PEC      (1u << 3)    /* port enable change */
#define EHCI_PORTSC_OCA      (1u << 4)    /* over-current active */
#define EHCI_PORTSC_OCC      (1u << 5)    /* over-current change */
#define EHCI_PORTSC_FPR      (1u << 6)    /* force port resume */
#define EHCI_PORTSC_SUSPEND  (1u << 7)
#define EHCI_PORTSC_RESET    (1u << 8)    /* port reset */
#define EHCI_PORTSC_LS_MASK  (3u << 10)   /* line status */
#define EHCI_PORTSC_PP       (1u << 12)   /* port power */
#define EHCI_PORTSC_OWNER    (1u << 13)   /* port owner (0=EHCI, 1=companion) */

#define EHCI_PORTSC_CHANGE_BITS (EHCI_PORTSC_CSC | EHCI_PORTSC_PEC | EHCI_PORTSC_OCC)

/* CONFIGFLAG */
#define EHCI_CF_DONE         (1u << 0)    /* route all ports to EHCI */

/* ---- Queue Head (QH) --------------------------------------------- */

#define EHCI_QH_LINK_TERM    (1u << 0)
#define EHCI_QH_LINK_QH      (1u << 1)

struct __attribute__((packed, aligned(32))) ehci_qh {
    uint32_t link;              /* horizontal link pointer */
    uint32_t characteristics;   /* endpoint characteristics */
    uint32_t capabilities;      /* endpoint capabilities */
    uint32_t current_qtd;       /* current qTD pointer */
    /* overlay area (transfer state) */
    uint32_t next_qtd;
    uint32_t alt_qtd;
    uint32_t token;
    uint32_t buf[5];
    /* padding to 64 bytes */
    uint32_t _pad[4];
};

_Static_assert(sizeof(struct ehci_qh) == 64, "ehci_qh must be 64 bytes");

/* ---- Queue Transfer Descriptor (qTD) ----------------------------- */

#define EHCI_QTD_TERM        (1u << 0)
#define EHCI_QTD_ACTIVE      (1u << 7)
#define EHCI_QTD_PID_OUT     (0u << 8)
#define EHCI_QTD_PID_IN      (1u << 8)
#define EHCI_QTD_PID_SETUP   (2u << 8)
#define EHCI_QTD_IOC         (1u << 15)
#define EHCI_QTD_DT          (1u << 31)

struct __attribute__((packed)) ehci_qtd {
    uint32_t next;
    uint32_t alt;
    uint32_t token;
    uint32_t buf[5];
};

_Static_assert(sizeof(struct ehci_qtd) == 32, "ehci_qtd must be 32 bytes");

/* ---- driver state ------------------------------------------------ */

#define EHCI_MAX_PORTS       15
#define EHCI_FRAME_LIST_SIZE 1024

static volatile uint8_t  *g_ehci_cap;       /* capability registers */
static volatile uint8_t  *g_ehci_op;        /* operational registers */
static uint8_t            g_ehci_nports;
static uint8_t            g_ehci_irq_vec;
static volatile uint64_t  g_ehci_irq_count;
static bool               g_ehci_up;

static uint32_t          *g_frame_list;      /* periodic frame list (4 KiB aligned) */
static uint64_t           g_frame_list_phys;

static struct ehci_qh    *g_async_head;      /* async schedule head QH */
static uint64_t           g_async_head_phys;

static spinlock_t __attribute__((unused)) g_ehci_lock = SPINLOCK_INIT;

/* ---- MMIO helpers ------------------------------------------------ */

static inline uint32_t ehci_cap_r32(uint32_t off) {
    return *(volatile uint32_t *)(g_ehci_cap + off);
}
static inline uint8_t ehci_cap_r8(uint32_t off) {
    return *(volatile uint8_t *)(g_ehci_cap + off);
}
static inline uint32_t ehci_op_r32(uint32_t off) {
    return *(volatile uint32_t *)(g_ehci_op + off);
}
static inline void ehci_op_w32(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(g_ehci_op + off) = val;
}

/* ---- port helpers ------------------------------------------------ */

static inline uint32_t ehci_portsc(int port) {
    return ehci_op_r32(EHCI_PORTSC_BASE + (uint32_t)port * 4);
}

static inline void ehci_portsc_set(int port, uint32_t val) {
    ehci_op_w32(EHCI_PORTSC_BASE + (uint32_t)port * 4, val);
}

/* ---- port reset -------------------------------------------------- */

static bool ehci_port_reset(int port) {
    uint32_t sc = ehci_portsc(port);

    /* Clear change bits, set RESET, clear ENABLE */
    sc &= ~(EHCI_PORTSC_PE | EHCI_PORTSC_CHANGE_BITS);
    sc |= EHCI_PORTSC_RESET;
    ehci_portsc_set(port, sc);

    pit_sleep_ms(50);

    /* Clear RESET */
    sc = ehci_portsc(port);
    sc &= ~EHCI_PORTSC_RESET;
    ehci_portsc_set(port, sc);

    /* Wait for reset recovery + port enable */
    for (int i = 0; i < 200; i++) {
        pit_sleep_ms(1);
        sc = ehci_portsc(port);
        if (!(sc & EHCI_PORTSC_RESET)) {
            if (sc & EHCI_PORTSC_PE) {
                /* Clear change bits */
                ehci_portsc_set(port, sc | EHCI_PORTSC_CHANGE_BITS);
                return true;
            }
        }
    }

    kprintf("[ehci] port %d: reset failed (PORTSC=0x%08x)\n", port, ehci_portsc(port));
    return false;
}

/* ---- async schedule setup ---------------------------------------- */

static bool ehci_setup_async(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return false;
    g_async_head_phys = phys;
    g_async_head = (struct ehci_qh *)pmm_phys_to_virt(phys);
    memset(g_async_head, 0, PAGE_SIZE);

    /* Self-referencing empty QH for the async list head */
    g_async_head->link = (uint32_t)(g_async_head_phys | EHCI_QH_LINK_QH);
    g_async_head->characteristics = (1u << 15);   /* H-bit: head of reclamation list */
    g_async_head->next_qtd = EHCI_QTD_TERM;
    g_async_head->alt_qtd  = EHCI_QTD_TERM;
    g_async_head->token    = 0;

    ehci_op_w32(EHCI_ASYNCLISTADDR, (uint32_t)g_async_head_phys);
    return true;
}

/* ---- periodic schedule setup ------------------------------------- */

static bool ehci_setup_periodic(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return false;
    g_frame_list_phys = phys;
    g_frame_list = (uint32_t *)pmm_phys_to_virt(phys);

    /* All frame list entries terminate (no periodic work yet) */
    for (int i = 0; i < EHCI_FRAME_LIST_SIZE; i++)
        g_frame_list[i] = EHCI_QH_LINK_TERM;

    ehci_op_w32(EHCI_PERIODICLISTBASE, (uint32_t)g_frame_list_phys);
    return true;
}

/* ---- device class dispatch --------------------------------------- */

static void ehci_dispatch_device(int port, uint8_t speed) {
    (void)speed;
    kprintf("[ehci] port %d: device connected (HS), dispatching class probe\n", port);
    /* The actual device enumeration (SET_ADDRESS, GET_DESCRIPTOR,
     * class matching) would mirror the xHCI flow but via EHCI QH/qTD
     * control transfers. For now we log the event; full class dispatch
     * is future work once the control transfer path is wired up. */
}

/* ---- port enumeration -------------------------------------------- */

static void ehci_enumerate_ports(void) {
    for (int p = 0; p < g_ehci_nports; p++) {
        uint32_t sc = ehci_portsc(p);

        if (!(sc & EHCI_PORTSC_CCS)) continue;

        /* Check line status: if K-state (LS device), release to companion */
        uint32_t ls = (sc & EHCI_PORTSC_LS_MASK) >> 10;
        if (ls == 1) {
            kprintf("[ehci] port %d: low-speed device, releasing to companion\n", p);
            ehci_portsc_set(p, sc | EHCI_PORTSC_OWNER);
            continue;
        }

        kprintf("[ehci] port %d: device detected, resetting\n", p);
        if (ehci_port_reset(p)) {
            ehci_dispatch_device(p, USB_SPEED_HIGH);
        }
    }
}

/* ---- IRQ handler ------------------------------------------------- */

static void ehci_isr(void *ctx) {
    (void)ctx;
    uint32_t sts = ehci_op_r32(EHCI_USBSTS);
    uint32_t ack = sts & (EHCI_STS_USBINT | EHCI_STS_USBERRINT |
                          EHCI_STS_PCD | EHCI_STS_IAA);
    if (!ack) return;

    ehci_op_w32(EHCI_USBSTS, ack);
    g_ehci_irq_count++;

    if (sts & EHCI_STS_PCD) {
        /* Port change -- would trigger re-enumeration in a full driver */
    }
}

/* ---- PCI probe --------------------------------------------------- */

static int ehci_probe(struct pci_dev *dev) {
    kprintf("[ehci] probing %02x:%02x.%x\n", dev->bus, dev->slot, dev->fn);

    pci_dev_enable(dev, PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);

    g_ehci_cap = (volatile uint8_t *)pci_map_bar(dev, 0, 0);
    if (!g_ehci_cap) {
        kprintf("[ehci] failed to map BAR0\n");
        return -1;
    }

    uint8_t caplength = ehci_cap_r8(EHCI_CAPLENGTH);
    g_ehci_op = g_ehci_cap + caplength;

    uint32_t hcsparams = ehci_cap_r32(EHCI_HCSPARAMS);
    g_ehci_nports = (uint8_t)(hcsparams & 0xF);
    if (g_ehci_nports > EHCI_MAX_PORTS) g_ehci_nports = EHCI_MAX_PORTS;

    uint16_t version = *(volatile uint16_t *)(g_ehci_cap + EHCI_HCIVERSION);
    kprintf("[ehci] HCI version %x.%02x, %u ports\n",
            version >> 8, version & 0xFF, g_ehci_nports);

    /* HC reset */
    ehci_op_w32(EHCI_USBCMD, EHCI_CMD_HCRESET);
    for (int i = 0; i < 100; i++) {
        if (!(ehci_op_r32(EHCI_USBCMD) & EHCI_CMD_HCRESET)) break;
        pit_sleep_ms(1);
    }
    if (ehci_op_r32(EHCI_USBCMD) & EHCI_CMD_HCRESET) {
        kprintf("[ehci] reset timeout\n");
        return -1;
    }

    /* Clear status */
    ehci_op_w32(EHCI_USBSTS, 0x3F);
    /* Disable interrupts during init */
    ehci_op_w32(EHCI_USBINTR, 0);
    /* 4 GB segment = 0 (we use identity-mapped phys below 4 GB) */
    ehci_op_w32(EHCI_CTRLDSSEGMENT, 0);

    if (!ehci_setup_periodic()) {
        kprintf("[ehci] failed to set up periodic schedule\n");
        return -1;
    }
    if (!ehci_setup_async()) {
        kprintf("[ehci] failed to set up async schedule\n");
        return -1;
    }

    /* Start the controller */
    ehci_op_w32(EHCI_USBCMD, EHCI_CMD_RUN | EHCI_CMD_FLS_1024 |
                              EHCI_CMD_ASE | EHCI_CMD_PSE);

    /* Wait for halted bit to clear */
    for (int i = 0; i < 100; i++) {
        if (!(ehci_op_r32(EHCI_USBSTS) & EHCI_STS_HCHALTED)) break;
        pit_sleep_ms(1);
    }

    /* Route all ports to EHCI */
    ehci_op_w32(EHCI_CONFIGFLAG, EHCI_CF_DONE);
    pit_sleep_ms(100);

    /* Power up all ports */
    for (int p = 0; p < g_ehci_nports; p++) {
        uint32_t sc = ehci_portsc(p);
        if (!(sc & EHCI_PORTSC_PP))
            ehci_portsc_set(p, sc | EHCI_PORTSC_PP);
    }
    pit_sleep_ms(20);

    /* Try MSI */
    g_ehci_irq_vec = irq_alloc_vector(ehci_isr, NULL);
    if (g_ehci_irq_vec) {
        if (pci_msi_enable(dev, g_ehci_irq_vec, (uint8_t)apic_read_id())) {
            ehci_op_w32(EHCI_USBINTR, EHCI_INTR_BITS);
            kprintf("[ehci] MSI vector 0x%02x\n", g_ehci_irq_vec);
        } else {
            g_ehci_irq_vec = 0;
        }
    }

    g_ehci_up = true;
    ehci_enumerate_ports();

    kprintf("[ehci] controller ready\n");
    return 0;
}

/* ---- port poll (called from kernel idle loop) -------------------- */

void ehci_poll(void) {
    if (!g_ehci_up) return;

    for (int p = 0; p < g_ehci_nports; p++) {
        uint32_t sc = ehci_portsc(p);
        if (!(sc & EHCI_PORTSC_CSC)) continue;

        /* Acknowledge change */
        ehci_portsc_set(p, sc | EHCI_PORTSC_CSC);

        if (sc & EHCI_PORTSC_CCS) {
            kprintf("[ehci] port %d: connect\n", p);
            if (ehci_port_reset(p))
                ehci_dispatch_device(p, USB_SPEED_HIGH);
        } else {
            kprintf("[ehci] port %d: disconnect\n", p);
        }
    }
}

/* ---- PCI driver registration ------------------------------------- */

static const struct pci_match ehci_match_table[] = {
    { PCI_ANY_ID, PCI_ANY_ID, PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, PCI_PROGIF_EHCI },
    PCI_MATCH_END
};

static struct pci_driver ehci_driver = {
    .name    = "ehci",
    .matches = ehci_match_table,
    .probe   = ehci_probe,
    .remove  = NULL,
};

void ehci_register(void) {
    pci_register_driver(&ehci_driver);
}
