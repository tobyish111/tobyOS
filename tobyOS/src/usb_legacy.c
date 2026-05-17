/* usb_legacy.c -- UHCI/OHCI/EHCI controller detection and safe handoff.
 *
 * This is intentionally conservative. Older laptops often rely on BIOS
 * USB-legacy SMI emulation to present USB keyboards/mice through the
 * 8042 PS/2 ports. Until tobyOS has complete UHCI/OHCI/EHCI transfer
 * schedulers, taking ownership of those controllers would make input
 * worse or disappear entirely. We therefore bind and report legacy HCIs,
 * but preserve firmware ownership when it is actively providing input.
 */

#include <tobyos/usb_legacy.h>
#include <tobyos/pci.h>
#include <tobyos/cpu.h>
#include <tobyos/pit.h>
#include <tobyos/pmm.h>
#include <tobyos/vmm.h>
#include <tobyos/usb.h>
#include <tobyos/usb_hid.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/abi/abi.h>

#define USB_LEGACY_MAX_CONTROLLERS 8

#define EHCI_CAP_CAPLENGTH 0x00
#define EHCI_CAP_HCIVERSION 0x02
#define EHCI_CAP_HCSPARAMS 0x04
#define EHCI_CAP_HCCPARAMS 0x08

#define EHCI_HCC_EECP_SHIFT 8
#define EHCI_HCC_EECP_MASK  0xFFu

#define EHCI_LEGSUP_BIOS_OWNED (1u << 16)
#define EHCI_LEGSUP_OS_OWNED   (1u << 24)

#define EHCI_OP_USBCMD     0x00
#define EHCI_OP_USBSTS     0x04
#define EHCI_OP_USBINTR    0x08
#define EHCI_OP_ASYNCLIST  0x18
#define EHCI_OP_PERIODIC   0x14
#define EHCI_OP_CONFIGFLAG 0x40
#define EHCI_OP_PORTSC(n)  (0x44u + ((uint32_t)(n) * 4u))

#define EHCI_CMD_RUN       (1u << 0)
#define EHCI_CMD_HCRESET   (1u << 1)
#define EHCI_CMD_PSE       (1u << 4)
#define EHCI_CMD_ASE       (1u << 5)
#define EHCI_STS_HCH       (1u << 12)
#define EHCI_STS_RECL      (1u << 13)
#define EHCI_STS_PSS       (1u << 14)
#define EHCI_STS_ASS       (1u << 15)
#define EHCI_PORT_CCS      (1u << 0)
#define EHCI_PORT_PE       (1u << 2)
#define EHCI_PORT_PR       (1u << 8)
#define EHCI_PORT_PP       (1u << 12)
#define EHCI_PORT_OWNER    (1u << 13)
#define EHCI_PORT_RW1C     ((1u << 1) | (1u << 3) | (1u << 5))

#define EHCI_LINK_TERM     0x00000001u
#define EHCI_LINK_QH       0x00000002u

#define EHCI_QTD_PID_OUT   0u
#define EHCI_QTD_PID_IN    1u
#define EHCI_QTD_PID_SETUP 2u
#define EHCI_QTD_ACTIVE    (1u << 7)
#define EHCI_QTD_HALTED    (1u << 6)
#define EHCI_QTD_DBE       (1u << 5)
#define EHCI_QTD_BABBLE    (1u << 4)
#define EHCI_QTD_XACT      (1u << 3)
#define EHCI_QTD_MMF       (1u << 2)
#define EHCI_QTD_IOC       (1u << 15)
#define EHCI_QTD_DT        (1u << 31)

#define EHCI_MAX_QH        32
#define EHCI_MAX_QTD       128
#define EHCI_MAX_DEV       8

struct __attribute__((packed, aligned(32))) ehci_qtd {
    volatile uint32_t next;
    volatile uint32_t alt_next;
    volatile uint32_t token;
    volatile uint32_t buf[5];
};

struct __attribute__((packed, aligned(32))) ehci_qh {
    volatile uint32_t horiz;
    volatile uint32_t epchar;
    volatile uint32_t epcap;
    volatile uint32_t current;
    volatile uint32_t next;
    volatile uint32_t alt_next;
    volatile uint32_t token;
    volatile uint32_t buf[5];
};

struct ehci_dev {
    bool in_use;
    uint8_t addr;
    uint8_t port;
    uint8_t ep_addr;
    uint16_t mps;
    struct usb_device usb;
    struct ehci_qh *int_qh;
    uint64_t int_qh_phys;
    struct ehci_qtd *int_qtd;
    uint64_t int_qtd_phys;
};

#define UHCI_USBCMD      0x00
#define UHCI_USBSTS      0x02
#define UHCI_USBINTR     0x04
#define UHCI_FRNUM       0x06
#define UHCI_FLBASEADD   0x08
#define UHCI_SOFMOD      0x0C
#define UHCI_PORTSC1     0x10
#define UHCI_PORTSC2     0x12

#define UHCI_CMD_RUN     (1u << 0)
#define UHCI_CMD_HCRESET (1u << 1)
#define UHCI_CMD_CF      (1u << 6)

#define UHCI_STS_USBINT  (1u << 0)
#define UHCI_STS_ERRINT  (1u << 1)
#define UHCI_STS_RD      (1u << 2)
#define UHCI_STS_HSE     (1u << 3)
#define UHCI_STS_HCPE    (1u << 4)
#define UHCI_STS_HCH     (1u << 5)

#define UHCI_PORT_CCS    (1u << 0)
#define UHCI_PORT_CSC    (1u << 1)
#define UHCI_PORT_PE     (1u << 2)
#define UHCI_PORT_PEC    (1u << 3)
#define UHCI_PORT_LSDA   (1u << 8)
#define UHCI_PORT_RESET  (1u << 9)

#define UHCI_PTR_TERM    0x00000001u
#define UHCI_PTR_QH      0x00000002u

#define UHCI_TD_ACTIVE   (1u << 23)
#define UHCI_TD_STALLED  (1u << 22)
#define UHCI_TD_DBUFERR  (1u << 21)
#define UHCI_TD_BABBLE   (1u << 20)
#define UHCI_TD_NAK      (1u << 19)
#define UHCI_TD_CRC      (1u << 18)
#define UHCI_TD_BITSTUFF (1u << 17)
#define UHCI_TD_LS       (1u << 26)
#define UHCI_TD_ERRCNT3  (3u << 27)

#define UHCI_PID_OUT     0xE1u
#define UHCI_PID_IN      0x69u
#define UHCI_PID_SETUP   0x2Du

#define UHCI_MAX_TD      128
#define UHCI_MAX_QH      16
#define UHCI_MAX_DEV     4

#define OHCI_REVISION          0x00
#define OHCI_CONTROL           0x04
#define OHCI_CMDSTATUS         0x08
#define OHCI_INTR_STATUS       0x0C
#define OHCI_INTR_ENABLE       0x10
#define OHCI_INTR_DISABLE      0x14
#define OHCI_HCCA              0x18
#define OHCI_PERIOD_CURRENT_ED 0x1C
#define OHCI_CONTROL_HEAD_ED   0x20
#define OHCI_CONTROL_CURRENT_ED 0x24
#define OHCI_DONE_HEAD         0x30
#define OHCI_FM_INTERVAL       0x34
#define OHCI_PERIODIC_START    0x40
#define OHCI_RH_DESC_A         0x48
#define OHCI_RH_STATUS         0x50
#define OHCI_RH_PORT_STATUS(n) (0x54u + ((uint32_t)(n) * 4u))

#define OHCI_CTL_PLE           (1u << 2)
#define OHCI_CTL_CLE           (1u << 4)
#define OHCI_CTL_USB_RESET     (0u << 6)
#define OHCI_CTL_USB_RESUME    (1u << 6)
#define OHCI_CTL_USB_OPER      (2u << 6)
#define OHCI_CTL_IR            (1u << 8)

#define OHCI_CMD_HCR           (1u << 0)
#define OHCI_CMD_CLF           (1u << 1)

#define OHCI_INTR_MIE          (1u << 31)

#define OHCI_RHDA_NDP(v)       ((uint8_t)((v) & 0xFFu))
#define OHCI_RH_PORT_CCS       (1u << 0)
#define OHCI_RH_PORT_PES       (1u << 1)
#define OHCI_RH_PORT_PRS       (1u << 4)
#define OHCI_RH_PORT_LSDA      (1u << 9)
#define OHCI_RH_PORT_CSC       (1u << 16)
#define OHCI_RH_PORT_PESC      (1u << 17)
#define OHCI_RH_PORT_PRSC      (1u << 20)

#define OHCI_ED_SKIP           (1u << 14)
#define OHCI_ED_LOW_SPEED      (1u << 13)
#define OHCI_ED_DIR_FROM_TD    (0u << 11)

#define OHCI_TD_CC_ACTIVE      (0xFu << 28)
#define OHCI_TD_CC_MASK        (0xFu << 28)
#define OHCI_TD_DP_SETUP       (0u << 19)
#define OHCI_TD_DP_OUT         (1u << 19)
#define OHCI_TD_DP_IN          (2u << 19)
#define OHCI_TD_DI_NONE        (7u << 21)
#define OHCI_TD_T_DATA0        (2u << 24)
#define OHCI_TD_T_DATA1        (3u << 24)

#define OHCI_MAX_ED            32
#define OHCI_MAX_TD            128
#define OHCI_MAX_DEV           8

struct __attribute__((packed, aligned(16))) ohci_ed {
    volatile uint32_t control;
    volatile uint32_t tailp;
    volatile uint32_t headp;
    volatile uint32_t nexted;
};

struct __attribute__((packed, aligned(16))) ohci_td {
    volatile uint32_t control;
    volatile uint32_t cbp;
    volatile uint32_t nexttd;
    volatile uint32_t be;
};

struct ohci_dev {
    bool in_use;
    bool low_speed;
    uint8_t addr;
    uint8_t port;
    uint8_t ep_addr;
    uint16_t mps;
    struct usb_device usb;
    struct ohci_ed *int_ed;
    uint64_t int_ed_phys;
    struct ohci_td *int_td;
    uint64_t int_td_phys;
    struct ohci_td *int_dummy;
    uint64_t int_dummy_phys;
};

struct __attribute__((packed, aligned(16))) uhci_td {
    volatile uint32_t link;
    volatile uint32_t status;
    volatile uint32_t token;
    volatile uint32_t buffer;
};

struct __attribute__((packed, aligned(16))) uhci_qh {
    volatile uint32_t link;
    volatile uint32_t elem;
};

struct uhci_dev {
    bool in_use;
    bool low_speed;
    uint8_t addr;
    uint8_t port;
    uint8_t ep_addr;
    uint16_t mps;
    struct usb_device usb;
    struct uhci_qh *int_qh;
    uint64_t int_qh_phys;
    struct uhci_td *int_td;
    uint64_t int_td_phys;
};

struct legacy_usb_ctl {
    bool in_use;
    uint8_t prog_if;
    uint8_t bus, slot, fn;
    uint8_t irq_line;
    uint16_t vendor, device;
    uint16_t version;
    uint8_t ports;
    bool bios_owned;
    bool os_owned;

    uint16_t io_base;
    bool uhci_online;
    uint32_t *frame;
    uint64_t frame_phys;
    struct uhci_qh *qh_pool;
    uint64_t qh_phys;
    int qh_used;
    struct uhci_td *td_pool;
    uint64_t td_phys;
    int td_used;
    int int_td_top;
    uint8_t *scratch;
    uint64_t scratch_phys;
    uint8_t next_addr;
    struct uhci_qh *async_qh;
    uint64_t async_qh_phys;
    struct uhci_dev devs[UHCI_MAX_DEV];

    volatile uint8_t *mmio;
    bool ohci_online;
    uint32_t *ohci_hcca;
    uint64_t ohci_hcca_phys;
    struct ohci_ed *ohci_ed_pool;
    uint64_t ohci_ed_phys;
    int ohci_ed_used;
    struct ohci_td *ohci_td_pool;
    uint64_t ohci_td_phys;
    int ohci_td_used;
    int ohci_int_td_top;
    uint8_t *ohci_scratch;
    uint64_t ohci_scratch_phys;
    uint8_t ohci_next_addr;
    struct ohci_ed *ohci_ctrl_ed;
    uint64_t ohci_ctrl_ed_phys;
    struct ohci_dev ohci_devs[OHCI_MAX_DEV];

    volatile uint8_t *ehci_op;
    bool ehci_online;
    uint32_t *ehci_periodic;
    uint64_t ehci_periodic_phys;
    struct ehci_qh *ehci_qh_pool;
    uint64_t ehci_qh_phys;
    int ehci_qh_used;
    struct ehci_qtd *ehci_qtd_pool;
    uint64_t ehci_qtd_phys;
    int ehci_qtd_used;
    int ehci_int_qtd_top;
    uint8_t *ehci_scratch;
    uint64_t ehci_scratch_phys;
    uint8_t ehci_next_addr;
    struct ehci_qh *ehci_async_qh;
    uint64_t ehci_async_qh_phys;
    struct ehci_dev ehci_devs[EHCI_MAX_DEV];
};

static struct legacy_usb_ctl g_ctl[USB_LEGACY_MAX_CONTROLLERS];
static int g_count;
static int g_uhci_count;
static int g_ohci_count;
static int g_ehci_count;

static bool uhci_control_xfer(struct usb_device *dev,
                              uint8_t bm_request_type, uint8_t b_request,
                              uint16_t w_value, uint16_t w_index,
                              void *buf, uint16_t w_length);
static void uhci_submit_int_in(struct usb_device *dev);
static bool ohci_control_xfer(struct usb_device *dev,
                              uint8_t bm_request_type, uint8_t b_request,
                              uint16_t w_value, uint16_t w_index,
                              void *buf, uint16_t w_length);
static void ohci_submit_int_in(struct usb_device *dev);
static bool ehci_control_xfer(struct usb_device *dev,
                              uint8_t bm_request_type, uint8_t b_request,
                              uint16_t w_value, uint16_t w_index,
                              void *buf, uint16_t w_length);
static void ehci_submit_int_in(struct usb_device *dev);

static const char *kind_name(uint8_t prog_if) {
    switch (prog_if) {
    case PCI_PROGIF_UHCI: return "UHCI";
    case PCI_PROGIF_OHCI: return "OHCI";
    case PCI_PROGIF_EHCI: return "EHCI";
    default:              return "USB";
    }
}

static struct legacy_usb_ctl *alloc_ctl(struct pci_dev *dev) {
    if (g_count >= USB_LEGACY_MAX_CONTROLLERS) return 0;
    struct legacy_usb_ctl *c = &g_ctl[g_count++];
    c->in_use = true;
    c->prog_if = dev->prog_if;
    c->bus = dev->bus;
    c->slot = dev->slot;
    c->fn = dev->fn;
    c->irq_line = dev->irq_line;
    c->vendor = dev->vendor;
    c->device = dev->device;
    return c;
}

static uint16_t uhci_r16(struct legacy_usb_ctl *c, uint16_t off) {
    return inw((uint16_t)(c->io_base + off));
}

static uint32_t mmio_r32(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static void mmio_w32(volatile uint8_t *base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(base + off) = v;
}

static struct ohci_ed *ohci_alloc_ed(struct legacy_usb_ctl *c,
                                     uint64_t *phys_out) {
    if (!c || c->ohci_ed_used >= OHCI_MAX_ED) return 0;
    int idx = c->ohci_ed_used++;
    struct ohci_ed *ed = &c->ohci_ed_pool[idx];
    memset(ed, 0, sizeof(*ed));
    if (phys_out) *phys_out = c->ohci_ed_phys + (uint64_t)idx * sizeof(*ed);
    return ed;
}

static struct ohci_td *ohci_alloc_td(struct legacy_usb_ctl *c,
                                     uint64_t *phys_out) {
    if (!c || c->ohci_td_used >= OHCI_MAX_TD) return 0;
    int idx = c->ohci_td_used++;
    struct ohci_td *td = &c->ohci_td_pool[idx];
    memset(td, 0, sizeof(*td));
    if (phys_out) *phys_out = c->ohci_td_phys + (uint64_t)idx * sizeof(*td);
    return td;
}

static struct ohci_td *ohci_alloc_int_td(struct legacy_usb_ctl *c,
                                         uint64_t *phys_out) {
    if (!c || c->ohci_int_td_top <= 64) return 0;
    int idx = --c->ohci_int_td_top;
    struct ohci_td *td = &c->ohci_td_pool[idx];
    memset(td, 0, sizeof(*td));
    if (phys_out) *phys_out = c->ohci_td_phys + (uint64_t)idx * sizeof(*td);
    return td;
}

static uint32_t ohci_ed_control(uint8_t addr, uint8_t ep,
                                bool low_speed, uint16_t mps) {
    return (uint32_t)addr |
           ((uint32_t)ep << 7) |
           OHCI_ED_DIR_FROM_TD |
           (low_speed ? OHCI_ED_LOW_SPEED : 0) |
           ((uint32_t)mps << 16);
}

static void ohci_td_fill(struct ohci_td *td, uint64_t next_phys,
                         uint32_t dp, bool data1, void *buf, uint16_t len) {
    td->control = OHCI_TD_CC_ACTIVE | OHCI_TD_DI_NONE | dp |
                  (data1 ? OHCI_TD_T_DATA1 : OHCI_TD_T_DATA0);
    td->cbp = (buf && len) ? (uint32_t)vmm_translate((uint64_t)buf) : 0;
    td->nexttd = (uint32_t)next_phys;
    td->be = (buf && len) ? (td->cbp + len - 1u) : 0;
}

static bool ohci_wait_tds(struct legacy_usb_ctl *c, struct ohci_td *first,
                          int count, const char *what) {
    for (int spin = 0; spin < 1000; spin++) {
        bool active = false;
        for (int i = 0; i < count; i++) {
            if ((first[i].control & OHCI_TD_CC_MASK) == OHCI_TD_CC_ACTIVE) {
                active = true;
                break;
            }
        }
        if (!active) break;
        pit_sleep_ms(1);
    }

    bool ok = true;
    for (int i = 0; i < count; i++) {
        uint32_t cc = first[i].control & OHCI_TD_CC_MASK;
        if (cc == OHCI_TD_CC_ACTIVE) {
            kprintf("[usb-legacy] OHCI %s timeout td=%d ctrl=0x%08x\n",
                    what, i, first[i].control);
            ok = false;
        } else if (cc != 0) {
            kprintf("[usb-legacy] OHCI %s error td=%d ctrl=0x%08x\n",
                    what, i, first[i].control);
            ok = false;
        }
    }
    mmio_w32(c->mmio, OHCI_INTR_STATUS, 0xFFFFFFFFu);
    return ok;
}

static bool ohci_control_xfer(struct usb_device *dev,
                              uint8_t bm_request_type, uint8_t b_request,
                              uint16_t w_value, uint16_t w_index,
                              void *buf, uint16_t w_length) {
    if (!dev || !dev->hci_priv) return false;
    struct legacy_usb_ctl *c = (struct legacy_usb_ctl *)dev->hci_priv;
    if (!c->ohci_online || !c->ohci_ctrl_ed) return false;

    c->ohci_td_used = 0;
    bool dir_in = (bm_request_type & USB_DIR_IN) != 0;
    bool low = (dev->speed == USB_SPEED_LOW);
    uint16_t mps = dev->mps0 ? dev->mps0 : (low ? 8u : 64u);

    struct usb_setup_packet *setup = (struct usb_setup_packet *)c->ohci_scratch;
    setup->bmRequestType = bm_request_type;
    setup->bRequest = b_request;
    setup->wValue = w_value;
    setup->wIndex = w_index;
    setup->wLength = w_length;

    int td_count = 1 + ((w_length + mps - 1u) / mps) + 1;
    if (td_count > OHCI_MAX_TD - 1) return false;

    uint64_t phys[OHCI_MAX_TD];
    struct ohci_td *td0 = ohci_alloc_td(c, &phys[0]);
    if (!td0) return false;
    for (int i = 1; i <= td_count; i++) {
        if (!ohci_alloc_td(c, &phys[i])) return false;
    }

    int ix = 0;
    ohci_td_fill(&td0[ix], phys[ix + 1], OHCI_TD_DP_SETUP, false,
                 setup, sizeof(*setup));
    ix++;

    uint8_t *p = (uint8_t *)buf;
    uint16_t left = w_length;
    bool data1 = true;
    while (left > 0) {
        uint16_t chunk = left > mps ? mps : left;
        ohci_td_fill(&td0[ix], phys[ix + 1],
                     dir_in ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT,
                     data1, p, chunk);
        p += chunk;
        left -= chunk;
        data1 = !data1;
        ix++;
    }

    ohci_td_fill(&td0[ix], phys[ix + 1],
                 dir_in ? OHCI_TD_DP_OUT : OHCI_TD_DP_IN,
                 true, 0, 0);

    c->ohci_ctrl_ed->control =
        ohci_ed_control(dev->bus_address, 0, low, mps);
    c->ohci_ctrl_ed->tailp = (uint32_t)phys[td_count];
    c->ohci_ctrl_ed->headp = (uint32_t)phys[0];
    c->ohci_ctrl_ed->nexted = 0;
    mmio_w32(c->mmio, OHCI_CONTROL_HEAD_ED, (uint32_t)c->ohci_ctrl_ed_phys);
    mmio_w32(c->mmio, OHCI_CMDSTATUS, OHCI_CMD_CLF);

    bool ok = ohci_wait_tds(c, td0, td_count, "control");
    c->ohci_ctrl_ed->headp = c->ohci_ctrl_ed->tailp;
    return ok;
}

static bool ohci_get_desc(struct usb_device *dev, uint8_t type,
                          void *buf, uint16_t len) {
    return ohci_control_xfer(dev, USB_DIR_IN | USB_TYPE_STANDARD |
                                  USB_RECIP_DEVICE,
                             USB_REQ_GET_DESCRIPTOR,
                             (uint16_t)type << 8, 0, buf, len);
}

static bool ohci_set_address(struct usb_device *dev, uint8_t addr) {
    if (!ohci_control_xfer(dev, USB_DIR_OUT | USB_TYPE_STANDARD |
                                USB_RECIP_DEVICE,
                           USB_REQ_SET_ADDRESS, addr, 0, 0, 0)) {
        return false;
    }
    pit_sleep_ms(10);
    dev->bus_address = addr;
    return true;
}

static bool ohci_set_config(struct usb_device *dev, uint8_t cfg) {
    return ohci_control_xfer(dev, USB_DIR_OUT | USB_TYPE_STANDARD |
                                  USB_RECIP_DEVICE,
                             USB_REQ_SET_CONFIGURATION, cfg, 0, 0, 0);
}

static void ohci_submit_int_in(struct usb_device *dev) {
    if (!dev || !dev->hci_priv || !dev->int_buf || dev->int_armed) return;
    struct legacy_usb_ctl *c = (struct legacy_usb_ctl *)dev->hci_priv;
    struct ohci_dev *od = (struct ohci_dev *)dev->hci_dev_priv;
    if (!od || !od->int_ed || !od->int_td || !od->int_dummy) return;

    bool low = dev->speed == USB_SPEED_LOW;
    od->int_ed->control =
        ohci_ed_control(dev->bus_address, USB_EP_NUM(od->ep_addr),
                        low, dev->int_buf_size);
    ohci_td_fill(od->int_td, od->int_dummy_phys, OHCI_TD_DP_IN,
                 true, dev->int_buf, dev->int_buf_size);
    od->int_dummy->control = 0;
    od->int_dummy->nexttd = 0;
    od->int_ed->tailp = (uint32_t)od->int_dummy_phys;
    od->int_ed->headp = (uint32_t)od->int_td_phys;
    dev->int_armed = true;
    (void)c;
}

static void ohci_poll_device(struct legacy_usb_ctl *c, struct ohci_dev *od) {
    if (!od->in_use || !od->usb.int_armed || !od->int_td) return;
    uint32_t cc = od->int_td->control & OHCI_TD_CC_MASK;
    if (cc == OHCI_TD_CC_ACTIVE) return;

    od->usb.int_armed = false;
    if (cc == 0 && od->usb.int_complete) {
        uint32_t len = od->usb.int_buf_size;
        if (od->int_td->cbp && od->int_td->be >= od->int_td->cbp) {
            uint32_t remaining = od->int_td->be - od->int_td->cbp + 1u;
            if (remaining < len) len -= remaining;
        }
        od->usb.int_complete(&od->usb, od->usb.int_buf, len);
    }
    mmio_w32(c->mmio, OHCI_INTR_STATUS, 0xFFFFFFFFu);
    ohci_submit_int_in(&od->usb);
}

static void uhci_w16(struct legacy_usb_ctl *c, uint16_t off, uint16_t v) {
    outw((uint16_t)(c->io_base + off), v);
}

static void uhci_w32(struct legacy_usb_ctl *c, uint16_t off, uint32_t v) {
    outl((uint16_t)(c->io_base + off), v);
}

static struct uhci_qh *uhci_alloc_qh(struct legacy_usb_ctl *c,
                                     uint64_t *phys_out) {
    if (!c || c->qh_used >= UHCI_MAX_QH) return 0;
    int idx = c->qh_used++;
    struct uhci_qh *qh = &c->qh_pool[idx];
    memset(qh, 0, sizeof(*qh));
    if (phys_out) *phys_out = c->qh_phys + (uint64_t)idx * sizeof(*qh);
    qh->link = UHCI_PTR_TERM;
    qh->elem = UHCI_PTR_TERM;
    return qh;
}

static struct uhci_td *uhci_alloc_td(struct legacy_usb_ctl *c,
                                     uint64_t *phys_out) {
    if (!c || c->td_used >= UHCI_MAX_TD) return 0;
    int idx = c->td_used++;
    struct uhci_td *td = &c->td_pool[idx];
    memset(td, 0, sizeof(*td));
    if (phys_out) *phys_out = c->td_phys + (uint64_t)idx * sizeof(*td);
    td->link = UHCI_PTR_TERM;
    return td;
}

static struct uhci_td *uhci_alloc_int_td(struct legacy_usb_ctl *c,
                                         uint64_t *phys_out) {
    if (!c || c->int_td_top <= 64) return 0;
    int idx = --c->int_td_top;
    struct uhci_td *td = &c->td_pool[idx];
    memset(td, 0, sizeof(*td));
    if (phys_out) *phys_out = c->td_phys + (uint64_t)idx * sizeof(*td);
    td->link = UHCI_PTR_TERM;
    return td;
}

static uint32_t uhci_len_token(uint16_t len) {
    return len == 0 ? (0x7FFu << 21) : (((uint32_t)len - 1u) << 21);
}

static void uhci_td_fill(struct legacy_usb_ctl *c, struct uhci_td *td,
                         uint64_t next_phys, uint8_t pid, uint8_t addr,
                         uint8_t ep, bool toggle, bool low_speed,
                         void *buf, uint16_t len) {
    td->link = next_phys ? (uint32_t)next_phys : UHCI_PTR_TERM;
    td->status = UHCI_TD_ACTIVE | UHCI_TD_ERRCNT3 |
                 (low_speed ? UHCI_TD_LS : 0);
    td->token = (uint32_t)pid |
                ((uint32_t)addr << 8) |
                ((uint32_t)ep << 15) |
                (toggle ? (1u << 19) : 0) |
                uhci_len_token(len);
    td->buffer = (buf && len) ? (uint32_t)vmm_translate((uint64_t)buf) : 0;
    (void)c;
}

static bool uhci_wait_td_chain(struct legacy_usb_ctl *c,
                               struct uhci_td *first, int count,
                               const char *what) {
    for (int spin = 0; spin < 1000; spin++) {
        bool any_active = false;
        for (int i = 0; i < count; i++) {
            if (first[i].status & UHCI_TD_ACTIVE) {
                any_active = true;
                break;
            }
        }
        if (!any_active) break;
        pit_sleep_ms(1);
    }

    for (int i = 0; i < count; i++) {
        uint32_t st = first[i].status;
        if (st & UHCI_TD_ACTIVE) {
            kprintf("[usb-legacy] UHCI %s timeout td=%d st=0x%08x\n",
                    what, i, st);
            return false;
        }
        if (st & (UHCI_TD_STALLED | UHCI_TD_DBUFERR | UHCI_TD_BABBLE |
                  UHCI_TD_CRC | UHCI_TD_BITSTUFF)) {
            kprintf("[usb-legacy] UHCI %s error td=%d st=0x%08x\n",
                    what, i, st);
            return false;
        }
    }
    uhci_w16(c, UHCI_USBSTS, UHCI_STS_USBINT | UHCI_STS_ERRINT |
                            UHCI_STS_RD | UHCI_STS_HSE | UHCI_STS_HCPE);
    return true;
}

static bool uhci_control_xfer(struct usb_device *dev,
                              uint8_t bm_request_type, uint8_t b_request,
                              uint16_t w_value, uint16_t w_index,
                              void *buf, uint16_t w_length) {
    if (!dev || !dev->hci_priv) return false;
    struct legacy_usb_ctl *c = (struct legacy_usb_ctl *)dev->hci_priv;
    if (!c->uhci_online) return false;

    c->td_used = 0;
    bool dir_in = (bm_request_type & USB_DIR_IN) != 0;
    bool low = (dev->speed == USB_SPEED_LOW);
    uint8_t addr = dev->bus_address;
    uint8_t ep = 0;
    uint16_t mps = dev->mps0 ? dev->mps0 : (low ? 8u : 64u);

    struct usb_setup_packet *setup = (struct usb_setup_packet *)c->scratch;
    setup->bmRequestType = bm_request_type;
    setup->bRequest = b_request;
    setup->wValue = w_value;
    setup->wIndex = w_index;
    setup->wLength = w_length;

    int td_count = 1 + ((w_length + mps - 1u) / mps) + 1;
    if (td_count > UHCI_MAX_TD) return false;

    uint64_t phys[UHCI_MAX_TD];
    struct uhci_td *td0 = uhci_alloc_td(c, &phys[0]);
    if (!td0) return false;
    for (int i = 1; i < td_count; i++) {
        if (!uhci_alloc_td(c, &phys[i])) return false;
    }

    int ix = 0;
    uhci_td_fill(c, &td0[ix], phys[ix + 1], UHCI_PID_SETUP, addr, ep,
                 false, low, setup, sizeof(*setup));
    ix++;

    uint8_t *p = (uint8_t *)buf;
    uint16_t left = w_length;
    bool toggle = true;
    while (left > 0) {
        uint16_t chunk = left > mps ? mps : left;
        uhci_td_fill(c, &td0[ix], phys[ix + 1],
                     dir_in ? UHCI_PID_IN : UHCI_PID_OUT,
                     addr, ep, toggle, low, p, chunk);
        p += chunk;
        left -= chunk;
        toggle = !toggle;
        ix++;
    }

    uhci_td_fill(c, &td0[ix], 0, dir_in ? UHCI_PID_OUT : UHCI_PID_IN,
                 addr, ep, true, low, 0, 0);

    c->async_qh->elem = (uint32_t)phys[0];
    bool ok = uhci_wait_td_chain(c, td0, td_count, "control");
    c->async_qh->elem = UHCI_PTR_TERM;
    return ok;
}

static bool uhci_get_desc(struct usb_device *dev, uint8_t type,
                          void *buf, uint16_t len) {
    return uhci_control_xfer(dev, USB_DIR_IN | USB_TYPE_STANDARD |
                                  USB_RECIP_DEVICE,
                             USB_REQ_GET_DESCRIPTOR,
                             (uint16_t)type << 8, 0, buf, len);
}

static bool uhci_set_address(struct usb_device *dev, uint8_t addr) {
    if (!uhci_control_xfer(dev, USB_DIR_OUT | USB_TYPE_STANDARD |
                                USB_RECIP_DEVICE,
                           USB_REQ_SET_ADDRESS, addr, 0, 0, 0)) {
        return false;
    }
    pit_sleep_ms(10);
    dev->bus_address = addr;
    return true;
}

static bool uhci_set_config(struct usb_device *dev, uint8_t cfg) {
    return uhci_control_xfer(dev, USB_DIR_OUT | USB_TYPE_STANDARD |
                                  USB_RECIP_DEVICE,
                             USB_REQ_SET_CONFIGURATION, cfg, 0, 0, 0);
}

static void uhci_submit_int_in(struct usb_device *dev) {
    if (!dev || !dev->hci_priv || !dev->int_buf || dev->int_armed) return;
    struct uhci_dev *ud = (struct uhci_dev *)dev->hci_dev_priv;
    if (!ud || !ud->int_qh || !ud->int_td) return;

    bool low = dev->speed == USB_SPEED_LOW;
    uhci_td_fill((struct legacy_usb_ctl *)dev->hci_priv, ud->int_td, 0,
                 UHCI_PID_IN, dev->bus_address,
                 USB_EP_NUM(ud->ep_addr), true, low,
                 dev->int_buf, dev->int_buf_size);
    ud->int_qh->elem = (uint32_t)ud->int_td_phys;
    dev->int_armed = true;
}

static void uhci_poll_device(struct legacy_usb_ctl *c, struct uhci_dev *ud) {
    if (!ud->in_use || !ud->usb.int_armed || !ud->int_td) return;
    uint32_t st = ud->int_td->status;
    if (st & UHCI_TD_ACTIVE) return;

    ud->usb.int_armed = false;
    ud->int_qh->elem = UHCI_PTR_TERM;

    if (!(st & (UHCI_TD_STALLED | UHCI_TD_DBUFERR | UHCI_TD_BABBLE |
                UHCI_TD_CRC | UHCI_TD_BITSTUFF | UHCI_TD_NAK))) {
        uint32_t actual = ((st & 0x7FFu) == 0x7FFu) ? 0u :
                          ((st & 0x7FFu) + 1u);
        if (actual > ud->usb.int_buf_size) actual = ud->usb.int_buf_size;
        if (ud->usb.int_complete) {
            ud->usb.int_complete(&ud->usb, ud->usb.int_buf, actual);
        }
    }
    uhci_w16(c, UHCI_USBSTS, UHCI_STS_USBINT | UHCI_STS_ERRINT |
                            UHCI_STS_RD | UHCI_STS_HSE | UHCI_STS_HCPE);
    uhci_submit_int_in(&ud->usb);
}

static struct uhci_dev *uhci_alloc_dev(struct legacy_usb_ctl *c) {
    for (int i = 0; i < UHCI_MAX_DEV; i++) {
        if (!c->devs[i].in_use) {
            memset(&c->devs[i], 0, sizeof(c->devs[i]));
            c->devs[i].in_use = true;
            return &c->devs[i];
        }
    }
    return 0;
}

static bool uhci_probe_hid_config(struct legacy_usb_ctl *c,
                                  struct uhci_dev *ud,
                                  struct usb_device_desc *dd) {
    uint8_t *cfg = c->scratch;
    memset(cfg, 0, PAGE_SIZE);
    if (!uhci_get_desc(&ud->usb, USB_DESC_CONFIG, cfg, 9)) return false;

    struct usb_config_desc *cd = (struct usb_config_desc *)cfg;
    uint16_t total = cd->wTotalLength;
    if (total > PAGE_SIZE) total = PAGE_SIZE;
    if (total > 9 && !uhci_get_desc(&ud->usb, USB_DESC_CONFIG, cfg, total)) {
        return false;
    }
    if (!uhci_set_config(&ud->usb, cd->bConfigurationValue)) return false;

    uint16_t off = cd->bLength;
    const struct usb_iface_desc *iface = 0;
    while (off + 2u <= total) {
        uint8_t len = cfg[off];
        uint8_t typ = cfg[off + 1];
        if (len == 0 || off + len > total) break;
        if (typ == USB_DESC_INTERFACE) {
            iface = (const struct usb_iface_desc *)(cfg + off);
        } else if (typ == USB_DESC_ENDPOINT && iface &&
                   iface->bInterfaceClass == USB_CLASS_HID) {
            const struct usb_endpoint_desc *ep =
                (const struct usb_endpoint_desc *)(cfg + off);
            if (USB_EP_TYPE(ep->bmAttributes) == USB_EP_INTERRUPT &&
                USB_EP_DIR_IN(ep->bEndpointAddress) &&
                usb_hid_probe(&ud->usb, iface, ep)) {
                uint64_t qhp = 0, tdp = 0;
                ud->int_qh = uhci_alloc_qh(c, &qhp);
                ud->int_td = uhci_alloc_int_td(c, &tdp);
                if (!ud->int_qh || !ud->int_td) return false;
                ud->int_qh_phys = qhp;
                ud->int_td_phys = tdp;
                ud->ep_addr = ep->bEndpointAddress;
                ud->mps = ep->wMaxPacketSize & 0x07FFu;
                ud->int_qh->link = c->async_qh->link;
                c->async_qh->link = (uint32_t)qhp | UHCI_PTR_QH;
                uhci_submit_int_in(&ud->usb);
                kprintf("[usb-legacy] UHCI HID %04x:%04x addr=%u port=%u ep=0x%02x\n",
                        dd->idVendor, dd->idProduct, ud->addr, ud->port,
                        ud->ep_addr);
                return true;
            }
        }
        off += len;
    }
    return false;
}

static bool uhci_enumerate_port(struct legacy_usb_ctl *c, uint8_t port) {
    uint16_t reg = (uint16_t)(port == 1 ? UHCI_PORTSC1 : UHCI_PORTSC2);
    uint16_t ps = uhci_r16(c, reg);
    if (!(ps & UHCI_PORT_CCS)) return false;

    uhci_w16(c, reg, (uint16_t)((ps & ~(UHCI_PORT_CSC | UHCI_PORT_PEC)) |
                                UHCI_PORT_RESET));
    pit_sleep_ms(50);
    ps = uhci_r16(c, reg);
    uhci_w16(c, reg, (uint16_t)(ps & ~UHCI_PORT_RESET));
    pit_sleep_ms(20);
    ps = uhci_r16(c, reg);
    uhci_w16(c, reg, (uint16_t)((ps | UHCI_PORT_PE) &
                                ~(UHCI_PORT_CSC | UHCI_PORT_PEC)));
    pit_sleep_ms(20);
    ps = uhci_r16(c, reg);
    if (!(ps & UHCI_PORT_PE)) {
        kprintf("[usb-legacy] UHCI port %u connected but not enabled "
                "(PORTSC=0x%04x)\n", port, ps);
        return false;
    }

    struct uhci_dev *ud = uhci_alloc_dev(c);
    if (!ud) return false;
    ud->low_speed = (ps & UHCI_PORT_LSDA) != 0;
    ud->port = port;
    ud->addr = ++c->next_addr;
    if (c->next_addr == 0 || c->next_addr > 120) c->next_addr = 1;

    ud->usb.slot_id = (uint8_t)(0x80u | ud->addr);
    ud->usb.port_id = port;
    ud->usb.speed = ud->low_speed ? USB_SPEED_LOW : USB_SPEED_FULL;
    ud->usb.mps0 = ud->low_speed ? 8u : 8u;
    ud->usb.control_xfer = uhci_control_xfer;
    ud->usb.submit_int_in = uhci_submit_int_in;
    ud->usb.hci_priv = c;
    ud->usb.hci_dev_priv = ud;

    struct usb_device_desc *dd = (struct usb_device_desc *)c->scratch;
    memset(dd, 0, sizeof(*dd));
    if (!uhci_get_desc(&ud->usb, USB_DESC_DEVICE, dd, 8)) {
        kprintf("[usb-legacy] UHCI port %u: device descriptor probe failed\n",
                port);
        ud->in_use = false;
        return false;
    }
    if (dd->bMaxPacketSize0) ud->usb.mps0 = dd->bMaxPacketSize0;
    if (!uhci_set_address(&ud->usb, ud->addr)) {
        kprintf("[usb-legacy] UHCI port %u: SET_ADDRESS failed\n", port);
        ud->in_use = false;
        return false;
    }
    if (!uhci_get_desc(&ud->usb, USB_DESC_DEVICE, dd, sizeof(*dd))) {
        kprintf("[usb-legacy] UHCI port %u: full device descriptor failed\n",
                port);
        ud->in_use = false;
        return false;
    }

    kprintf("[usb-legacy] UHCI port %u: addr=%u speed=%s vid=%04x pid=%04x class=%02x\n",
            port, ud->addr, ud->low_speed ? "low" : "full",
            dd->idVendor, dd->idProduct, dd->bDeviceClass);

    if (!uhci_probe_hid_config(c, ud, dd)) {
        kprintf("[usb-legacy] UHCI port %u: no boot HID driver claimed device\n",
                port);
    }
    return true;
}

static bool uhci_init_controller(struct pci_dev *dev,
                                 struct legacy_usb_ctl *c) {
    if (!dev->bar_is_io[4] || dev->bar[4] == 0) {
        kprintf("[usb-legacy] UHCI %02x:%02x.%x: BAR4 I/O base missing\n",
                dev->bus, dev->slot, dev->fn);
        return false;
    }
    c->io_base = (uint16_t)(dev->bar[4] & 0xFFF0u);

    c->frame_phys = pmm_alloc_page();
    c->qh_phys = pmm_alloc_page();
    c->td_phys = pmm_alloc_page();
    c->scratch_phys = pmm_alloc_page();
    if (!c->frame_phys || !c->qh_phys || !c->td_phys || !c->scratch_phys) {
        kprintf("[usb-legacy] UHCI OOM during ring allocation\n");
        return false;
    }
    c->frame = (uint32_t *)pmm_phys_to_virt(c->frame_phys);
    c->qh_pool = (struct uhci_qh *)pmm_phys_to_virt(c->qh_phys);
    c->td_pool = (struct uhci_td *)pmm_phys_to_virt(c->td_phys);
    c->scratch = (uint8_t *)pmm_phys_to_virt(c->scratch_phys);
    memset(c->frame, 0, PAGE_SIZE);
    memset(c->qh_pool, 0, PAGE_SIZE);
    memset(c->td_pool, 0, PAGE_SIZE);
    memset(c->scratch, 0, PAGE_SIZE);
    c->int_td_top = UHCI_MAX_TD;

    uint64_t async_phys = 0;
    c->async_qh = uhci_alloc_qh(c, &async_phys);
    c->async_qh_phys = async_phys;
    if (!c->async_qh) return false;
    for (int i = 0; i < 1024; i++) {
        c->frame[i] = (uint32_t)c->async_qh_phys | UHCI_PTR_QH;
    }

    pci_dev_enable(dev, PCI_CMD_IO_SPACE | PCI_CMD_BUS_MASTER);
    uhci_w16(c, UHCI_USBCMD, 0);
    pit_sleep_ms(1);
    uhci_w16(c, UHCI_USBCMD, UHCI_CMD_HCRESET);
    for (int i = 0; i < 100; i++) {
        if (!(uhci_r16(c, UHCI_USBCMD) & UHCI_CMD_HCRESET)) break;
        pit_sleep_ms(1);
    }
    uhci_w16(c, UHCI_USBSTS, 0x003Fu);
    uhci_w16(c, UHCI_USBINTR, 0);
    uhci_w16(c, UHCI_FRNUM, 0);
    uhci_w32(c, UHCI_FLBASEADD, (uint32_t)c->frame_phys);
    outb((uint16_t)(c->io_base + UHCI_SOFMOD), 0x40);
    uhci_w16(c, UHCI_USBCMD, UHCI_CMD_RUN | UHCI_CMD_CF);
    pit_sleep_ms(20);

    c->uhci_online = true;
    c->ports = 2;
    c->next_addr = 0;
    kprintf("[usb-legacy] UHCI online io=0x%04x frame=%p\n",
            (unsigned)c->io_base, (void *)c->frame_phys);

    (void)uhci_enumerate_port(c, 1);
    (void)uhci_enumerate_port(c, 2);
    return true;
}

static struct ohci_dev *ohci_alloc_dev(struct legacy_usb_ctl *c) {
    for (int i = 0; i < OHCI_MAX_DEV; i++) {
        if (!c->ohci_devs[i].in_use) {
            memset(&c->ohci_devs[i], 0, sizeof(c->ohci_devs[i]));
            c->ohci_devs[i].in_use = true;
            return &c->ohci_devs[i];
        }
    }
    return 0;
}

static bool ohci_probe_hid_config(struct legacy_usb_ctl *c,
                                  struct ohci_dev *od,
                                  struct usb_device_desc *dd) {
    uint8_t *cfg = c->ohci_scratch;
    memset(cfg, 0, PAGE_SIZE);
    if (!ohci_get_desc(&od->usb, USB_DESC_CONFIG, cfg, 9)) return false;

    struct usb_config_desc *cd = (struct usb_config_desc *)cfg;
    uint16_t total = cd->wTotalLength;
    if (total > PAGE_SIZE) total = PAGE_SIZE;
    if (total > 9 && !ohci_get_desc(&od->usb, USB_DESC_CONFIG, cfg, total)) {
        return false;
    }
    if (!ohci_set_config(&od->usb, cd->bConfigurationValue)) return false;

    uint16_t off = cd->bLength;
    const struct usb_iface_desc *iface = 0;
    while (off + 2u <= total) {
        uint8_t len = cfg[off];
        uint8_t typ = cfg[off + 1];
        if (len == 0 || off + len > total) break;
        if (typ == USB_DESC_INTERFACE) {
            iface = (const struct usb_iface_desc *)(cfg + off);
        } else if (typ == USB_DESC_ENDPOINT && iface &&
                   iface->bInterfaceClass == USB_CLASS_HID) {
            const struct usb_endpoint_desc *ep =
                (const struct usb_endpoint_desc *)(cfg + off);
            if (USB_EP_TYPE(ep->bmAttributes) == USB_EP_INTERRUPT &&
                USB_EP_DIR_IN(ep->bEndpointAddress) &&
                usb_hid_probe(&od->usb, iface, ep)) {
                uint64_t edp = 0, tdp = 0, dummyp = 0;
                od->int_ed = ohci_alloc_ed(c, &edp);
                od->int_td = ohci_alloc_int_td(c, &tdp);
                od->int_dummy = ohci_alloc_int_td(c, &dummyp);
                if (!od->int_ed || !od->int_td || !od->int_dummy) {
                    return false;
                }
                od->int_ed_phys = edp;
                od->int_td_phys = tdp;
                od->int_dummy_phys = dummyp;
                od->ep_addr = ep->bEndpointAddress;
                od->mps = ep->wMaxPacketSize & 0x07FFu;
                od->int_ed->nexted = 0;
                od->int_ed->headp = (uint32_t)dummyp;
                od->int_ed->tailp = (uint32_t)dummyp;
                c->ohci_hcca[0] = (uint32_t)edp;
                ohci_submit_int_in(&od->usb);
                kprintf("[usb-legacy] OHCI HID %04x:%04x addr=%u port=%u ep=0x%02x\n",
                        dd->idVendor, dd->idProduct, od->addr, od->port,
                        od->ep_addr);
                return true;
            }
        }
        off += len;
    }
    return false;
}

static bool ohci_enumerate_port(struct legacy_usb_ctl *c, uint8_t port) {
    uint32_t off = OHCI_RH_PORT_STATUS((uint32_t)(port - 1u));
    uint32_t ps = mmio_r32(c->mmio, off);
    if (!(ps & OHCI_RH_PORT_CCS)) return false;

    mmio_w32(c->mmio, off, OHCI_RH_PORT_PRS);
    pit_sleep_ms(60);
    for (int i = 0; i < 100; i++) {
        ps = mmio_r32(c->mmio, off);
        if (!(ps & OHCI_RH_PORT_PRS)) break;
        pit_sleep_ms(1);
    }
    mmio_w32(c->mmio, off, OHCI_RH_PORT_CSC |
                           OHCI_RH_PORT_PESC |
                           OHCI_RH_PORT_PRSC);
    ps = mmio_r32(c->mmio, off);
    if (!(ps & OHCI_RH_PORT_PES)) {
        kprintf("[usb-legacy] OHCI port %u connected but not enabled "
                "(PORTSC=0x%08x)\n", port, ps);
        return false;
    }

    struct ohci_dev *od = ohci_alloc_dev(c);
    if (!od) return false;
    od->low_speed = (ps & OHCI_RH_PORT_LSDA) != 0;
    od->port = port;
    od->addr = ++c->ohci_next_addr;
    if (c->ohci_next_addr == 0 || c->ohci_next_addr > 120) {
        c->ohci_next_addr = 1;
    }

    od->usb.slot_id = (uint8_t)(0x40u | od->addr);
    od->usb.port_id = port;
    od->usb.speed = od->low_speed ? USB_SPEED_LOW : USB_SPEED_FULL;
    od->usb.mps0 = 8;
    od->usb.control_xfer = ohci_control_xfer;
    od->usb.submit_int_in = ohci_submit_int_in;
    od->usb.hci_priv = c;
    od->usb.hci_dev_priv = od;

    struct usb_device_desc *dd = (struct usb_device_desc *)c->ohci_scratch;
    memset(dd, 0, sizeof(*dd));
    if (!ohci_get_desc(&od->usb, USB_DESC_DEVICE, dd, 8)) {
        kprintf("[usb-legacy] OHCI port %u: device descriptor probe failed\n",
                port);
        od->in_use = false;
        return false;
    }
    if (dd->bMaxPacketSize0) od->usb.mps0 = dd->bMaxPacketSize0;
    if (!ohci_set_address(&od->usb, od->addr)) {
        kprintf("[usb-legacy] OHCI port %u: SET_ADDRESS failed\n", port);
        od->in_use = false;
        return false;
    }
    if (!ohci_get_desc(&od->usb, USB_DESC_DEVICE, dd, sizeof(*dd))) {
        kprintf("[usb-legacy] OHCI port %u: full device descriptor failed\n",
                port);
        od->in_use = false;
        return false;
    }

    kprintf("[usb-legacy] OHCI port %u: addr=%u speed=%s vid=%04x pid=%04x class=%02x\n",
            port, od->addr, od->low_speed ? "low" : "full",
            dd->idVendor, dd->idProduct, dd->bDeviceClass);
    if (!ohci_probe_hid_config(c, od, dd)) {
        kprintf("[usb-legacy] OHCI port %u: no boot HID driver claimed device\n",
                port);
    }
    return true;
}

static bool ohci_init_controller(struct pci_dev *dev,
                                 struct legacy_usb_ctl *c) {
    volatile uint8_t *mmio = (volatile uint8_t *)pci_map_bar(dev, 0, 0x1000);
    if (!mmio) {
        kprintf("[usb-legacy] OHCI %02x:%02x.%x: BAR0 map failed\n",
                dev->bus, dev->slot, dev->fn);
        return false;
    }
    c->mmio = mmio;
    c->version = (uint16_t)(mmio_r32(mmio, OHCI_REVISION) & 0xFFu);
    c->ports = OHCI_RHDA_NDP(mmio_r32(mmio, OHCI_RH_DESC_A));

    c->ohci_hcca_phys = pmm_alloc_page();
    c->ohci_ed_phys = pmm_alloc_page();
    c->ohci_td_phys = pmm_alloc_page();
    c->ohci_scratch_phys = pmm_alloc_page();
    if (!c->ohci_hcca_phys || !c->ohci_ed_phys ||
        !c->ohci_td_phys || !c->ohci_scratch_phys) {
        kprintf("[usb-legacy] OHCI OOM during schedule allocation\n");
        return false;
    }
    c->ohci_hcca = (uint32_t *)pmm_phys_to_virt(c->ohci_hcca_phys);
    c->ohci_ed_pool = (struct ohci_ed *)pmm_phys_to_virt(c->ohci_ed_phys);
    c->ohci_td_pool = (struct ohci_td *)pmm_phys_to_virt(c->ohci_td_phys);
    c->ohci_scratch = (uint8_t *)pmm_phys_to_virt(c->ohci_scratch_phys);
    memset(c->ohci_hcca, 0, PAGE_SIZE);
    memset(c->ohci_ed_pool, 0, PAGE_SIZE);
    memset(c->ohci_td_pool, 0, PAGE_SIZE);
    memset(c->ohci_scratch, 0, PAGE_SIZE);
    c->ohci_int_td_top = OHCI_MAX_TD;

    c->ohci_ctrl_ed = ohci_alloc_ed(c, &c->ohci_ctrl_ed_phys);
    if (!c->ohci_ctrl_ed) return false;

    pci_dev_enable(dev, PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);

    uint32_t ctl = mmio_r32(mmio, OHCI_CONTROL);
    if (ctl & OHCI_CTL_IR) {
        kprintf("[usb-legacy] OHCI %02x:%02x.%x: SMM owns controller; "
                "leaving BIOS legacy input in place\n",
                dev->bus, dev->slot, dev->fn);
        c->bios_owned = true;
        return false;
    }

    mmio_w32(mmio, OHCI_INTR_DISABLE, OHCI_INTR_MIE);
    mmio_w32(mmio, OHCI_CONTROL, OHCI_CTL_USB_RESET);
    pit_sleep_ms(10);
    mmio_w32(mmio, OHCI_CMDSTATUS, OHCI_CMD_HCR);
    for (int i = 0; i < 100; i++) {
        if (!(mmio_r32(mmio, OHCI_CMDSTATUS) & OHCI_CMD_HCR)) break;
        pit_sleep_ms(1);
    }
    mmio_w32(mmio, OHCI_HCCA, (uint32_t)c->ohci_hcca_phys);
    mmio_w32(mmio, OHCI_CONTROL_HEAD_ED, (uint32_t)c->ohci_ctrl_ed_phys);
    mmio_w32(mmio, OHCI_INTR_STATUS, 0xFFFFFFFFu);
    uint32_t fm = mmio_r32(mmio, OHCI_FM_INTERVAL);
    if (fm) {
        mmio_w32(mmio, OHCI_PERIODIC_START, (fm & 0x3FFFu) * 9u / 10u);
    }
    mmio_w32(mmio, OHCI_CONTROL, OHCI_CTL_USB_OPER |
                                OHCI_CTL_CLE |
                                OHCI_CTL_PLE);
    pit_sleep_ms(20);

    c->ohci_online = true;
    kprintf("[usb-legacy] OHCI online mmio=%p rev=0x%02x ports=%u\n",
            (void *)dev->bar[0], (unsigned)c->version, (unsigned)c->ports);

    uint8_t nports = c->ports;
    if (nports > OHCI_MAX_DEV) nports = OHCI_MAX_DEV;
    for (uint8_t p = 1; p <= nports; p++) {
        (void)ohci_enumerate_port(c, p);
    }
    return true;
}

static uint8_t ehci_cap_r8(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint8_t *)(base + off);
}

static uint16_t ehci_cap_r16(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint16_t *)(base + off);
}

static uint32_t ehci_cap_r32(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}

static struct ehci_qh *ehci_alloc_qh(struct legacy_usb_ctl *c,
                                     uint64_t *phys_out) {
    if (!c || c->ehci_qh_used >= EHCI_MAX_QH) return 0;
    int idx = c->ehci_qh_used++;
    struct ehci_qh *qh = &c->ehci_qh_pool[idx];
    memset(qh, 0, sizeof(*qh));
    if (phys_out) *phys_out = c->ehci_qh_phys + (uint64_t)idx * sizeof(*qh);
    qh->horiz = EHCI_LINK_TERM;
    qh->next = EHCI_LINK_TERM;
    qh->alt_next = EHCI_LINK_TERM;
    return qh;
}

static struct ehci_qtd *ehci_alloc_qtd(struct legacy_usb_ctl *c,
                                       uint64_t *phys_out) {
    if (!c || c->ehci_qtd_used >= EHCI_MAX_QTD) return 0;
    int idx = c->ehci_qtd_used++;
    struct ehci_qtd *qtd = &c->ehci_qtd_pool[idx];
    memset(qtd, 0, sizeof(*qtd));
    if (phys_out) *phys_out = c->ehci_qtd_phys + (uint64_t)idx * sizeof(*qtd);
    qtd->next = EHCI_LINK_TERM;
    qtd->alt_next = EHCI_LINK_TERM;
    return qtd;
}

static struct ehci_qtd *ehci_alloc_int_qtd(struct legacy_usb_ctl *c,
                                           uint64_t *phys_out) {
    if (!c || c->ehci_int_qtd_top <= 64) return 0;
    int idx = --c->ehci_int_qtd_top;
    struct ehci_qtd *qtd = &c->ehci_qtd_pool[idx];
    memset(qtd, 0, sizeof(*qtd));
    if (phys_out) *phys_out = c->ehci_qtd_phys + (uint64_t)idx * sizeof(*qtd);
    qtd->next = EHCI_LINK_TERM;
    qtd->alt_next = EHCI_LINK_TERM;
    return qtd;
}

static uint32_t ehci_qh_epchar(uint8_t addr, uint8_t ep,
                               uint16_t mps, bool async_head) {
    return ((uint32_t)addr & 0x7Fu) |
           (((uint32_t)ep & 0x0Fu) << 8) |
           (2u << 12) |                 /* high speed */
           (1u << 14) |                 /* data toggle from qTD */
           (async_head ? (1u << 15) : 0) |
           (((uint32_t)mps & 0x07FFu) << 16) |
           (8u << 28);                  /* NAK reload count */
}

static uint32_t ehci_qh_epcap_interrupt(void) {
    return 0x01u | (1u << 30);          /* microframe 0, one transaction */
}

static void ehci_qtd_fill(struct ehci_qtd *qtd, uint64_t next_phys,
                          uint8_t pid, bool data1, void *buf,
                          uint16_t len, bool ioc) {
    uint32_t phys = 0;
    qtd->next = next_phys ? (uint32_t)next_phys : EHCI_LINK_TERM;
    qtd->alt_next = EHCI_LINK_TERM;
    qtd->token = EHCI_QTD_ACTIVE |
                 ((uint32_t)pid << 8) |
                 (3u << 10) |
                 (ioc ? EHCI_QTD_IOC : 0) |
                 ((uint32_t)len << 16) |
                 (data1 ? EHCI_QTD_DT : 0);
    if (buf && len) {
        phys = (uint32_t)vmm_translate((uint64_t)buf);
    }
    uint32_t base = phys & ~0xFFFu;
    for (int i = 0; i < 5; i++) {
        qtd->buf[i] = phys ? (base + (uint32_t)i * 0x1000u) : 0;
    }
    if (phys) qtd->buf[0] = phys;
}

static bool ehci_wait_qtds(struct legacy_usb_ctl *c, struct ehci_qtd *first,
                           int count, const char *what) {
    for (int spin = 0; spin < 1000; spin++) {
        bool active = false;
        for (int i = 0; i < count; i++) {
            if (first[i].token & EHCI_QTD_ACTIVE) {
                active = true;
                break;
            }
        }
        if (!active) break;
        pit_sleep_ms(1);
    }

    uint32_t err = EHCI_QTD_HALTED | EHCI_QTD_DBE | EHCI_QTD_BABBLE |
                   EHCI_QTD_XACT | EHCI_QTD_MMF;
    for (int i = 0; i < count; i++) {
        uint32_t tok = first[i].token;
        if (tok & EHCI_QTD_ACTIVE) {
            kprintf("[usb-legacy] EHCI %s timeout qTD=%d tok=0x%08x\n",
                    what, i, tok);
            return false;
        }
        if (tok & err) {
            kprintf("[usb-legacy] EHCI %s error qTD=%d tok=0x%08x\n",
                    what, i, tok);
            return false;
        }
    }
    mmio_w32(c->ehci_op, EHCI_OP_USBSTS, 0x0000003Fu);
    return true;
}

static bool ehci_control_xfer(struct usb_device *dev,
                              uint8_t bm_request_type, uint8_t b_request,
                              uint16_t w_value, uint16_t w_index,
                              void *buf, uint16_t w_length) {
    if (!dev || !dev->hci_priv) return false;
    struct legacy_usb_ctl *c = (struct legacy_usb_ctl *)dev->hci_priv;
    if (!c->ehci_online || !c->ehci_async_qh) return false;

    c->ehci_qtd_used = 0;
    bool dir_in = (bm_request_type & USB_DIR_IN) != 0;
    uint16_t mps = dev->mps0 ? dev->mps0 : 64u;

    struct usb_setup_packet *setup =
        (struct usb_setup_packet *)c->ehci_scratch;
    setup->bmRequestType = bm_request_type;
    setup->bRequest = b_request;
    setup->wValue = w_value;
    setup->wIndex = w_index;
    setup->wLength = w_length;

    int qtd_count = 1 + ((w_length + mps - 1u) / mps) + 1;
    if (qtd_count > EHCI_MAX_QTD) return false;

    uint64_t phys[EHCI_MAX_QTD];
    struct ehci_qtd *qtd0 = ehci_alloc_qtd(c, &phys[0]);
    if (!qtd0) return false;
    for (int i = 1; i < qtd_count; i++) {
        if (!ehci_alloc_qtd(c, &phys[i])) return false;
    }

    int ix = 0;
    ehci_qtd_fill(&qtd0[ix], phys[ix + 1], EHCI_QTD_PID_SETUP,
                  false, setup, sizeof(*setup), false);
    ix++;

    uint8_t *p = (uint8_t *)buf;
    uint16_t left = w_length;
    bool data1 = true;
    while (left > 0) {
        uint16_t chunk = left > mps ? mps : left;
        ehci_qtd_fill(&qtd0[ix], phys[ix + 1],
                      dir_in ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT,
                      data1, p, chunk, false);
        p += chunk;
        left -= chunk;
        data1 = !data1;
        ix++;
    }

    ehci_qtd_fill(&qtd0[ix], 0,
                  dir_in ? EHCI_QTD_PID_OUT : EHCI_QTD_PID_IN,
                  true, 0, 0, true);

    c->ehci_async_qh->epchar =
        ehci_qh_epchar(dev->bus_address, 0, mps, true);
    c->ehci_async_qh->epcap = 1u << 30;
    c->ehci_async_qh->current = 0;
    c->ehci_async_qh->next = (uint32_t)phys[0];
    c->ehci_async_qh->alt_next = EHCI_LINK_TERM;
    c->ehci_async_qh->token = 0;
    for (int i = 0; i < 5; i++) c->ehci_async_qh->buf[i] = 0;

    uint32_t cmd = mmio_r32(c->ehci_op, EHCI_OP_USBCMD);
    if (!(cmd & EHCI_CMD_ASE)) {
        mmio_w32(c->ehci_op, EHCI_OP_USBCMD, cmd | EHCI_CMD_ASE);
    }

    bool ok = ehci_wait_qtds(c, qtd0, qtd_count, "control");
    c->ehci_async_qh->next = EHCI_LINK_TERM;
    c->ehci_async_qh->token = 0;
    return ok;
}

static bool ehci_get_desc(struct usb_device *dev, uint8_t type,
                          void *buf, uint16_t len) {
    return ehci_control_xfer(dev, USB_DIR_IN | USB_TYPE_STANDARD |
                                  USB_RECIP_DEVICE,
                             USB_REQ_GET_DESCRIPTOR,
                             (uint16_t)type << 8, 0, buf, len);
}

static bool ehci_set_address(struct usb_device *dev, uint8_t addr) {
    if (!ehci_control_xfer(dev, USB_DIR_OUT | USB_TYPE_STANDARD |
                                USB_RECIP_DEVICE,
                           USB_REQ_SET_ADDRESS, addr, 0, 0, 0)) {
        return false;
    }
    pit_sleep_ms(10);
    dev->bus_address = addr;
    return true;
}

static bool ehci_set_config(struct usb_device *dev, uint8_t cfg) {
    return ehci_control_xfer(dev, USB_DIR_OUT | USB_TYPE_STANDARD |
                                  USB_RECIP_DEVICE,
                             USB_REQ_SET_CONFIGURATION, cfg, 0, 0, 0);
}

static void ehci_submit_int_in(struct usb_device *dev) {
    if (!dev || !dev->hci_priv || !dev->int_buf || dev->int_armed) return;
    struct legacy_usb_ctl *c = (struct legacy_usb_ctl *)dev->hci_priv;
    struct ehci_dev *ed = (struct ehci_dev *)dev->hci_dev_priv;
    if (!c->ehci_online || !ed || !ed->int_qh || !ed->int_qtd) return;

    ehci_qtd_fill(ed->int_qtd, 0, EHCI_QTD_PID_IN, true,
                  dev->int_buf, dev->int_buf_size, true);
    ed->int_qh->horiz = EHCI_LINK_TERM;
    ed->int_qh->epchar =
        ehci_qh_epchar(dev->bus_address, USB_EP_NUM(ed->ep_addr),
                       ed->mps ? ed->mps : dev->int_buf_size, false);
    ed->int_qh->epcap = ehci_qh_epcap_interrupt();
    ed->int_qh->current = 0;
    ed->int_qh->next = (uint32_t)ed->int_qtd_phys;
    ed->int_qh->alt_next = EHCI_LINK_TERM;
    ed->int_qh->token = 0;
    for (int i = 0; i < 5; i++) ed->int_qh->buf[i] = 0;

    for (int i = 0; i < 1024; i++) {
        c->ehci_periodic[i] = (uint32_t)ed->int_qh_phys | EHCI_LINK_QH;
    }

    uint32_t cmd = mmio_r32(c->ehci_op, EHCI_OP_USBCMD);
    if (!(cmd & EHCI_CMD_PSE)) {
        mmio_w32(c->ehci_op, EHCI_OP_USBCMD, cmd | EHCI_CMD_PSE);
    }
    dev->int_armed = true;
}

static void ehci_poll_device(struct legacy_usb_ctl *c, struct ehci_dev *ed) {
    if (!ed->in_use || !ed->usb.int_armed || !ed->int_qtd) return;
    uint32_t tok = ed->int_qtd->token;
    if (tok & EHCI_QTD_ACTIVE) return;

    ed->usb.int_armed = false;
    ed->int_qh->next = EHCI_LINK_TERM;

    uint32_t err = EHCI_QTD_HALTED | EHCI_QTD_DBE | EHCI_QTD_BABBLE |
                   EHCI_QTD_XACT | EHCI_QTD_MMF;
    if (!(tok & err)) {
        uint32_t remaining = (tok >> 16) & 0x7FFFu;
        uint32_t actual = ed->usb.int_buf_size;
        if (remaining < actual) actual -= remaining;
        if (ed->usb.int_complete) {
            ed->usb.int_complete(&ed->usb, ed->usb.int_buf, actual);
        }
    }
    mmio_w32(c->ehci_op, EHCI_OP_USBSTS, 0x0000003Fu);
    ehci_submit_int_in(&ed->usb);
}

static struct ehci_dev *ehci_alloc_dev(struct legacy_usb_ctl *c) {
    for (int i = 0; i < EHCI_MAX_DEV; i++) {
        if (!c->ehci_devs[i].in_use) {
            memset(&c->ehci_devs[i], 0, sizeof(c->ehci_devs[i]));
            c->ehci_devs[i].in_use = true;
            return &c->ehci_devs[i];
        }
    }
    return 0;
}

static bool ehci_probe_hid_config(struct legacy_usb_ctl *c,
                                  struct ehci_dev *ed,
                                  struct usb_device_desc *dd) {
    uint8_t *cfg = c->ehci_scratch;
    memset(cfg, 0, PAGE_SIZE);
    if (!ehci_get_desc(&ed->usb, USB_DESC_CONFIG, cfg, 9)) return false;

    struct usb_config_desc *cd = (struct usb_config_desc *)cfg;
    uint16_t total = cd->wTotalLength;
    if (total > PAGE_SIZE) total = PAGE_SIZE;
    if (total > 9 && !ehci_get_desc(&ed->usb, USB_DESC_CONFIG, cfg, total)) {
        return false;
    }
    if (!ehci_set_config(&ed->usb, cd->bConfigurationValue)) return false;

    uint16_t off = cd->bLength;
    const struct usb_iface_desc *iface = 0;
    while (off + 2u <= total) {
        uint8_t len = cfg[off];
        uint8_t typ = cfg[off + 1];
        if (len == 0 || off + len > total) break;
        if (typ == USB_DESC_INTERFACE) {
            iface = (const struct usb_iface_desc *)(cfg + off);
        } else if (typ == USB_DESC_ENDPOINT && iface &&
                   iface->bInterfaceClass == USB_CLASS_HID) {
            const struct usb_endpoint_desc *ep =
                (const struct usb_endpoint_desc *)(cfg + off);
            if (USB_EP_TYPE(ep->bmAttributes) == USB_EP_INTERRUPT &&
                USB_EP_DIR_IN(ep->bEndpointAddress) &&
                usb_hid_probe(&ed->usb, iface, ep)) {
                uint64_t qhp = 0, qtdp = 0;
                ed->int_qh = ehci_alloc_qh(c, &qhp);
                ed->int_qtd = ehci_alloc_int_qtd(c, &qtdp);
                if (!ed->int_qh || !ed->int_qtd) return false;
                ed->int_qh_phys = qhp;
                ed->int_qtd_phys = qtdp;
                ed->ep_addr = ep->bEndpointAddress;
                ed->mps = ep->wMaxPacketSize & 0x07FFu;
                if (ed->mps == 0) ed->mps = ed->usb.int_buf_size;
                ehci_submit_int_in(&ed->usb);
                kprintf("[usb-legacy] EHCI high-speed HID %04x:%04x "
                        "addr=%u port=%u ep=0x%02x\n",
                        dd->idVendor, dd->idProduct, ed->addr, ed->port,
                        ed->ep_addr);
                return true;
            }
        }
        off += len;
    }
    return false;
}

static bool ehci_enumerate_port(struct legacy_usb_ctl *c, uint8_t port) {
    uint8_t pidx = (uint8_t)(port - 1u);
    uint32_t ps = mmio_r32(c->ehci_op, EHCI_OP_PORTSC(pidx));
    if (!(ps & EHCI_PORT_CCS)) return false;

    mmio_w32(c->ehci_op, EHCI_OP_PORTSC(pidx),
             (ps & ~EHCI_PORT_RW1C) | EHCI_PORT_PP | EHCI_PORT_PR);
    pit_sleep_ms(60);
    ps = mmio_r32(c->ehci_op, EHCI_OP_PORTSC(pidx));
    mmio_w32(c->ehci_op, EHCI_OP_PORTSC(pidx),
             (ps & ~(EHCI_PORT_RW1C | EHCI_PORT_PR)) | EHCI_PORT_PP);
    pit_sleep_ms(20);
    ps = mmio_r32(c->ehci_op, EHCI_OP_PORTSC(pidx));

    if (!(ps & EHCI_PORT_PE)) {
        mmio_w32(c->ehci_op, EHCI_OP_PORTSC(pidx),
                 (ps & ~EHCI_PORT_RW1C) | EHCI_PORT_OWNER);
        kprintf("[usb-legacy] EHCI port %u routed to companion HCI "
                "(PORTSC=0x%08x)\n", (unsigned)port, ps);
        return false;
    }

    struct ehci_dev *ed = ehci_alloc_dev(c);
    if (!ed) return false;
    ed->port = port;
    ed->addr = ++c->ehci_next_addr;
    if (c->ehci_next_addr == 0 || c->ehci_next_addr > 120) {
        c->ehci_next_addr = 1;
    }

    ed->usb.slot_id = (uint8_t)(0x60u | ed->addr);
    ed->usb.port_id = port;
    ed->usb.speed = USB_SPEED_HIGH;
    ed->usb.mps0 = 64;
    ed->usb.control_xfer = ehci_control_xfer;
    ed->usb.submit_int_in = ehci_submit_int_in;
    ed->usb.hci_priv = c;
    ed->usb.hci_dev_priv = ed;

    struct usb_device_desc *dd = (struct usb_device_desc *)c->ehci_scratch;
    memset(dd, 0, sizeof(*dd));
    if (!ehci_get_desc(&ed->usb, USB_DESC_DEVICE, dd, 8)) {
        kprintf("[usb-legacy] EHCI port %u: device descriptor probe failed\n",
                port);
        ed->in_use = false;
        return false;
    }
    if (dd->bMaxPacketSize0) ed->usb.mps0 = dd->bMaxPacketSize0;
    if (!ehci_set_address(&ed->usb, ed->addr)) {
        kprintf("[usb-legacy] EHCI port %u: SET_ADDRESS failed\n", port);
        ed->in_use = false;
        return false;
    }
    if (!ehci_get_desc(&ed->usb, USB_DESC_DEVICE, dd, sizeof(*dd))) {
        kprintf("[usb-legacy] EHCI port %u: full device descriptor failed\n",
                port);
        ed->in_use = false;
        return false;
    }

    kprintf("[usb-legacy] EHCI port %u: addr=%u speed=high vid=%04x pid=%04x class=%02x\n",
            port, ed->addr, dd->idVendor, dd->idProduct, dd->bDeviceClass);
    if (!ehci_probe_hid_config(c, ed, dd)) {
        kprintf("[usb-legacy] EHCI port %u: no boot HID driver claimed device\n",
                port);
    }
    return true;
}

static void ehci_scan_ports(struct legacy_usb_ctl *c) {
    if (!c->ehci_op || c->ports == 0) return;
    uint8_t nports = c->ports;
    if (nports > EHCI_MAX_DEV) nports = EHCI_MAX_DEV;
    for (uint8_t p = 1; p <= nports; p++) {
        (void)ehci_enumerate_port(c, p);
    }
}

static void ehci_probe_legacy_ownership(struct pci_dev *dev,
                                        struct legacy_usb_ctl *c) {
    volatile uint8_t *bar = (volatile uint8_t *)pci_map_bar(dev, 0, 0x1000);
    if (!bar) {
        kprintf("[usb-legacy] EHCI %02x:%02x.%x: BAR0 map failed; "
                "leaving firmware state untouched\n",
                dev->bus, dev->slot, dev->fn);
        return;
    }

    uint8_t caplen = ehci_cap_r8(bar, EHCI_CAP_CAPLENGTH);
    uint32_t hcs = ehci_cap_r32(bar, EHCI_CAP_HCSPARAMS);
    uint32_t hcc = ehci_cap_r32(bar, EHCI_CAP_HCCPARAMS);
    uint8_t eecp = (uint8_t)((hcc >> EHCI_HCC_EECP_SHIFT) & EHCI_HCC_EECP_MASK);

    c->version = ehci_cap_r16(bar, EHCI_CAP_HCIVERSION);
    c->ports = (uint8_t)(hcs & 0x0Fu);

    if (caplen < 0x10 || eecp == 0) {
        return;
    }

    uint32_t legsup = pci_cfg_read32(dev->bus, dev->slot, dev->fn, eecp);
    c->bios_owned = (legsup & EHCI_LEGSUP_BIOS_OWNED) != 0;
    c->os_owned = (legsup & EHCI_LEGSUP_OS_OWNED) != 0;

    if (c->bios_owned && !c->os_owned) {
        kprintf("[usb-legacy] EHCI %02x:%02x.%x: BIOS owns legacy USB "
                "emulation; preserving it for keyboard/mouse input\n",
                dev->bus, dev->slot, dev->fn);
    }
}

static bool legacy_companions_online(void) {
    for (int i = 0; i < g_count; i++) {
        if (g_ctl[i].uhci_online || g_ctl[i].ohci_online) return true;
    }
    return false;
}

static bool ehci_init_controller(struct pci_dev *dev,
                                 struct legacy_usb_ctl *c) {
    volatile uint8_t *bar = (volatile uint8_t *)pci_map_bar(dev, 0, 0x1000);
    if (!bar) {
        kprintf("[usb-legacy] EHCI %02x:%02x.%x: BAR0 map failed\n",
                dev->bus, dev->slot, dev->fn);
        return false;
    }

    uint8_t caplen = ehci_cap_r8(bar, EHCI_CAP_CAPLENGTH);
    uint32_t hcs = ehci_cap_r32(bar, EHCI_CAP_HCSPARAMS);
    c->version = ehci_cap_r16(bar, EHCI_CAP_HCIVERSION);
    c->ports = (uint8_t)(hcs & 0x0Fu);
    c->ehci_op = bar + caplen;

    if (c->bios_owned && !legacy_companions_online()) {
        kprintf("[usb-legacy] EHCI %02x:%02x.%x: preserving BIOS ownership "
                "because no companion HCI is online yet\n",
                dev->bus, dev->slot, dev->fn);
        return false;
    }

    pci_dev_enable(dev, PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);

    if (c->bios_owned) {
        uint32_t hcc = ehci_cap_r32(bar, EHCI_CAP_HCCPARAMS);
        uint8_t eecp = (uint8_t)((hcc >> EHCI_HCC_EECP_SHIFT) &
                                 EHCI_HCC_EECP_MASK);
        if (eecp) {
            uint32_t legsup = pci_cfg_read32(dev->bus, dev->slot, dev->fn, eecp);
            pci_cfg_write32(dev->bus, dev->slot, dev->fn, eecp,
                            legsup | EHCI_LEGSUP_OS_OWNED);
            for (int i = 0; i < 100; i++) {
                legsup = pci_cfg_read32(dev->bus, dev->slot, dev->fn, eecp);
                if (!(legsup & EHCI_LEGSUP_BIOS_OWNED)) break;
                pit_sleep_ms(10);
            }
            c->bios_owned = (legsup & EHCI_LEGSUP_BIOS_OWNED) != 0;
            c->os_owned = (legsup & EHCI_LEGSUP_OS_OWNED) != 0;
        }
    }

    mmio_w32(c->ehci_op, EHCI_OP_USBCMD, 0);
    for (int i = 0; i < 100; i++) {
        if (mmio_r32(c->ehci_op, EHCI_OP_USBSTS) & EHCI_STS_HCH) break;
        pit_sleep_ms(1);
    }
    mmio_w32(c->ehci_op, EHCI_OP_USBCMD, EHCI_CMD_HCRESET);
    for (int i = 0; i < 100; i++) {
        if (!(mmio_r32(c->ehci_op, EHCI_OP_USBCMD) & EHCI_CMD_HCRESET)) break;
        pit_sleep_ms(1);
    }

    c->ehci_periodic_phys = pmm_alloc_page();
    c->ehci_qh_phys = pmm_alloc_page();
    c->ehci_qtd_phys = pmm_alloc_page();
    c->ehci_scratch_phys = pmm_alloc_page();
    if (!c->ehci_periodic_phys || !c->ehci_qh_phys ||
        !c->ehci_qtd_phys || !c->ehci_scratch_phys) {
        kprintf("[usb-legacy] EHCI OOM during schedule allocation\n");
        return false;
    }
    c->ehci_periodic = (uint32_t *)pmm_phys_to_virt(c->ehci_periodic_phys);
    c->ehci_qh_pool = (struct ehci_qh *)pmm_phys_to_virt(c->ehci_qh_phys);
    c->ehci_qtd_pool = (struct ehci_qtd *)pmm_phys_to_virt(c->ehci_qtd_phys);
    c->ehci_scratch = (uint8_t *)pmm_phys_to_virt(c->ehci_scratch_phys);
    memset(c->ehci_periodic, 0, PAGE_SIZE);
    memset(c->ehci_qh_pool, 0, PAGE_SIZE);
    memset(c->ehci_qtd_pool, 0, PAGE_SIZE);
    memset(c->ehci_scratch, 0, PAGE_SIZE);
    for (int i = 0; i < 1024; i++) c->ehci_periodic[i] = EHCI_LINK_TERM;
    c->ehci_int_qtd_top = EHCI_MAX_QTD;

    c->ehci_async_qh = ehci_alloc_qh(c, &c->ehci_async_qh_phys);
    if (!c->ehci_async_qh) return false;
    c->ehci_async_qh->horiz = (uint32_t)c->ehci_async_qh_phys | EHCI_LINK_QH;
    c->ehci_async_qh->epchar = ehci_qh_epchar(0, 0, 64, true);
    c->ehci_async_qh->epcap = 1u << 30;
    c->ehci_async_qh->next = EHCI_LINK_TERM;
    c->ehci_async_qh->alt_next = EHCI_LINK_TERM;
    c->ehci_async_qh->token = 0;

    mmio_w32(c->ehci_op, EHCI_OP_USBINTR, 0);
    mmio_w32(c->ehci_op, EHCI_OP_USBSTS, 0x0000003Fu);
    mmio_w32(c->ehci_op, EHCI_OP_PERIODIC, (uint32_t)c->ehci_periodic_phys);
    mmio_w32(c->ehci_op, EHCI_OP_ASYNCLIST, (uint32_t)c->ehci_async_qh_phys);
    mmio_w32(c->ehci_op, EHCI_OP_CONFIGFLAG, 1);
    mmio_w32(c->ehci_op, EHCI_OP_USBCMD,
             EHCI_CMD_RUN | EHCI_CMD_ASE | EHCI_CMD_PSE);
    pit_sleep_ms(20);

    c->ehci_online = true;
    kprintf("[usb-legacy] EHCI online mmio=%p version=0x%04x ports=%u\n",
            (void *)dev->bar[0], (unsigned)c->version, (unsigned)c->ports);
    ehci_scan_ports(c);
    return true;
}

static int usb_legacy_probe(struct pci_dev *dev) {
    struct legacy_usb_ctl *c = alloc_ctl(dev);
    if (!c) {
        kprintf("[usb-legacy] too many legacy USB controllers; ignoring "
                "%02x:%02x.%x\n", dev->bus, dev->slot, dev->fn);
        return -1;
    }

    switch (dev->prog_if) {
    case PCI_PROGIF_UHCI:
        g_uhci_count++;
        (void)uhci_init_controller(dev, c);
        break;
    case PCI_PROGIF_OHCI:
        g_ohci_count++;
        (void)ohci_init_controller(dev, c);
        break;
    case PCI_PROGIF_EHCI:
        g_ehci_count++;
        ehci_probe_legacy_ownership(dev, c);
        (void)ehci_init_controller(dev, c);
        break;
    default:
        return -1;
    }

    kprintf("[usb-legacy] %s controller at %02x:%02x.%x vid:did %04x:%04x "
            "irq=%u",
            kind_name(dev->prog_if), dev->bus, dev->slot, dev->fn,
            dev->vendor, dev->device, (unsigned)dev->irq_line);
    if (dev->prog_if == PCI_PROGIF_EHCI) {
        kprintf(" version=0x%04x ports=%u bios=%u os=%u",
                (unsigned)c->version, (unsigned)c->ports,
                c->bios_owned ? 1u : 0u, c->os_owned ? 1u : 0u);
    }
    kprintf("\n");

    dev->driver_data = c;
    return 0;
}

static const struct pci_match g_usb_legacy_matches[] = {
    { PCI_ANY_ID, PCI_ANY_ID,
      PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, PCI_PROGIF_UHCI },
    { PCI_ANY_ID, PCI_ANY_ID,
      PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, PCI_PROGIF_OHCI },
    { PCI_ANY_ID, PCI_ANY_ID,
      PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, PCI_PROGIF_EHCI },
    PCI_MATCH_END,
};

static struct pci_driver g_usb_legacy_driver = {
    .name    = "usb_legacy",
    .matches = g_usb_legacy_matches,
    .probe   = usb_legacy_probe,
    .remove  = 0,
    .flags   = PCI_DRIVER_GENERIC,
};

void usb_legacy_register(void) {
    pci_register_driver(&g_usb_legacy_driver);
}

void usb_legacy_poll(void) {
    for (int i = 0; i < g_count; i++) {
        struct legacy_usb_ctl *c = &g_ctl[i];
        if (!c->in_use) continue;
        if (c->uhci_online) {
            for (int d = 0; d < UHCI_MAX_DEV; d++) {
                uhci_poll_device(c, &c->devs[d]);
            }
        }
        if (c->ohci_online) {
            for (int d = 0; d < OHCI_MAX_DEV; d++) {
                ohci_poll_device(c, &c->ohci_devs[d]);
            }
        }
        if (c->ehci_online) {
            for (int d = 0; d < EHCI_MAX_DEV; d++) {
                ehci_poll_device(c, &c->ehci_devs[d]);
            }
        }
    }
}

int usb_legacy_count(void)      { return g_count; }
int usb_legacy_uhci_count(void) { return g_uhci_count; }
int usb_legacy_ohci_count(void) { return g_ohci_count; }
int usb_legacy_ehci_count(void) { return g_ehci_count; }

int usb_legacy_selftest(char *msg, size_t cap) {
    if (g_count == 0) {
        ksnprintf(msg, cap, "no UHCI/OHCI/EHCI controllers present");
        return ABI_DEVT_SKIP;
    }

    int bios_owned = 0;
    for (int i = 0; i < g_count; i++) {
        if (g_ctl[i].in_use && g_ctl[i].bios_owned) bios_owned++;
    }

    ksnprintf(msg, cap,
              "%d legacy USB HCI(s): uhci=%d ohci=%d ehci=%d bios_legacy=%d",
              g_count, g_uhci_count, g_ohci_count, g_ehci_count,
              bios_owned);
    return 0;
}
