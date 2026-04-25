/* xhci.c -- USB 3.x xHCI host controller driver, polled, no IRQs.
 *
 * Bound through the milestone-21 PCI driver registry. We support
 * the bare minimum needed to enumerate USB-HID Boot keyboards and
 * mice on the root-hub ports of any 1.0/1.1 xHCI controller:
 *
 *   - BIOS handoff via USB Legacy Support extended capability.
 *   - Soft reset, MaxSlotsEn, DCBAA + scratchpad allocation, command
 *     ring, primary event ring (one segment, no IRQs).
 *   - Run controller, walk every PORTSC, reset attached devices.
 *   - For each connected device: Enable Slot, Address Device,
 *     GET_DESCRIPTOR (8 then 18 bytes), GET_CONFIG_DESCRIPTOR (full),
 *     walk interfaces, hand HID interfaces to usb_hid_probe(),
 *     Configure Endpoint, prime the first interrupt-IN transfer.
 *   - From the kernel idle loop, xhci_poll() drains the event ring
 *     and re-arms each HID device's interrupt-IN endpoint after
 *     each completed report.
 *
 * Out of scope (deliberately, for this milestone):
 *   - USB hubs (no SET_HUB_DEPTH, no recursive port enumeration)
 *   - Bulk / Isoch / streams / MSI-X / event ring segments > 1
 *   - Hot-plug after probe (would require Port Status Change Event
 *     handling beyond just clearing the change bits)
 *   - 64-byte device contexts (HCCPARAMS1.CSZ=1) -- we assert CSZ=0,
 *     which covers QEMU's qemu-xhci and most consumer Intel xHCIs.
 *     Real Intel server chips with CSZ=1 will currently log a clear
 *     error and stay unbound; the upgrade is a memcpy-time field-
 *     offset table away.
 *   - Unaligned non-contiguous DMA: every buffer + ring + context
 *     fits in a single page from pmm_alloc_page().
 *
 * QEMU verification: `make run-xhci` boots with `-device qemu-xhci
 * -device usb-kbd -device usb-mouse`, which exposes a synthetic
 * xHCI controller (1b36:000d) with the keyboard + mouse pre-attached
 * to root ports 1 and 2.
 */

#include <tobyos/xhci.h>
#include <tobyos/usb.h>
#include <tobyos/usb_hid.h>
#include <tobyos/usb_msc.h>
#include <tobyos/usb_hub.h>
#include <tobyos/usbreg.h>
#include <tobyos/pci.h>
#include <tobyos/safemode.h>
#include <tobyos/pmm.h>
#include <tobyos/vmm.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/cpu.h>
#include <tobyos/pit.h>
#include <tobyos/irq.h>
#include <tobyos/apic.h>

/* ============================================================== */
/* Capability + Operational + Runtime + Doorbell register offsets */
/* ============================================================== */

/* Capability registers (offsets from BAR0). */
#define XHCI_CAP_CAPLENGTH     0x00    /* 8-bit, length to op regs */
#define XHCI_CAP_HCIVERSION    0x02    /* 16-bit */
#define XHCI_CAP_HCSPARAMS1    0x04    /* 32-bit: max ports / intrs / slots */
#define XHCI_CAP_HCSPARAMS2    0x08    /* 32-bit: scratchpad bufs, ERST max */
#define XHCI_CAP_HCSPARAMS3    0x0C    /* 32-bit: U1/U2 device exit latency */
#define XHCI_CAP_HCCPARAMS1    0x10    /* 32-bit: CSZ, AC64, xECP, ... */
#define XHCI_CAP_DBOFF         0x14    /* 32-bit: doorbell register offset */
#define XHCI_CAP_RTSOFF        0x18    /* 32-bit: runtime register offset */

#define HCSPARAMS1_MAX_SLOTS(v)  ((v)        & 0xFFu)
#define HCSPARAMS1_MAX_INTRS(v) (((v) >>  8) & 0x7FFu)
#define HCSPARAMS1_MAX_PORTS(v) (((v) >> 24) & 0xFFu)
#define HCSPARAMS2_MAX_SCRATCH(v)  ((((v) >> 27) & 0x1Fu) << 5 | (((v) >> 21) & 0x1Fu))
#define HCCPARAMS1_AC64(v)         (((v) >>  0) & 1u)
#define HCCPARAMS1_CSZ(v)          (((v) >>  2) & 1u)
#define HCCPARAMS1_XECP(v)         (((v) >> 16) & 0xFFFFu)  /* in dwords */

/* Operational registers (offsets from BAR0 + CAPLENGTH). */
#define XHCI_OP_USBCMD         0x00    /* 32-bit */
#define XHCI_OP_USBSTS         0x04    /* 32-bit */
#define XHCI_OP_PAGESIZE       0x08    /* 32-bit */
#define XHCI_OP_DNCTRL         0x14    /* 32-bit */
#define XHCI_OP_CRCR_LO        0x18    /* 32-bit (lower half of 64-bit) */
#define XHCI_OP_CRCR_HI        0x1C
#define XHCI_OP_DCBAAP_LO      0x30
#define XHCI_OP_DCBAAP_HI      0x34
#define XHCI_OP_CONFIG         0x38    /* 32-bit, low byte = MaxSlotsEn */
#define XHCI_OP_PORTSC(p)     (0x400 + 0x10 * (p))   /* p = 0-based port */

#define USBCMD_RUN             (1u <<  0)
#define USBCMD_HCRST           (1u <<  1)
#define USBCMD_INTE            (1u <<  2)
#define USBCMD_HSEE            (1u <<  3)

#define USBSTS_HCH             (1u <<  0)
#define USBSTS_HSE             (1u <<  2)
#define USBSTS_EINT            (1u <<  3)
#define USBSTS_PCD             (1u <<  4)
#define USBSTS_CNR             (1u << 11)
#define USBSTS_HCE             (1u << 12)

/* Runtime register offsets. */
#define XHCI_RT_MFINDEX        0x00
#define XHCI_RT_IR0            0x20    /* primary interrupter base */
#define XHCI_IR_IMAN           0x00
#define XHCI_IR_IMOD           0x04
#define XHCI_IR_ERSTSZ         0x08
#define XHCI_IR_ERSTBA_LO      0x10
#define XHCI_IR_ERSTBA_HI      0x14
#define XHCI_IR_ERDP_LO        0x18
#define XHCI_IR_ERDP_HI        0x1C

#define IMAN_IP                (1u <<  0)   /* RW1C: pending interrupt */
#define IMAN_IE                (1u <<  1)   /* interrupt enable */

/* PORTSC bit fields (xHCI 1.1 spec § 5.4.8). */
#define PORTSC_CCS             (1u <<  0)   /* connect status (RO) */
#define PORTSC_PED             (1u <<  1)   /* enabled (RW1C disables) */
#define PORTSC_OCA             (1u <<  3)
#define PORTSC_PR              (1u <<  4)   /* port reset (RW1S) */
#define PORTSC_PLS_MASK        (15u << 5)
#define PORTSC_PP              (1u <<  9)
#define PORTSC_SPEED_MASK      (15u << 10)
#define PORTSC_SPEED(v)       (((v) >> 10) & 0xFu)
#define PORTSC_PIC_MASK        (3u << 14)
#define PORTSC_LWS             (1u << 16)
#define PORTSC_CSC             (1u << 17)   /* RW1C */
#define PORTSC_PEC             (1u << 18)
#define PORTSC_WRC             (1u << 19)
#define PORTSC_OCC             (1u << 20)
#define PORTSC_PRC             (1u << 21)
#define PORTSC_PLC             (1u << 22)
#define PORTSC_CEC             (1u << 23)
#define PORTSC_RW1C_MASK       (PORTSC_CSC | PORTSC_PEC | PORTSC_WRC | \
                                PORTSC_OCC | PORTSC_PRC | PORTSC_PLC | \
                                PORTSC_CEC)

/* Speed codes returned in PORTSC[13:10]. */
#define USB_SPEED_FULL    1
#define USB_SPEED_LOW     2
#define USB_SPEED_HIGH    3
#define USB_SPEED_SUPER   4

/* ============================================================== */
/* TRB (Transfer Request Block) -- the universal xHCI ring entry. */
/* ============================================================== */

struct __attribute__((packed)) trb {
    uint32_t param_lo;
    uint32_t param_hi;
    uint32_t status;
    uint32_t control;
};
_Static_assert(sizeof(struct trb) == 16, "TRB must be 16 bytes");

/* Control field (DW3) bit positions. Cycle bit is bit 0; TRB type
 * is in bits 15:10; remaining bits vary per TRB type. */
#define TRB_CYCLE              (1u <<  0)
#define TRB_ENT                (1u <<  1)   /* Evaluate Next TRB */
#define TRB_ISP                (1u <<  2)   /* Interrupt on Short Packet */
#define TRB_NS                 (1u <<  3)   /* No Snoop */
#define TRB_CH                 (1u <<  4)   /* Chain bit */
#define TRB_IOC                (1u <<  5)   /* Interrupt On Completion */
#define TRB_IDT                (1u <<  6)   /* Immediate Data (Setup TRB) */
#define TRB_TC                 (1u <<  1)   /* Toggle Cycle (LINK TRB only) */
#define TRB_TYPE(t)           ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c)       (((c) >> 10) & 0x3Fu)
#define TRB_GET_SLOT(c)       (((c) >> 24) & 0xFFu)
#define TRB_GET_EP(c)         (((c) >> 16) & 0x1Fu)
#define TRB_SLOT(s)           ((uint32_t)(s) << 24)
#define TRB_EP(e)             ((uint32_t)(e) << 16)
#define TRB_TRT_NO_DATA        (0u << 16)
#define TRB_TRT_OUT_DATA       (2u << 16)
#define TRB_TRT_IN_DATA        (3u << 16)
#define TRB_DIR_IN             (1u << 16)   /* Data Stage TRB direction */

/* TRB types we use. */
enum {
    TRB_TYPE_NORMAL          =  1,
    TRB_TYPE_SETUP_STAGE     =  2,
    TRB_TYPE_DATA_STAGE      =  3,
    TRB_TYPE_STATUS_STAGE    =  4,
    TRB_TYPE_LINK            =  6,
    TRB_TYPE_NOOP_TRANSFER   =  8,
    TRB_TYPE_ENABLE_SLOT     =  9,
    TRB_TYPE_DISABLE_SLOT    = 10,
    TRB_TYPE_ADDRESS_DEVICE  = 11,
    TRB_TYPE_CONFIGURE_EP    = 12,
    TRB_TYPE_EVALUATE_CTX    = 13,
    TRB_TYPE_RESET_EP        = 14,    /* M23C: bulk-stall recovery */
    TRB_TYPE_SET_TR_DEQ      = 16,    /* M23C: bulk-stall recovery */
    TRB_TYPE_NOOP_CMD        = 23,
    TRB_TYPE_TRANSFER_EVENT  = 32,
    TRB_TYPE_CMD_COMPLETION  = 33,
    TRB_TYPE_PORT_STATUS     = 34,
};

/* Completion codes (Status field, bits 31:24). */
#define TRB_CC(s)             (((s) >> 24) & 0xFFu)
#define CC_SUCCESS             1
#define CC_DATA_BUFFER_ERR     2
#define CC_BABBLE_ERR          3
#define CC_USB_TX_ERR          4
#define CC_TRB_ERR             5
#define CC_STALL_ERR           6
#define CC_RESOURCE_ERR        7
#define CC_BANDWIDTH_ERR       8
#define CC_NO_SLOTS_ERR        9
#define CC_INV_STREAM_ERR     10
#define CC_SLOT_NOT_ENABLED   11
#define CC_EP_NOT_ENABLED     12
#define CC_SHORT_PACKET       13

/* ============================================================== */
/* Context structures (32-byte variants; CSZ=0)                    */
/* ============================================================== */

struct __attribute__((packed, aligned(32))) xhci_slot_ctx {
    uint32_t dw0;   /* CtxEntries[31:27] | Hub[26] | MTT[25] | Speed[23:20] | RouteString[19:0] */
    uint32_t dw1;   /* NumPorts[31:24] | RootHubPortNumber[23:16] | MaxExitLatency[15:0] */
    uint32_t dw2;   /* InterrupterTarget[31:22] | TTT[17:16] | TTPortNumber[15:8] | ParentHubSlotID[7:0] */
    uint32_t dw3;   /* SlotState[31:27] | USBDeviceAddress[7:0] */
    uint32_t reserved[4];
};
_Static_assert(sizeof(struct xhci_slot_ctx) == 32, "slot ctx 32B");

struct __attribute__((packed, aligned(32))) xhci_ep_ctx {
    uint32_t dw0;   /* Interval[31:24] | LSA[23] | MaxPStreams[22:16] | Mult[15:8] | EPState[2:0] */
    uint32_t dw1;   /* MaxPacketSize[31:16] | MaxBurstSize[15:8] | EPType[5:3] | CErr[2:1] */
    uint32_t deq_lo;/* TR Dequeue Pointer low | DCS[0] */
    uint32_t deq_hi;
    uint32_t dw4;   /* MaxESITPayloadLo[31:16] | AverageTRBLength[15:0] */
    uint32_t reserved[3];
};
_Static_assert(sizeof(struct xhci_ep_ctx) == 32, "ep ctx 32B");

/* Input Control Context, 32 bytes; lives at offset 0 of the Input
 * Context. Add/Drop bitmaps are indexed by DCI (1=EP0, 2=EP1OUT, ...). */
struct __attribute__((packed, aligned(32))) xhci_input_ctrl_ctx {
    uint32_t drop_flags;
    uint32_t add_flags;
    uint32_t reserved[5];
    uint32_t cfg_iface_alt; /* high byte: cfg value, low: iface, mid: alt */
};
_Static_assert(sizeof(struct xhci_input_ctrl_ctx) == 32, "ictl ctx 32B");

/* ERST entry (16 bytes): segment base + size. */
struct __attribute__((packed)) xhci_erst_entry {
    uint64_t base;
    uint32_t size;
    uint32_t reserved;
};
_Static_assert(sizeof(struct xhci_erst_entry) == 16, "ERST entry 16B");

/* ============================================================== */
/* Ring sizing                                                     */
/* ============================================================== */

#define XHCI_MAX_SLOTS         8       /* MaxSlotsEn we ask for */
#define XHCI_MAX_PORTS         32      /* sanity cap on root-hub ports */
#define XHCI_MAX_DEVICES       8

#define XHCI_RING_TRBS         256u    /* 256 * 16 = 4096 = 1 page */
#define XHCI_EVT_TRBS          256u    /* one segment, 256 entries */

/* ============================================================== */
/* Driver state                                                    */
/* ============================================================== */

struct xhci_dev_state {
    bool                in_use;
    struct usb_device   usb;
    /* Backing storage for the device's input + output context, plus
     * EP0 and (optional) interrupt-IN transfer rings. We allocate
     * one PMM page per buffer; pointers are HHDM-translated. */
    struct trb         *ep0_ring;          /* duplicates usb.ep0_ring as typed ptr */
    void               *input_ctx;         /* Input Ctrl + Slot + EPs */
    void               *device_ctx;        /* Slot + EPs only (HC writes back) */
    uint64_t            input_ctx_phys;
    uint64_t            device_ctx_phys;
    struct trb         *int_ring;
    /* On a completed interrupt-IN, the event drain stores the
     * actual transferred length here so usb_hid sees it. */
    uint32_t            last_int_len;
};

static struct {
    bool                bound;

    volatile uint8_t   *cap_regs;          /* BAR0 */
    volatile uint8_t   *op_regs;           /* BAR0 + CAPLENGTH */
    volatile uint8_t   *rt_regs;           /* BAR0 + RTSOFF */
    volatile uint8_t   *db_regs;           /* BAR0 + DBOFF */

    uint32_t            hccparams1;
    uint32_t            page_size_kb;      /* PAGESIZE field, in 4-KiB units */
    uint8_t             max_slots;
    uint8_t             max_ports;
    uint16_t            max_intrs;
    uint16_t            version;
    uint8_t             ctx64;             /* 1 if HCCPARAMS1.CSZ=1 */
    uint16_t            xecp_off;          /* in BYTES from BAR0 */

    /* Device Context Base Address Array. dcbaa[0] = scratchpad
     * pointer; dcbaa[1..MaxSlots] = device context phys addrs. */
    uint64_t           *dcbaa;
    uint64_t            dcbaa_phys;

    /* Scratchpad buffer pointer array + the buffers themselves.
     * Even when MaxScratchpadBufs == 0 we don't allocate any, but we
     * keep dcbaa[0] = 0 then (HC tolerates that). */
    uint64_t           *spad_array;
    uint64_t            spad_array_phys;
    uint32_t            num_scratch;

    /* Command ring (single segment + LINK back to start). */
    struct trb         *cmd_ring;
    uint64_t            cmd_ring_phys;
    uint16_t            cmd_idx;
    uint8_t             cmd_cycle;

    /* Event ring + ERST (single segment). */
    struct trb         *evt_ring;
    uint64_t            evt_ring_phys;
    struct xhci_erst_entry *erst;
    uint64_t            erst_phys;
    uint16_t            evt_idx;
    uint8_t             evt_cycle;

    /* Per-slot state + USB device data. Indexed by slot_id (1-based);
     * slot 0 is unused. */
    struct xhci_dev_state devs[XHCI_MAX_SLOTS + 1];

    /* Last command completion event (synchronous command path). */
    volatile bool       cmd_complete;
    volatile uint8_t    cmd_completion_code;
    volatile uint8_t    cmd_slot_id;

    /* Shared DMA scratch page used during descriptor walks. Lives at
     * an HHDM virt + matching phys so it's safe to feed straight into
     * a Data Stage TRB. Reused across every device probe (probes are
     * synchronous + serialised via xhci_cmd, so no concurrency). */
    uint8_t            *desc_buf;
    uint64_t            desc_buf_phys;

    /* MSI / MSI-X bring-up state. After enumerate-on-probe completes
     * we route IR0 at this vector; until then irq_enabled is false
     * and xhci_poll runs purely from the kernel idle loop / busy-
     * waits in xhci_cmd / xhci_control_class. */
    uint8_t             irq_vector;
    bool                irq_enabled;
    volatile uint64_t   irq_count;

    /* M26C: deferred port-status processing.
     *
     * The xHC raises a Port Status Change TRB on every CSC/PEC/PRC/...
     * RW1C transition. Doing the heavy lifting (Enable Slot, Address
     * Device, descriptor walk, class probes, OR Disable Slot + class
     * unbinds) directly from xhci_poll() would mean issuing commands
     * from MSI context, which deadlocks against xhci_cmd's "wait for
     * Command Completion" spin. Instead we just RW1C-clear the change
     * bits in xhci_poll() and set this bit-per-port; the kernel idle
     * loop calls xhci_service_port_changes() which does the work in
     * normal context. Bit `i` covers root port `i+1`. */
    volatile uint32_t   port_change_pending;
} g_xhci;

/* ============================================================== */
/* MMIO accessors                                                  */
/* ============================================================== */

static inline uint8_t  cap_r8 (uint32_t off) { return *(volatile uint8_t  *)(g_xhci.cap_regs + off); }
static inline uint16_t cap_r16(uint32_t off) { return *(volatile uint16_t *)(g_xhci.cap_regs + off); }
static inline uint32_t cap_r32(uint32_t off) { return *(volatile uint32_t *)(g_xhci.cap_regs + off); }

static inline uint32_t op_r32 (uint32_t off) { return *(volatile uint32_t *)(g_xhci.op_regs  + off); }
static inline void     op_w32 (uint32_t off, uint32_t v) { *(volatile uint32_t *)(g_xhci.op_regs  + off) = v; }

static inline uint32_t rt_r32 (uint32_t off) { return *(volatile uint32_t *)(g_xhci.rt_regs  + off); }
static inline void     rt_w32 (uint32_t off, uint32_t v) { *(volatile uint32_t *)(g_xhci.rt_regs  + off) = v; }

static inline void db_write(uint32_t db_idx, uint32_t v) {
    *(volatile uint32_t *)(g_xhci.db_regs + 4u * db_idx) = v;
}

/* ============================================================== */
/* Scratch helpers: alloc one zeroed page; read/write portsc safely */
/* ============================================================== */

static void *alloc_zero_page(uint64_t *out_phys) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return 0;
    void *virt = pmm_phys_to_virt(phys);
    memset(virt, 0, PAGE_SIZE);
    if (out_phys) *out_phys = phys;
    return virt;
}

static uint32_t portsc_read(uint8_t port_idx) {
    return op_r32(XHCI_OP_PORTSC(port_idx));
}

/* PORTSC writes must mask out RW1C bits (so we don't accidentally
 * clear something the user is observing) AND PED (which is RW1C-
 * disable). Caller passes the bits it wants set, plus any RW1C bits
 * it wants explicitly cleared (e.g. PRC after a port reset). */
static void portsc_write(uint8_t port_idx, uint32_t set_bits) {
    uint32_t v = portsc_read(port_idx);
    v &= ~(PORTSC_PED | PORTSC_RW1C_MASK | PORTSC_LWS);
    v |= set_bits;
    op_w32(XHCI_OP_PORTSC(port_idx), v);
}

/* ============================================================== */
/* BIOS handoff via USB Legacy Support Extended Capability         */
/* ============================================================== */

/* USB Legacy Support cap (id=1) layout, relative to xECP cap base:
 *   +0x00  USBLEGSUP    -- bit  0..7: cap id (1)
 *                           bit  8..15: next cap pointer (dwords)
 *                           bit  16: HC OS Owned Semaphore (we set)
 *                           bit  24: HC BIOS Owned Semaphore (we wait clear)
 *   +0x04  USBLEGCTLSTS -- SMI enable + status bits; we zero. */

#define XECP_ID_LEGACY        1
#define USBLEGSUP_OS_OWNED    (1u << 24)   /* note: at +0x00, bit 24 */
#define USBLEGSUP_BIOS_OWNED  (1u << 16)
/* (Spec actually puts BIOS OWNED at bit 16 and OS OWNED at bit 24,
 * which is the other way round from the more-readable pseudocode --
 * the bit positions above match the on-the-wire register layout.) */

static void xhci_bios_handoff(void) {
    if (!g_xhci.xecp_off) return;
    volatile uint8_t *p = g_xhci.cap_regs + g_xhci.xecp_off;
    for (int hops = 0; hops < 32; hops++) {
        uint32_t cap = *(volatile uint32_t *)p;
        uint8_t  id  = cap & 0xFFu;
        uint8_t  nxt = (cap >> 8) & 0xFFu;
        if (id == XECP_ID_LEGACY) {
            /* Set OS Owned bit (24). Wait for BIOS Owned (16) to drop. */
            *(volatile uint32_t *)p = cap | USBLEGSUP_OS_OWNED;
            for (int i = 0; i < 100; i++) {
                cap = *(volatile uint32_t *)p;
                if (!(cap & USBLEGSUP_BIOS_OWNED)) break;
                pit_sleep_ms(10);
            }
            if (cap & USBLEGSUP_BIOS_OWNED) {
                /* BIOS won't yield. Force ownership by clearing its bit. */
                kprintf("[xhci] WARN: BIOS handoff timed out -- forcing\n");
                *(volatile uint32_t *)p = (cap & ~USBLEGSUP_BIOS_OWNED) |
                                          USBLEGSUP_OS_OWNED;
            }
            /* Clear all SMI sources (USBLEGCTLSTS @ +4). */
            *(volatile uint32_t *)(p + 4) = 0xE0000000u;
            kprintf("[xhci] BIOS handoff complete (xECP @ +0x%x)\n",
                    (unsigned)(p - g_xhci.cap_regs));
            return;
        }
        if (nxt == 0) break;
        p += (uint32_t)nxt * 4u;
    }
}

/* ============================================================== */
/* Ring construction helpers                                       */
/* ============================================================== */

/* Build a ring of XHCI_RING_TRBS entries, with the last slot being a
 * Link TRB that points back to the start with Toggle Cycle set. The
 * caller writes to slots [0 .. XHCI_RING_TRBS-2]; slot
 * XHCI_RING_TRBS-1 stays the Link TRB forever. Returns the virt
 * pointer, writes the phys to *out_phys. */
static struct trb *make_ring(uint64_t *out_phys) {
    uint64_t phys;
    struct trb *r = (struct trb *)alloc_zero_page(&phys);
    if (!r) return 0;
    /* Last TRB is a LINK TRB pointing back to the ring base. TC=1
     * tells the controller to flip its consumer cycle bit on wrap.
     * We do NOT set the cycle bit here -- producer flips it on first
     * write, then again on each subsequent wrap. */
    struct trb *link = &r[XHCI_RING_TRBS - 1];
    link->param_lo = (uint32_t)(phys & 0xFFFFFFFFu);
    link->param_hi = (uint32_t)(phys >> 32);
    link->status   = 0;
    link->control  = TRB_TYPE(TRB_TYPE_LINK) | TRB_TC;
    if (out_phys) *out_phys = phys;
    return r;
}

/* Generic transfer-ring enqueuer. Fills the next slot of `ring`
 * (advancing `*idx` and possibly `*cycle`), and ALSO fixes up the
 * Link TRB's cycle bit on wrap so the controller follows the link
 * exactly once per traversal.
 *
 * Returns the phys address of the TRB we just enqueued (callers use
 * it to match Transfer Events back to outstanding TRBs). */
static uint64_t ring_enqueue(struct trb *ring, uint64_t ring_phys,
                             uint16_t *idx, uint8_t *cycle,
                             uint32_t param_lo, uint32_t param_hi,
                             uint32_t status, uint32_t control_no_cycle) {
    uint16_t i = *idx;
    struct trb *t = &ring[i];
    t->param_lo = param_lo;
    t->param_hi = param_hi;
    t->status   = status;
    /* Set cycle in control AFTER all other fields are visible. The
     * controller polls cycle and consumes the TRB the moment it
     * matches its expected value. */
    uint32_t ctl = control_no_cycle | (*cycle ? TRB_CYCLE : 0);
    t->control = ctl;

    uint64_t trb_phys = ring_phys + (uint64_t)i * sizeof(struct trb);

    /* Advance index. If we just wrote slot N-2 (the last data slot),
     * the next index would be the LINK TRB at N-1. Update that LINK
     * TRB's cycle to current producer cycle, then wrap to index 0
     * and toggle our producer cycle. */
    *idx = (uint16_t)(i + 1u);
    if (*idx == XHCI_RING_TRBS - 1u) {
        struct trb *link = &ring[XHCI_RING_TRBS - 1];
        uint32_t lctl = TRB_TYPE(TRB_TYPE_LINK) | TRB_TC;
        if (*cycle) lctl |= TRB_CYCLE;
        link->control = lctl;
        *idx   = 0;
        *cycle = !*cycle;
    }
    return trb_phys;
}

/* ============================================================== */
/* Command ring + event ring drain                                 */
/* ============================================================== */

/* Enqueue a command TRB, ring doorbell 0, and busy-wait for the
 * matching Command Completion Event from the event ring.
 * Returns the completion code (1 = success). */
static uint8_t xhci_cmd(uint32_t param_lo, uint32_t param_hi,
                        uint32_t control_no_cycle, uint8_t *out_slot_id) {
    g_xhci.cmd_complete        = false;
    g_xhci.cmd_completion_code = 0;
    g_xhci.cmd_slot_id         = 0;

    (void)ring_enqueue(g_xhci.cmd_ring, g_xhci.cmd_ring_phys,
                       &g_xhci.cmd_idx, &g_xhci.cmd_cycle,
                       param_lo, param_hi, 0, control_no_cycle);

    /* Doorbell 0 = host controller (target=command ring). */
    db_write(0, 0);

    /* Drain events until we see the matching Command Completion. We
     * call the same poll routine the idle loop uses; transfer events
     * for in-flight HID polls are dispatched normally. */
    for (int i = 0; i < 1000; i++) {
        xhci_poll();
        if (g_xhci.cmd_complete) break;
        pit_sleep_ms(1);
    }
    if (!g_xhci.cmd_complete) {
        kprintf("[xhci] cmd timeout (control=0x%x)\n",
                (unsigned)control_no_cycle);
        return 0;
    }
    if (out_slot_id) *out_slot_id = g_xhci.cmd_slot_id;
    return g_xhci.cmd_completion_code;
}

/* ============================================================== */
/* Per-device storage                                              */
/* ============================================================== */

/* Allocate a slot in g_xhci.devs that matches the given xHCI slot
 * id (1-based). Returns NULL on overflow. */
static struct xhci_dev_state *get_dev_state(uint8_t slot_id) {
    if (slot_id == 0 || slot_id > XHCI_MAX_SLOTS) return 0;
    return &g_xhci.devs[slot_id];
}

/* ============================================================== */
/* Address Device + per-slot init                                  */
/* ============================================================== */

/* Initial guess for EP0 max packet size based on USB speed. The
 * eventual GET_DESCRIPTOR(8) will tell us the real value and we'll
 * patch the EP context if it differs. */
static uint16_t default_mps0(uint8_t speed) {
    switch (speed) {
    case USB_SPEED_LOW:   return 8;
    case USB_SPEED_FULL:  return 8;     /* could be 8/16/32/64 */
    case USB_SPEED_HIGH:  return 64;
    case USB_SPEED_SUPER: return 512;
    default:              return 8;
    }
}

/* Build the Slot + EP0 contexts inside the input context, then issue
 * Address Device. Block-Set-Address (BSR) is 0 so the controller
 * actually assigns + sets the device address.
 *
 * `port_id` is the *root* hub port number (xHCI Slot Context dw1 bits
 * 23:16 -- the controller uses it to pick which root-hub PORTSC the
 * Setup TRB physically targets). For root-hub-attached devices that
 * is also the port the user plugged the device into; for devices
 * behind a hub it is the root port the topology eventually leads to,
 * and the Route String bits in dw0 tell the controller + intervening
 * hubs which downstream port to forward to. The route string + parent
 * slot id are read directly off `st->usb` so callers don't have to
 * pass redundant arguments (M26B). */
static bool xhci_address_device(struct xhci_dev_state *st,
                                uint8_t port_id, uint8_t speed) {
    /* Input Control: add slot (DCI 0) and EP0 (DCI 1). */
    struct xhci_input_ctrl_ctx *ictl = (struct xhci_input_ctrl_ctx *)st->input_ctx;
    ictl->drop_flags = 0;
    ictl->add_flags  = (1u << 0) | (1u << 1);

    /* Slot context lives at offset 32 inside the input context (first
     * 32 bytes are the input control context). EP0 at offset 64. */
    struct xhci_slot_ctx *slot = (struct xhci_slot_ctx *)((uint8_t *)st->input_ctx + 32);
    struct xhci_ep_ctx   *ep0  = (struct xhci_ep_ctx   *)((uint8_t *)st->input_ctx + 64);

    memset(slot, 0, sizeof(*slot));
    memset(ep0,  0, sizeof(*ep0));

    /* Slot dw0: ContextEntries=1 (only EP0), Speed in 23:20, RouteString in 19:0.
     * The route string is non-zero only for devices behind a hub
     * (M26B); for root-hub devices it is 0 and the controller routes
     * directly to RootHubPortNumber.  */
    uint32_t route = st->usb.route_string & 0x000FFFFFu;
    slot->dw0 = ((uint32_t)1u << 27) | ((uint32_t)speed << 20) | route;
    /* Slot dw1: RootHubPortNumber in 23:16. */
    slot->dw1 = ((uint32_t)port_id << 16);
    slot->dw2 = 0;
    slot->dw3 = 0;

    /* EP0 dw1: MaxPacketSize in 31:16, EPType=4 (Control) in bits 5:3,
     * CErr=3 in bits 2:1. */
    uint16_t mps = st->usb.mps0 ? st->usb.mps0 : default_mps0(speed);
    ep0->dw1 = ((uint32_t)mps << 16) | (4u << 3) | (3u << 1);
    /* EP0 dw0: Interval=0, Mult=0, MaxPStreams=0, EPState=0 (RO). */
    ep0->dw0 = 0;
    /* EP0 transfer ring dequeue pointer + DCS=1. */
    ep0->deq_lo = (uint32_t)(st->usb.ep0_ring_phys & 0xFFFFFFFFu) | 1u;
    ep0->deq_hi = (uint32_t)(st->usb.ep0_ring_phys >> 32);
    /* Average TRB length: a setup TD is roughly 16 bytes; pick a safe
     * 8 for control to be conservative. */
    ep0->dw4 = 8;

    /* Issue Address Device command. Param_lo:hi = phys of input ctx. */
    uint8_t cc = xhci_cmd((uint32_t)(st->input_ctx_phys & 0xFFFFFFFFu),
                          (uint32_t)(st->input_ctx_phys >> 32),
                          TRB_TYPE(TRB_TYPE_ADDRESS_DEVICE) |
                          TRB_SLOT(st->usb.slot_id),
                          0);
    if (cc != CC_SUCCESS) {
        kprintf("[xhci] Address Device failed (slot %u port %u cc=%u)\n",
                st->usb.slot_id, port_id, cc);
        return false;
    }

    /* Read the assigned device address out of the device context the
     * controller just populated. Slot context dw3 bits 7:0. */
    struct xhci_slot_ctx *out_slot = (struct xhci_slot_ctx *)st->device_ctx;
    st->usb.bus_address = (uint8_t)(out_slot->dw3 & 0xFFu);
    return true;
}

/* ============================================================== */
/* Control transfer (synchronous, polls event ring)                */
/* ============================================================== */

/* Build a Setup-Stage + (optional) Data-Stage + Status-Stage TD on
 * the device's EP0 ring, ring the EP0 doorbell, and busy-wait for
 * the Status-Stage transfer event. dir_in only matters when len > 0.
 * Returns true on success. The caller's buf must be DMA-safe (we
 * use vmm_translate via pmm_virt_to_phys; for buffers from
 * pmm_alloc_page that's a no-op). */
static bool g_ep0_complete[XHCI_MAX_SLOTS + 1];
static uint8_t g_ep0_cc[XHCI_MAX_SLOTS + 1];

/* Per-slot, per-DCI completion latch for synchronous BULK transfers.
 * The DCI index space is 1..31 (1 is EP0 / control; 2..31 are the 15
 * pairs of OUT/IN endpoints). xhci_poll() sets these on Transfer
 * Events; xhci_bulk_xfer_sync() spins on `complete` and reads back
 * cc + bytes_left after the spin returns. */
#define XHCI_MAX_DCI 32
static volatile bool     g_bulk_complete[XHCI_MAX_SLOTS + 1][XHCI_MAX_DCI];
static volatile uint8_t  g_bulk_cc      [XHCI_MAX_SLOTS + 1][XHCI_MAX_DCI];
static volatile uint32_t g_bulk_residue [XHCI_MAX_SLOTS + 1][XHCI_MAX_DCI];

/* Shared control-transfer entry. Exported (no `static`) so the HID
 * class driver in usb_hid.c can issue SET_PROTOCOL / SET_IDLE
 * without going through a separate HCI-ops indirection. If a second
 * HCI ever lands the cleanest move is to lift this into a function
 * pointer on struct usb_device, but with one HCI today the direct
 * call avoids an unnecessary layer. */
bool xhci_control_class(struct usb_device *dev,
                        uint8_t bm_request_type, uint8_t b_request,
                        uint16_t w_value, uint16_t w_index,
                        void *buf, uint16_t w_length) {
    bool dir_in = (bm_request_type & USB_DIR_IN) != 0;

    g_ep0_complete[dev->slot_id] = false;
    g_ep0_cc[dev->slot_id]       = 0;

    /* --- Setup Stage TRB. Immediate Data: 8-byte setup packet lives
     * directly in param_lo/param_hi.   */
    uint32_t setup_lo = (uint32_t)bm_request_type
                      | ((uint32_t)b_request << 8)
                      | ((uint32_t)w_value   << 16);
    uint32_t setup_hi = (uint32_t)w_index
                      | ((uint32_t)w_length  << 16);
    uint32_t trt = (w_length == 0) ? TRB_TRT_NO_DATA :
                   (dir_in ? TRB_TRT_IN_DATA : TRB_TRT_OUT_DATA);
    (void)ring_enqueue((struct trb *)dev->ep0_ring, dev->ep0_ring_phys,
                       &dev->ep0_idx, &dev->ep0_cycle,
                       setup_lo, setup_hi,
                       8u,                        /* TRB Transfer Length */
                       TRB_TYPE(TRB_TYPE_SETUP_STAGE) | TRB_IDT | trt);

    /* --- Data Stage TRB (only if len > 0). The buffer can come from
     * any region of kernel virtual space (heap, HHDM-allocated PMM
     * page, kernel-image static); pmm_virt_to_phys only handles HHDM,
     * but vmm_translate walks the real page tables and works for all
     * three. */
    if (w_length > 0) {
        uint64_t buf_phys = vmm_translate((uint64_t)buf);
        uint32_t dctl = TRB_TYPE(TRB_TYPE_DATA_STAGE) |
                        TRB_ISP |
                        (dir_in ? TRB_DIR_IN : 0);
        (void)ring_enqueue((struct trb *)dev->ep0_ring, dev->ep0_ring_phys,
                           &dev->ep0_idx, &dev->ep0_cycle,
                           (uint32_t)(buf_phys & 0xFFFFFFFFu),
                           (uint32_t)(buf_phys >> 32),
                           w_length,              /* TRB Transfer Length */
                           dctl);
    }

    /* --- Status Stage TRB. Direction is opposite of Data Stage; if
     * there was no data stage, direction is IN. We always set IOC so
     * the event ring tells us this control TD is done. */
    uint32_t sctl = TRB_TYPE(TRB_TYPE_STATUS_STAGE) | TRB_IOC;
    bool status_in = (w_length == 0) ? true : !dir_in;
    if (status_in) sctl |= TRB_DIR_IN;
    (void)ring_enqueue((struct trb *)dev->ep0_ring, dev->ep0_ring_phys,
                       &dev->ep0_idx, &dev->ep0_cycle,
                       0, 0, 0, sctl);

    /* Doorbell EP0: target = DCI 1. */
    db_write(dev->slot_id, 1u);

    /* Drain events until we see the matching transfer event. */
    for (int i = 0; i < 1000; i++) {
        xhci_poll();
        if (g_ep0_complete[dev->slot_id]) break;
        pit_sleep_ms(1);
    }
    if (!g_ep0_complete[dev->slot_id]) {
        kprintf("[xhci] ctrl xfer timeout (slot %u, req 0x%x/0x%x)\n",
                dev->slot_id, bm_request_type, b_request);
        return false;
    }
    if (g_ep0_cc[dev->slot_id] != CC_SUCCESS &&
        g_ep0_cc[dev->slot_id] != CC_SHORT_PACKET) {
        kprintf("[xhci] ctrl xfer NACK (slot %u, req 0x%x/0x%x cc=%u)\n",
                dev->slot_id, bm_request_type, b_request,
                g_ep0_cc[dev->slot_id]);
        return false;
    }
    return true;
}

/* ============================================================== */
/* Configure Endpoint (for the HID interrupt-IN endpoint)          */
/* ============================================================== */

/* Build EP context for the chosen interrupt-IN endpoint at DCI = ep_dci,
 * point its dequeue pointer at a fresh transfer ring, then issue
 * Configure Endpoint to commit the change to the device context. */
static bool xhci_configure_int_in(struct xhci_dev_state *st,
                                  uint8_t ep_dci,
                                  uint16_t mps, uint8_t interval) {
    /* Build a fresh interrupt-IN ring. */
    uint64_t int_phys;
    struct trb *int_ring = make_ring(&int_phys);
    if (!int_ring) {
        kprintf("[xhci] OOM for INT-IN ring\n");
        return false;
    }
    st->int_ring             = int_ring;
    st->usb.int_ring         = int_ring;
    st->usb.int_ring_phys    = int_phys;
    st->usb.int_idx          = 0;
    st->usb.int_cycle        = 1;

    /* Input Control: bump ContextEntries on the slot, add the new EP. */
    struct xhci_input_ctrl_ctx *ictl = (struct xhci_input_ctrl_ctx *)st->input_ctx;
    ictl->drop_flags = 0;
    ictl->add_flags  = (1u << 0) | (1u << ep_dci);

    /* Pull current slot context from the OUTPUT (device) context --
     * the controller wrote the actual values during Address Device.
     * We only need to bump ContextEntries to >= ep_dci. */
    struct xhci_slot_ctx *out_slot = (struct xhci_slot_ctx *)st->device_ctx;
    struct xhci_slot_ctx *in_slot  = (struct xhci_slot_ctx *)((uint8_t *)st->input_ctx + 32);
    *in_slot = *out_slot;
    /* Bits 31:27 = ContextEntries. Set to ep_dci. */
    in_slot->dw0 = (in_slot->dw0 & ~0xF8000000u) | ((uint32_t)ep_dci << 27);

    /* Build the EP context. Each EP context is 32 bytes (CSZ=0); EP
     * at DCI=N lives at offset 32 + 32*N in the input context. */
    struct xhci_ep_ctx *ep =
        (struct xhci_ep_ctx *)((uint8_t *)st->input_ctx + 32 + 32 * ep_dci);
    memset(ep, 0, sizeof(*ep));

    /* Interval field encoding: for high/full/low-speed interrupt
     * endpoints the spec wants log2(interval-in-microframes). The
     * descriptor's bInterval is in frames (1ms each) for FS/LS, in
     * 125us microframes for HS. We just clamp to a useful range. */
    uint8_t enc_interval = interval ? interval - 1 : 3;
    if (enc_interval > 15) enc_interval = 15;
    ep->dw0 = ((uint32_t)enc_interval << 16);   /* Interval[31:16 in xhci 1.1; spec actual is 23:16 -- both work for the values we use */
    ep->dw1 = ((uint32_t)mps << 16) | (7u << 3) | (3u << 1); /* EPType=Int IN, CErr=3 */
    ep->deq_lo = (uint32_t)(int_phys & 0xFFFFFFFFu) | 1u;     /* DCS=1 */
    ep->deq_hi = (uint32_t)(int_phys >> 32);
    ep->dw4 = ((uint32_t)mps << 16) | mps;                    /* MaxESITPayloadLo + AvgTRBLength */

    uint8_t cc = xhci_cmd((uint32_t)(st->input_ctx_phys & 0xFFFFFFFFu),
                          (uint32_t)(st->input_ctx_phys >> 32),
                          TRB_TYPE(TRB_TYPE_CONFIGURE_EP) |
                          TRB_SLOT(st->usb.slot_id),
                          0);
    if (cc != CC_SUCCESS) {
        kprintf("[xhci] Configure Endpoint failed (slot %u DCI %u cc=%u)\n",
                st->usb.slot_id, ep_dci, cc);
        return false;
    }
    return true;
}

/* ============================================================== */
/* Bulk endpoint configuration + synchronous bulk transfer         */
/* (used by the USB Mass-Storage Class driver, src/usb_msc.c)      */
/* ============================================================== */

/* Configure both bulk-IN and bulk-OUT endpoints in a single
 * Configure-Endpoint command. After Address-Device leaves us in
 * Addressed state with only EP0 valid, MSC needs us to add two more
 * endpoints (a pair of bulk pipes). xHCI lets us do this with a
 * single doorbell-less command -- one input context populated for
 * BOTH new EPs, one Configure-Endpoint TRB.
 *
 * The dev_state's input context is reused (it was zeroed at probe);
 * fresh transfer rings are allocated and pinned into the device's
 * usb_device fields so future bulk_xfer calls can ring them. */
static bool xhci_configure_bulk_pair(struct xhci_dev_state *st,
                                     uint8_t  in_dci,  uint16_t in_mps,
                                     uint8_t  out_dci, uint16_t out_mps) {
    uint64_t in_phys, out_phys;
    struct trb *in_ring  = make_ring(&in_phys);
    struct trb *out_ring = make_ring(&out_phys);
    if (!in_ring || !out_ring) {
        kprintf("[xhci] OOM for BULK rings\n");
        return false;
    }
    st->usb.bulk_in_ring       = in_ring;
    st->usb.bulk_in_ring_phys  = in_phys;
    st->usb.bulk_in_idx        = 0;
    st->usb.bulk_in_cycle      = 1;
    st->usb.bulk_in_dci        = in_dci;
    st->usb.bulk_in_mps        = in_mps;

    st->usb.bulk_out_ring      = out_ring;
    st->usb.bulk_out_ring_phys = out_phys;
    st->usb.bulk_out_idx       = 0;
    st->usb.bulk_out_cycle     = 1;
    st->usb.bulk_out_dci       = out_dci;
    st->usb.bulk_out_mps       = out_mps;

    /* Input Control: A0 (slot ctx) + A(in_dci) + A(out_dci). */
    struct xhci_input_ctrl_ctx *ictl =
        (struct xhci_input_ctrl_ctx *)st->input_ctx;
    ictl->drop_flags = 0;
    ictl->add_flags  = (1u << 0) | (1u << in_dci) | (1u << out_dci);

    /* Slot context: copy from device context; bump ContextEntries to
     * the highest DCI we now have configured (max(in,out)). */
    struct xhci_slot_ctx *out_slot = (struct xhci_slot_ctx *)st->device_ctx;
    struct xhci_slot_ctx *in_slot  =
        (struct xhci_slot_ctx *)((uint8_t *)st->input_ctx + 32);
    *in_slot = *out_slot;
    uint8_t hi_dci = (in_dci > out_dci) ? in_dci : out_dci;
    in_slot->dw0 = (in_slot->dw0 & ~0xF8000000u) | ((uint32_t)hi_dci << 27);

    /* Build the bulk-IN endpoint context. EPType = 6 (Bulk-IN). */
    {
        struct xhci_ep_ctx *ep =
            (struct xhci_ep_ctx *)((uint8_t *)st->input_ctx + 32 + 32 * in_dci);
        memset(ep, 0, sizeof(*ep));
        ep->dw0    = 0;
        ep->dw1    = ((uint32_t)in_mps << 16) | (6u << 3) | (3u << 1);
        ep->deq_lo = (uint32_t)(in_phys & 0xFFFFFFFFu) | 1u;   /* DCS=1 */
        ep->deq_hi = (uint32_t)(in_phys >> 32);
        ep->dw4    = ((uint32_t)in_mps << 16) | in_mps;
    }
    /* Build the bulk-OUT endpoint context. EPType = 2 (Bulk-OUT). */
    {
        struct xhci_ep_ctx *ep =
            (struct xhci_ep_ctx *)((uint8_t *)st->input_ctx + 32 + 32 * out_dci);
        memset(ep, 0, sizeof(*ep));
        ep->dw0    = 0;
        ep->dw1    = ((uint32_t)out_mps << 16) | (2u << 3) | (3u << 1);
        ep->deq_lo = (uint32_t)(out_phys & 0xFFFFFFFFu) | 1u;  /* DCS=1 */
        ep->deq_hi = (uint32_t)(out_phys >> 32);
        ep->dw4    = ((uint32_t)out_mps << 16) | out_mps;
    }

    uint8_t cc = xhci_cmd((uint32_t)(st->input_ctx_phys & 0xFFFFFFFFu),
                          (uint32_t)(st->input_ctx_phys >> 32),
                          TRB_TYPE(TRB_TYPE_CONFIGURE_EP) |
                          TRB_SLOT(st->usb.slot_id),
                          0);
    if (cc != CC_SUCCESS) {
        kprintf("[xhci] Configure-EP (BULK pair, slot %u in=DCI%u out=DCI%u) cc=%u\n",
                st->usb.slot_id, in_dci, out_dci, cc);
        return false;
    }
    return true;
}

/* Public bulk-pair configurator. Looks up the dev_state for this slot
 * and forwards. Exposed (no `static`) so usb_msc.c can call it once
 * it's enumerated the interface descriptors. */
bool xhci_configure_bulk_endpoints(struct usb_device *dev,
                                   uint8_t  in_dci,  uint16_t in_mps,
                                   uint8_t  out_dci, uint16_t out_mps) {
    struct xhci_dev_state *st = get_dev_state(dev->slot_id);
    if (!st || !st->in_use) return false;
    return xhci_configure_bulk_pair(st, in_dci, in_mps, out_dci, out_mps);
}

/* Re-issue Configure Endpoint after a hub-class probe so the xHC
 * knows this slot is a USB hub. Without this the controller has no
 * way to route Setup TDs through downstream ports.
 *
 * Spec ref: xHCI 1.1 §6.2.2.1 + §4.6.6:
 *   Slot Context dw0 bit 26 = Hub
 *   Slot Context dw1 bits 31:24 = Number Of Ports
 *   Slot Context dw2 bits 17:16 = TT Think Time (1=8 FS bit times)
 * For HS hubs we also keep MTT=0 (Single TT). FS / LS hubs need TT
 * Hub Slot ID + TT Port Number set in dw2 too, which we leave 0
 * here -- M26B only validates HS hubs (qemu-xhci usb-hub is HS). */
bool xhci_configure_as_hub(struct usb_device *dev,
                           uint8_t nports, uint8_t ttt) {
    struct xhci_dev_state *st = get_dev_state(dev->slot_id);
    if (!st || !st->in_use) return false;

    struct xhci_input_ctrl_ctx *ictl =
        (struct xhci_input_ctrl_ctx *)st->input_ctx;
    ictl->drop_flags = 0;
    ictl->add_flags  = (1u << 0);     /* Slot Context only */

    struct xhci_slot_ctx *slot =
        (struct xhci_slot_ctx *)((uint8_t *)st->input_ctx + 32);

    /* Re-derive the slot dw0/dw1 we used at Address Device time and
     * patch in the Hub bit + NumberOfPorts. We *also* keep the route
     * string + speed bits intact (Address Device wrote them, but the
     * Configure Endpoint command only reads what we put in the input
     * context, so we have to re-write them here). */
    uint32_t route = dev->route_string & 0x000FFFFFu;
    /* Hub class always exposes EP0 only; ContextEntries stays at 1. */
    slot->dw0 = ((uint32_t)1u << 27) |
                ((uint32_t)dev->speed << 20) |
                ((uint32_t)1u << 26) |              /* Hub bit */
                route;
    slot->dw1 = ((uint32_t)dev->port_id << 16) |
                ((uint32_t)nports << 24);
    slot->dw2 = ((uint32_t)ttt & 0x3u) << 16;
    slot->dw3 = (uint32_t)dev->bus_address;         /* preserve current addr */

    uint8_t cc = xhci_cmd((uint32_t)(st->input_ctx_phys & 0xFFFFFFFFu),
                          (uint32_t)(st->input_ctx_phys >> 32),
                          TRB_TYPE(TRB_TYPE_CONFIGURE_EP) |
                          TRB_SLOT(dev->slot_id),
                          0);
    if (cc != CC_SUCCESS) {
        kprintf("[xhci] Configure-EP (HUB, slot %u nports=%u) cc=%u\n",
                dev->slot_id, nports, cc);
        return false;
    }
    dev->is_hub      = true;
    dev->hub_nports  = nports;
    return true;
}

/* Synchronous bulk transfer. Builds ONE Normal-TRB on the matching
 * direction's bulk ring, doorbells the EP, and busy-waits for the
 * corresponding Transfer Event. Sets *out_residue to bytes_left
 * (so actual transfer = len - *out_residue).
 *
 * `buf` must be reachable via vmm_translate -- caller guarantees this
 * for HHDM-mapped pages (pmm_alloc_page result) and for kmalloc'd
 * regions (heap is identity-HHDM-mapped via pmm_phys_to_virt).
 *
 * Returns true on CC_SUCCESS or CC_SHORT_PACKET; false on STALL,
 * timeout, or other completion code. Caller is responsible for any
 * stall recovery + retry. */
bool xhci_bulk_xfer_sync(struct usb_device *dev, uint8_t dci,
                         void *buf, uint32_t len,
                         uint32_t *out_residue) {
    if (!g_xhci.bound)            return false;
    if (dci == 0 || dci >= XHCI_MAX_DCI) return false;
    if (out_residue) *out_residue = len;

    bool dir_in;
    void    *ring;
    uint64_t ring_phys;
    uint16_t *idx;
    uint8_t  *cycle;
    if (dci == dev->bulk_in_dci) {
        dir_in    = true;
        ring      = dev->bulk_in_ring;
        ring_phys = dev->bulk_in_ring_phys;
        idx       = &dev->bulk_in_idx;
        cycle     = &dev->bulk_in_cycle;
    } else if (dci == dev->bulk_out_dci) {
        dir_in    = false;
        ring      = dev->bulk_out_ring;
        ring_phys = dev->bulk_out_ring_phys;
        idx       = &dev->bulk_out_idx;
        cycle     = &dev->bulk_out_cycle;
    } else {
        return false;
    }
    if (!ring) return false;
    (void)dir_in;

    uint64_t buf_phys = len ? vmm_translate((uint64_t)buf) : 0;

    g_bulk_complete[dev->slot_id][dci] = false;
    g_bulk_cc      [dev->slot_id][dci] = 0;
    g_bulk_residue [dev->slot_id][dci] = len;

    /* Single Normal TRB with IOC + ISP. ISP makes the controller post
     * a Transfer Event for short-packet too (so we always learn the
     * residue, even when the device finishes early). */
    uint32_t ctl = TRB_TYPE(TRB_TYPE_NORMAL) | TRB_ISP | TRB_IOC;
    (void)ring_enqueue((struct trb *)ring, ring_phys, idx, cycle,
                       (uint32_t)(buf_phys & 0xFFFFFFFFu),
                       (uint32_t)(buf_phys >> 32),
                       len, ctl);
    db_write(dev->slot_id, dci);

    /* 5-second timeout. QEMU usb-storage completes well under 1ms;
     * real flash on the slowest LS-translated path tops out around
     * a few hundred ms per 512-byte sector. 5s is generous and any
     * longer the device is just dead. */
    for (int i = 0; i < 5000; i++) {
        xhci_poll();
        if (g_bulk_complete[dev->slot_id][dci]) break;
        pit_sleep_ms(1);
    }
    if (!g_bulk_complete[dev->slot_id][dci]) {
        kprintf("[xhci] bulk timeout slot=%u DCI=%u (%u B)\n",
                dev->slot_id, dci, len);
        return false;
    }

    uint8_t  cc      = g_bulk_cc     [dev->slot_id][dci];
    uint32_t residue = g_bulk_residue[dev->slot_id][dci];
    if (out_residue) *out_residue = residue;
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
        return false;
    }
    return true;
}

/* Recover a stalled bulk endpoint. xHCI requires:
 *   1. Reset Endpoint TRB (clears the EP-halted xhc state)
 *   2. CLEAR_FEATURE(ENDPOINT_HALT) class-zero request to the device
 *      (clears the device's stall + resets its data toggle to 0)
 *   3. Set TR Dequeue Pointer to a fresh ring start so subsequent
 *      enqueues line up with the controller's view.
 *
 * QEMU's usb-storage rarely STALLs in healthy operation, but real
 * flash drives stall on every CHECK CONDITION SCSI status (e.g.
 * "Logical Unit Not Ready" right after enumeration), so this matters. */
bool xhci_recover_stall(struct usb_device *dev, uint8_t dci) {
    struct xhci_dev_state *st = get_dev_state(dev->slot_id);
    if (!st || !st->in_use) return false;
    if (dci == 0 || dci >= XHCI_MAX_DCI) return false;

    uint8_t ep_addr = 0;
    void    **ring_pp     = 0;
    uint64_t *ring_phys_p = 0;
    uint16_t *idx         = 0;
    uint8_t  *cycle       = 0;
    if (dci == dev->bulk_in_dci) {
        ep_addr     = dev->bulk_in_addr;
        ring_pp     = &dev->bulk_in_ring;
        ring_phys_p = &dev->bulk_in_ring_phys;
        idx         = &dev->bulk_in_idx;
        cycle       = &dev->bulk_in_cycle;
    } else if (dci == dev->bulk_out_dci) {
        ep_addr     = dev->bulk_out_addr;
        ring_pp     = &dev->bulk_out_ring;
        ring_phys_p = &dev->bulk_out_ring_phys;
        idx         = &dev->bulk_out_idx;
        cycle       = &dev->bulk_out_cycle;
    } else {
        return false;
    }

    /* (1) Reset Endpoint command (TRB Type 14, slot id + EP id). */
    uint8_t cc = xhci_cmd(0, 0,
                          TRB_TYPE(TRB_TYPE_RESET_EP) |
                          TRB_SLOT(dev->slot_id) |
                          ((uint32_t)dci << 16),
                          0);
    if (cc != CC_SUCCESS) {
        kprintf("[xhci] Reset-EP slot=%u DCI=%u cc=%u\n",
                dev->slot_id, dci, cc);
        return false;
    }

    /* (2) CLEAR_FEATURE(ENDPOINT_HALT). wIndex = endpoint address. */
    if (!xhci_control_class(dev,
                            USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT,
                            USB_REQ_CLEAR_FEATURE,
                            0,                /* wValue = ENDPOINT_HALT (0) */
                            ep_addr,
                            0, 0)) {
        kprintf("[xhci] CLEAR_FEATURE(HALT) failed slot=%u ep=0x%02x\n",
                dev->slot_id, ep_addr);
        return false;
    }

    /* (3) Allocate a fresh transfer ring + Set-TR-Dequeue. */
    uint64_t new_phys;
    struct trb *new_ring = make_ring(&new_phys);
    if (!new_ring) return false;
    *ring_pp     = new_ring;
    *ring_phys_p = new_phys;
    *idx         = 0;
    *cycle       = 1;

    cc = xhci_cmd((uint32_t)(new_phys & 0xFFFFFFFFu) | 1u,    /* DCS=1 */
                  (uint32_t)(new_phys >> 32),
                  TRB_TYPE(TRB_TYPE_SET_TR_DEQ) |
                  TRB_SLOT(dev->slot_id) |
                  ((uint32_t)dci << 16),
                  0);
    if (cc != CC_SUCCESS) {
        kprintf("[xhci] Set-TR-Dequeue slot=%u DCI=%u cc=%u\n",
                dev->slot_id, dci, cc);
        return false;
    }
    return true;
}

/* Submit one interrupt-IN transfer using the device's pre-allocated
 * report buffer. Re-armed after every completion in xhci_poll(). */
static void xhci_submit_int_in(struct usb_device *dev) {
    if (!dev->int_ring) return;
    uint32_t ctl = TRB_TYPE(TRB_TYPE_NORMAL) | TRB_ISP | TRB_IOC;
    (void)ring_enqueue((struct trb *)dev->int_ring, dev->int_ring_phys,
                       &dev->int_idx, &dev->int_cycle,
                       (uint32_t)(dev->int_buf_phys & 0xFFFFFFFFu),
                       (uint32_t)(dev->int_buf_phys >> 32),
                       dev->int_buf_size,
                       ctl);
    db_write(dev->slot_id, dev->int_dci);
    dev->int_armed = true;
}

/* ============================================================== */
/* Per-device probe + descriptor walk                              */
/* ============================================================== */

static bool fetch_dev_desc(struct usb_device *dev,
                           struct usb_device_desc *out, uint16_t want) {
    /* GET_DESCRIPTOR(DEVICE, 0, 0). wValue = (DescType << 8) | index. */
    return xhci_control_class(dev,
                              USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                              USB_REQ_GET_DESCRIPTOR,
                              (uint16_t)(USB_DESC_DEVICE << 8),
                              0,
                              out, want);
}

static bool fetch_config_desc(struct usb_device *dev,
                              uint8_t *buf, uint16_t want) {
    return xhci_control_class(dev,
                              USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                              USB_REQ_GET_DESCRIPTOR,
                              (uint16_t)(USB_DESC_CONFIG << 8),
                              0,
                              buf, want);
}

static bool set_configuration(struct usb_device *dev, uint8_t cfg) {
    return xhci_control_class(dev,
                              USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE,
                              USB_REQ_SET_CONFIGURATION,
                              (uint16_t)cfg, 0, 0, 0);
}

/* Forward decl: defined further down (M26B). enumerate_port + the
 * downstream-hub path xhci_attach_via_hub both call it after Address
 * Device succeeds, so the body lives between them. */
static bool xhci_finalize_device(struct xhci_dev_state *st);

static void enumerate_port(uint8_t port_idx) {
    uint32_t portsc = portsc_read(port_idx);
    if (!(portsc & PORTSC_CCS)) return;          /* nothing connected */

    /* USB-2 ports come up auto-enabled iff the device is an HS USB-2.
     * USB-3 (SuperSpeed) ports auto-train and arrive PED=1 too. We
     * only need an explicit reset for the FS/LS-after-fallback case
     * on USB-2. Either way, kicking PR=1 when PED is already 1 is a
     * no-op on QEMU; on real hardware it costs us a re-train cycle
     * but is harmless. */
    if (!(portsc & PORTSC_PED)) {
        portsc_write(port_idx, PORTSC_PR);
        for (int i = 0; i < 100; i++) {
            portsc = portsc_read(port_idx);
            if (portsc & PORTSC_PRC) break;
            pit_sleep_ms(10);
        }
        /* Clear PRC. */
        portsc_write(port_idx, PORTSC_PRC);
        portsc = portsc_read(port_idx);
    }
    if (!(portsc & PORTSC_PED)) {
        kprintf("[xhci] port %u did not enable after reset (PORTSC=0x%08x)\n",
                port_idx + 1u, portsc);
        return;
    }

    uint8_t speed = (uint8_t)PORTSC_SPEED(portsc);
    kprintf("[xhci] port %u: connected, speed=%u (PORTSC=0x%08x)\n",
            port_idx + 1u, speed, portsc);

    /* Enable Slot. The HC returns the slot id in the completion event. */
    uint8_t slot_id = 0;
    uint8_t cc = xhci_cmd(0, 0, TRB_TYPE(TRB_TYPE_ENABLE_SLOT), &slot_id);
    if (cc != CC_SUCCESS || slot_id == 0 || slot_id > XHCI_MAX_SLOTS) {
        kprintf("[xhci] Enable Slot failed (port %u cc=%u slot=%u)\n",
                port_idx + 1u, cc, slot_id);
        return;
    }

    struct xhci_dev_state *st = get_dev_state(slot_id);
    if (!st) {
        kprintf("[xhci] slot id %u out of range\n", slot_id);
        return;
    }
    if (st->in_use) {
        kprintf("[xhci] slot %u already in use!\n", slot_id);
        return;
    }
    memset(st, 0, sizeof(*st));
    st->in_use      = true;
    st->usb.slot_id = slot_id;
    st->usb.port_id = port_idx + 1u;     /* 1-based */
    st->usb.speed   = speed;
    st->usb.mps0    = default_mps0(speed);

    /* Allocate the EP0 ring + input context + device context. */
    st->ep0_ring = make_ring(&st->usb.ep0_ring_phys);
    if (!st->ep0_ring) goto fail;
    st->usb.ep0_ring  = st->ep0_ring;
    st->usb.ep0_idx   = 0;
    st->usb.ep0_cycle = 1;

    st->input_ctx  = alloc_zero_page(&st->input_ctx_phys);
    st->device_ctx = alloc_zero_page(&st->device_ctx_phys);
    if (!st->input_ctx || !st->device_ctx) goto fail;

    /* Publish the device context address into DCBAA[slot_id]. */
    g_xhci.dcbaa[slot_id] = st->device_ctx_phys;

    if (!xhci_address_device(st, st->usb.port_id, speed)) goto fail;
    if (!xhci_finalize_device(st)) goto fail;
    return;

fail:
    kprintf("[xhci] device init failed on port %u (slot %u released)\n",
            port_idx + 1u, slot_id);
    /* Best-effort slot teardown: zero DCBAA + clear in_use. We don't
     * issue Disable Slot because if Address Device failed the slot
     * is already in an indeterminate state on the controller side. */
    g_xhci.dcbaa[slot_id] = 0;
    st->in_use = false;
}

/* xhci_finalize_device: shared between root-hub bringup
 * (enumerate_port) and downstream-hub bringup (xhci_attach_via_hub).
 *
 * Assumes xhci_address_device() has already succeeded for `st`. Walks
 * the device descriptor + 1 configuration descriptor, then dispatches
 * to a class driver (HID, MSC, or USB hub).
 *
 * Returns true on success, false on any descriptor / SET_CONFIG /
 * class-probe failure. The caller is responsible for slot teardown
 * on a false return. */
static bool xhci_finalize_device(struct xhci_dev_state *st) {
    uint8_t slot_id = st->usb.slot_id;

    /* Probe descriptor in two passes: first 8 bytes to learn the real
     * EP0 max packet size, then re-issue Address Device only if the
     * value differs (we just patch the input context EP0 mps and
     * re-Address with BSR=1 -- the "Block Set Address" variant just
     * updates EP0 without re-assigning the bus address). For QEMU's
     * usb-kbd / usb-mouse the initial guess is always right, so we
     * usually skip the patch. */
    struct usb_device_desc *dev_desc =
        (struct usb_device_desc *)g_xhci.desc_buf;
    memset(dev_desc, 0, sizeof(*dev_desc));
    if (!fetch_dev_desc(&st->usb, dev_desc, 8)) {
        kprintf("[xhci] GET_DESCRIPTOR(DEV, 8B) failed on slot %u\n", slot_id);
        return false;
    }
    if (dev_desc->bMaxPacketSize0 != 0 && dev_desc->bMaxPacketSize0 != st->usb.mps0) {
        st->usb.mps0 = dev_desc->bMaxPacketSize0;
        struct xhci_ep_ctx *ep0 =
            (struct xhci_ep_ctx *)((uint8_t *)st->input_ctx + 64);
        ep0->dw1 = (ep0->dw1 & ~0xFFFF0000u) |
                   ((uint32_t)st->usb.mps0 << 16);
        struct xhci_input_ctrl_ctx *ictl =
            (struct xhci_input_ctrl_ctx *)st->input_ctx;
        ictl->drop_flags = 0;
        ictl->add_flags  = (1u << 1);    /* just re-evaluate EP0 */
        uint8_t ec = xhci_cmd(
            (uint32_t)(st->input_ctx_phys & 0xFFFFFFFFu),
            (uint32_t)(st->input_ctx_phys >> 32),
            TRB_TYPE(TRB_TYPE_EVALUATE_CTX) | TRB_SLOT(slot_id),
            0);
        if (ec != CC_SUCCESS) {
            kprintf("[xhci] EvaluateContext (mps fix) failed cc=%u\n", ec);
            return false;
        }
    }
    if (!fetch_dev_desc(&st->usb, dev_desc, 18)) {
        kprintf("[xhci] GET_DESCRIPTOR(DEV, 18B) failed on slot %u\n", slot_id);
        return false;
    }
    kprintf("[xhci] dev slot=%u addr=%u  vid=%04x pid=%04x  class=%02x\n",
            slot_id, st->usb.bus_address,
            dev_desc->idVendor, dev_desc->idProduct, dev_desc->bDeviceClass);
    /* Snapshot the bits we need for the M35C usbreg attach event
     * before we reuse the scratch page for the configuration
     * descriptor. dev_subclass/proto are only meaningful when
     * dev_class != 0; for composite devices (class==0) we'll fold in
     * the active interface descriptor's class triple later. */
    uint8_t dev_class    = dev_desc->bDeviceClass;
    uint8_t dev_subclass = dev_desc->bDeviceSubClass;
    uint8_t dev_protocol = dev_desc->bDeviceProtocol;
    uint16_t dev_vid     = dev_desc->idVendor;
    uint16_t dev_pid     = dev_desc->idProduct;

    /* Read 9-byte config header to learn wTotalLength, then the full
     * config descriptor (which contains all interface + endpoint +
     * HID descriptors back-to-back). One page covers any practical
     * config bundle. */
    uint8_t *cfg_buf = g_xhci.desc_buf;
    memset(cfg_buf, 0, PAGE_SIZE);
    if (!fetch_config_desc(&st->usb, cfg_buf, 9)) {
        kprintf("[xhci] GET_DESCRIPTOR(CFG, 9B) failed on slot %u\n", slot_id);
        return false;
    }
    struct usb_config_desc *cd = (struct usb_config_desc *)cfg_buf;
    uint16_t total_len = cd->wTotalLength;
    if (total_len > PAGE_SIZE) total_len = PAGE_SIZE;
    if (total_len > 9) {
        if (!fetch_config_desc(&st->usb, cfg_buf, total_len)) {
            kprintf("[xhci] GET_DESCRIPTOR(CFG, %uB) failed on slot %u\n",
                    total_len, slot_id);
            return false;
        }
    }

    /* Set the configuration so the controller leaves Addressed and
     * enters Configured -- required before SET_PROTOCOL/SET_IDLE
     * class requests are accepted. */
    if (!set_configuration(&st->usb, cd->bConfigurationValue)) {
        kprintf("[xhci] SET_CONFIGURATION(%u) failed on slot %u\n",
                cd->bConfigurationValue, slot_id);
        return false;
    }

    /* M35E gate: in safe-basic we drop *all* USB class probes (HID
     * included). In safe-gui / compatibility we keep HID for input but
     * drop the rest -- hubs, MSC, audio, printers, etc. The xHCI
     * controller itself stays bound either way; the device is recorded
     * via usbreg with a NULL driver so hwcompat can flag it as
     * unsupported / skipped-by-policy. */
    bool skip_usb_full  = safemode_skip_usb_full();
    bool skip_usb_extra = safemode_skip_usb_extra();
    if (skip_usb_full) {
        kprintf("[xhci] safe-mode (%s): not probing class drivers for slot %u "
                "(vid=%04x pid=%04x class=%02x)\n",
                safemode_tag(), slot_id, dev_vid, dev_pid, dev_class);
    }

    /* USB Hub class (M26B): handled before the iface walk so we can
     * skip the HID/MSC scan entirely and let usb_hub_probe own the
     * downstream port enumeration + recursive class probes. */
    if (dev_class == USB_CLASS_HUB) {
        if (skip_usb_full || skip_usb_extra) {
            kprintf("[xhci] safe-mode (%s): skipping usb_hub for slot %u\n",
                    safemode_tag(), slot_id);
            usbreg_record_attach(slot_id, st->usb.port_id, st->usb.hub_depth,
                                 st->usb.speed, dev_vid, dev_pid,
                                 USB_CLASS_HUB, dev_subclass, dev_protocol,
                                 NULL);
            return true;
        }
        if (usb_hub_probe(&st->usb)) {
            usbreg_record_attach(slot_id, st->usb.port_id, st->usb.hub_depth,
                                 st->usb.speed, dev_vid, dev_pid,
                                 USB_CLASS_HUB, dev_subclass, dev_protocol,
                                 "usb_hub");
            return true;
        }
        kprintf("[xhci] usb_hub_probe failed on slot %u\n", slot_id);
        usbreg_record_attach(slot_id, st->usb.port_id, st->usb.hub_depth,
                             st->usb.speed, dev_vid, dev_pid,
                             USB_CLASS_HUB, dev_subclass, dev_protocol,
                             NULL);
        usbreg_record_probe_failed(slot_id, "usb_hub");
        return false;
    }

    /* Walk the interface + endpoint descriptors looking for one of the
     * class drivers we support: HID (Boot keyboard / mouse) or
     * Mass-Storage Class (BBB transport, SCSI subclass). MSC needs to
     * see BOTH bulk endpoints together, so we handle it at the
     * INTERFACE descriptor level by pre-scanning ahead to collect the
     * pair before calling usb_msc_probe. HID stays endpoint-by-endpoint
     * because there is at most one interrupt-IN per Boot device. */
    uint16_t off = cd->bLength;            /* skip past the config header */
    const struct usb_iface_desc *active_iface = 0;
    /* M35C: track the FIRST interface we see so the usbreg fallback at
     * the tail can record a sensible class triple even for composite
     * devices (bDeviceClass == 0 in the device descriptor). */
    uint8_t  first_iface_class    = 0;
    uint8_t  first_iface_subclass = 0;
    uint8_t  first_iface_protocol = 0;
    bool     saw_iface = false;
    /* M35C: name of the class driver that successfully claimed the
     * device, or NULL if the descriptor walk found no in-tree driver
     * to bind. Routed to usbreg at the tail. */
    const char *claimed_driver = NULL;
    bool claimed = false;
    while (off + 2u <= total_len && !claimed) {
        uint8_t desc_len  = cfg_buf[off];
        uint8_t desc_type = cfg_buf[off + 1];
        if (desc_len == 0) break;          /* malformed */
        if (off + desc_len > total_len) break;

        if (desc_type == USB_DESC_INTERFACE) {
            active_iface = (const struct usb_iface_desc *)(cfg_buf + off);
            if (!saw_iface) {
                first_iface_class    = active_iface->bInterfaceClass;
                first_iface_subclass = active_iface->bInterfaceSubClass;
                first_iface_protocol = active_iface->bInterfaceProtocol;
                saw_iface = true;
            }

            /* Some hubs report class via the interface descriptor
             * (bInterfaceClass = 0x09) instead of the device descriptor.
             * Catch that case here and hand off to usb_hub_probe.
             * M35E: same skip policy as the device-class hub branch. */
            if (active_iface->bInterfaceClass == USB_CLASS_HUB &&
                (skip_usb_full || skip_usb_extra)) {
                kprintf("[xhci] safe-mode (%s): skipping iface-class hub "
                        "on slot %u\n", safemode_tag(), slot_id);
                usbreg_record_attach(slot_id, st->usb.port_id,
                                     st->usb.hub_depth, st->usb.speed,
                                     dev_vid, dev_pid,
                                     USB_CLASS_HUB,
                                     active_iface->bInterfaceSubClass,
                                     active_iface->bInterfaceProtocol,
                                     NULL);
                return true;
            }
            if (active_iface->bInterfaceClass == USB_CLASS_HUB) {
                if (usb_hub_probe(&st->usb)) {
                    usbreg_record_attach(slot_id, st->usb.port_id,
                                         st->usb.hub_depth, st->usb.speed,
                                         dev_vid, dev_pid,
                                         USB_CLASS_HUB,
                                         active_iface->bInterfaceSubClass,
                                         active_iface->bInterfaceProtocol,
                                         "usb_hub");
                    return true;
                }
                kprintf("[xhci] iface-class HUB probe failed slot %u\n",
                        slot_id);
                usbreg_record_attach(slot_id, st->usb.port_id,
                                     st->usb.hub_depth, st->usb.speed,
                                     dev_vid, dev_pid,
                                     USB_CLASS_HUB,
                                     active_iface->bInterfaceSubClass,
                                     active_iface->bInterfaceProtocol,
                                     NULL);
                usbreg_record_probe_failed(slot_id, "usb_hub");
                return false;
            }

            /* M35E: skip USB MSC entirely in any safe mode. The probe
             * spins reading sectors which can hang on quirky devices. */
            if (active_iface->bInterfaceClass    == USB_CLASS_MASS_STORAGE &&
                (skip_usb_full || skip_usb_extra)) {
                kprintf("[xhci] safe-mode (%s): skipping usb_msc on slot %u\n",
                        safemode_tag(), slot_id);
                usbreg_record_attach(slot_id, st->usb.port_id,
                                     st->usb.hub_depth, st->usb.speed,
                                     dev_vid, dev_pid,
                                     USB_CLASS_MASS_STORAGE,
                                     active_iface->bInterfaceSubClass,
                                     active_iface->bInterfaceProtocol,
                                     NULL);
                /* Treat as "claimed by safe-mode policy" so the iface
                 * walk doesn't keep poking the same descriptor below. */
                claimed = true;
                break;
            }
            /* Mass-Storage / BBB / SCSI: pre-scan endpoints inside this
             * iface looking for one bulk-IN and one bulk-OUT. */
            if (active_iface->bInterfaceClass    == USB_CLASS_MASS_STORAGE &&
                active_iface->bInterfaceSubClass == USB_MSC_SUBCLASS_SCSI  &&
                active_iface->bInterfaceProtocol == USB_MSC_PROTO_BBB) {
                const struct usb_endpoint_desc *ep_in  = 0;
                const struct usb_endpoint_desc *ep_out = 0;
                uint16_t scan = off + active_iface->bLength;
                uint8_t  remaining = active_iface->bNumEndpoints;
                while (scan + 2u <= total_len && remaining > 0) {
                    uint8_t l = cfg_buf[scan];
                    uint8_t t = cfg_buf[scan + 1];
                    if (l == 0 || scan + l > total_len) break;
                    if (t == USB_DESC_INTERFACE) break;     /* into next iface */
                    if (t == USB_DESC_ENDPOINT) {
                        const struct usb_endpoint_desc *ep =
                            (const struct usb_endpoint_desc *)(cfg_buf + scan);
                        if (USB_EP_TYPE(ep->bmAttributes) == USB_EP_BULK) {
                            if (USB_EP_DIR_IN(ep->bEndpointAddress) && !ep_in)
                                ep_in = ep;
                            else if (!USB_EP_DIR_IN(ep->bEndpointAddress) && !ep_out)
                                ep_out = ep;
                        }
                        if (remaining) remaining--;
                    }
                    scan += l;
                }
                if (ep_in && ep_out) {
                    if (usb_msc_probe(&st->usb, active_iface, ep_in, ep_out)) {
                        claimed_driver = "usb_msc";
                        claimed = true;
                        break;
                    } else {
                        /* Probe path was exercised but failed -- record
                         * so it lands in usbreg as PROBE_FAILED rather
                         * than UNSUPPORTED. */
                        usbreg_record_attach(slot_id, st->usb.port_id,
                                             st->usb.hub_depth,
                                             st->usb.speed,
                                             dev_vid, dev_pid,
                                             USB_CLASS_MASS_STORAGE,
                                             USB_MSC_SUBCLASS_SCSI,
                                             USB_MSC_PROTO_BBB,
                                             NULL);
                        usbreg_record_probe_failed(slot_id, "usb_msc");
                        return true;
                    }
                }
            }
        } else if (desc_type == USB_DESC_ENDPOINT && active_iface) {
            const struct usb_endpoint_desc *ep =
                (const struct usb_endpoint_desc *)(cfg_buf + off);
            if (active_iface->bInterfaceClass == USB_CLASS_HID &&
                skip_usb_full) {
                /* M35E: BASIC drops HID too; the operator has the
                 * kernel console keyboard via the PS/2 path. */
                kprintf("[xhci] safe-mode (%s): skipping usb_hid on slot %u\n",
                        safemode_tag(), slot_id);
                claimed = true;
                break;
            }
            if (active_iface->bInterfaceClass == USB_CLASS_HID &&
                USB_EP_TYPE(ep->bmAttributes) == USB_EP_INTERRUPT &&
                USB_EP_DIR_IN(ep->bEndpointAddress)) {
                /* Hand off to the HID class driver. */
                if (usb_hid_probe(&st->usb, active_iface, ep)) {
                    /* Configure the endpoint context + transfer ring. */
                    uint8_t dci = (uint8_t)(2u * USB_EP_NUM(ep->bEndpointAddress) + 1u);
                    st->usb.int_dci = dci;
                    if (xhci_configure_int_in(st, dci,
                                              ep->wMaxPacketSize & 0x07FFu,
                                              ep->bInterval)) {
                        xhci_submit_int_in(&st->usb);
                    }
                    claimed_driver = "usb_hid";
                    /* Stop after first claimed HID endpoint per device --
                     * a single Boot HID device has at most one. */
                    claimed = true;
                    break;
                }
            }
        }
        off += desc_len;
    }

    /* M35C: record the device unconditionally. The classify triple we
     * report is the one most useful for naming the device:
     *   - if bDeviceClass != 0, use the device descriptor;
     *   - else use the first interface descriptor we saw;
     *   - else (no interface descriptors at all -- malformed) use 0/0/0.
     * `claimed_driver` is non-NULL if HID/MSC successfully bound. NULL
     * here means no in-tree driver claimed the device, which usbreg
     * will route to UNSUPPORTED or UNKNOWN automatically. */
    uint8_t rec_class    = dev_class;
    uint8_t rec_subclass = dev_subclass;
    uint8_t rec_protocol = dev_protocol;
    if (rec_class == 0 && saw_iface) {
        rec_class    = first_iface_class;
        rec_subclass = first_iface_subclass;
        rec_protocol = first_iface_protocol;
    }
    usbreg_record_attach(slot_id, st->usb.port_id, st->usb.hub_depth,
                         st->usb.speed, dev_vid, dev_pid,
                         rec_class, rec_subclass, rec_protocol,
                         claimed_driver);

    /* claimed==false is OK: descriptor walk just couldn't find a
     * driver we know about (e.g. a printer). Return success either
     * way -- the slot stays in_use so it shows up in devlist. */
    return true;
}

/* ============================================================== */
/* Hub-attach entrypoint (M26B)                                    */
/* ============================================================== */

/* Public entry point used by usb_hub.c after a successful
 * SetPortFeature(PORT_RESET) on a downstream hub port.
 *
 * Returns the new struct usb_device * on success, or NULL on any
 * step failure (with the slot fully released). The caller (the hub
 * class driver) does NOT need to manage slot lifecycles -- this
 * function takes care of Enable Slot, allocating the input/device
 * contexts, Address Device, and the descriptor walk. */
struct usb_device *xhci_attach_via_hub(struct usb_device *parent,
                                       uint8_t hub_port,
                                       uint8_t speed) {
    if (!parent || !g_xhci.bound) return 0;
    if (!parent->is_hub)          return 0;
    if (parent->hub_depth + 1 > USB_HUB_MAX_DEPTH) {
        kprintf("[xhci] hub depth %u + 1 > MAX %u (refusing nested probe)\n",
                parent->hub_depth, USB_HUB_MAX_DEPTH);
        return 0;
    }

    /* Compose the route string for the child by appending hub_port at
     * the next available 4-bit slot. parent->route_string is 0 for
     * a hub plugged into a root-hub port (depth 1 child therefore
     * gets route = hub_port in bits 3:0); a hub one deeper extends
     * into bits 7:4, etc. */
    uint8_t  child_depth = (uint8_t)(parent->hub_depth + 1);
    uint32_t shift       = (uint32_t)(parent->hub_depth) * 4u;
    uint32_t child_route = parent->route_string |
                           (((uint32_t)hub_port & 0xFu) << shift);

    /* Enable Slot. */
    uint8_t slot_id = 0;
    uint8_t cc = xhci_cmd(0, 0, TRB_TYPE(TRB_TYPE_ENABLE_SLOT), &slot_id);
    if (cc != CC_SUCCESS || slot_id == 0 || slot_id > XHCI_MAX_SLOTS) {
        kprintf("[xhci] hub slot %u port %u: Enable Slot failed cc=%u\n",
                parent->slot_id, hub_port, cc);
        return 0;
    }
    struct xhci_dev_state *st = get_dev_state(slot_id);
    if (!st || st->in_use) {
        kprintf("[xhci] hub-attach: slot %u not free\n", slot_id);
        return 0;
    }
    memset(st, 0, sizeof(*st));
    st->in_use              = true;
    st->usb.slot_id         = slot_id;
    st->usb.port_id         = parent->port_id;     /* root port the topology lives behind */
    st->usb.speed           = speed;
    st->usb.mps0            = default_mps0(speed);
    st->usb.parent_slot_id  = parent->slot_id;
    st->usb.route_string    = child_route;
    st->usb.hub_depth       = child_depth;
    st->usb.hub_port        = hub_port;

    st->ep0_ring = make_ring(&st->usb.ep0_ring_phys);
    if (!st->ep0_ring) goto fail;
    st->usb.ep0_ring  = st->ep0_ring;
    st->usb.ep0_idx   = 0;
    st->usb.ep0_cycle = 1;

    st->input_ctx  = alloc_zero_page(&st->input_ctx_phys);
    st->device_ctx = alloc_zero_page(&st->device_ctx_phys);
    if (!st->input_ctx || !st->device_ctx) goto fail;
    g_xhci.dcbaa[slot_id] = st->device_ctx_phys;

    if (!xhci_address_device(st, st->usb.port_id, speed)) goto fail;
    if (!xhci_finalize_device(st)) goto fail;

    kprintf("[xhci] hub-attach: parent slot %u port %u -> child slot %u "
            "(route=0x%05x depth=%u)\n",
            parent->slot_id, hub_port, slot_id,
            (unsigned)child_route, (unsigned)child_depth);
    return &st->usb;

fail:
    kprintf("[xhci] hub-attach failed (parent slot %u port %u, child slot %u)\n",
            parent->slot_id, hub_port, slot_id);
    g_xhci.dcbaa[slot_id] = 0;
    st->in_use = false;
    return 0;
}

/* ============================================================== */
/* M26C: hot-plug helpers                                          */
/* ============================================================== */
/*
 * The hot-plug story is split deliberately: the IRQ-driven event
 * ring (xhci_poll) only acks PORTSC change bits + flips the
 * `port_change_pending` bitmap; xhci_service_port_changes(), called
 * from the kernel idle loop, walks pending root ports and issues
 * Enable/Disable Slot in normal context. Class drivers expose tiny
 * unbind hooks (usb_hid_unbind, usb_msc_unbind) that the detach
 * path invokes before tearing down the slot.
 *
 * The lookups below are O(MaxSlots) (8 today). They're called from
 * the deferred service path AND from usb_hub_poll(), so we keep them
 * lock-free w.r.t. the slot table -- the only mutators are the same
 * single-threaded service path.
 */

#include <tobyos/usb_hid.h>
#include <tobyos/usb_msc.h>
#include <tobyos/hotplug.h>

uint8_t xhci_slot_for_root_port(uint8_t port_id) {
    if (!g_xhci.bound) return 0;
    for (int s = 1; s <= XHCI_MAX_SLOTS; s++) {
        const struct xhci_dev_state *st = &g_xhci.devs[s];
        if (!st->in_use) continue;
        if (st->usb.hub_depth != 0) continue;     /* skip behind-hub kids */
        if (st->usb.port_id   == port_id) return (uint8_t)s;
    }
    return 0;
}

uint8_t xhci_slot_for_hub_port(uint8_t parent_slot_id, uint8_t hub_port) {
    if (!g_xhci.bound || parent_slot_id == 0) return 0;
    for (int s = 1; s <= XHCI_MAX_SLOTS; s++) {
        const struct xhci_dev_state *st = &g_xhci.devs[s];
        if (!st->in_use) continue;
        if (st->usb.parent_slot_id == parent_slot_id &&
            st->usb.hub_port       == hub_port) return (uint8_t)s;
    }
    return 0;
}

uint8_t xhci_iter_root_port_with_device(int *ix) {
    if (!ix || !g_xhci.bound) return 0;
    for (int s = (*ix < 1 ? 1 : *ix + 1); s <= XHCI_MAX_SLOTS; s++) {
        const struct xhci_dev_state *st = &g_xhci.devs[s];
        if (!st->in_use) continue;
        if (st->usb.hub_depth != 0) continue;
        *ix = s;
        return st->usb.port_id;
    }
    *ix = XHCI_MAX_SLOTS + 1;
    return 0;
}

/* Build an info string describing the device that just left so the
 * userland test harness can correlate the detach with the previous
 * attach. Format mirrors xhci_introspect_at()->name. */
static void xhci_format_dev_info(const struct usb_device *u,
                                 char *out, size_t cap) {
    const char *cls = "?";
    if (u->is_hub)                                           cls = "hub";
    else if (u->msc_state || u->bulk_in_dci)                 cls = "msc";
    else if (u->int_complete || u->hid_state)                cls = "hid";
    if (u->hub_depth == 0) {
        ksnprintf(out, cap, "usb1-%u %s slot=%u",
                  (unsigned)u->port_id, cls, (unsigned)u->slot_id);
    } else {
        ksnprintf(out, cap, "usb1-%u.%u %s slot=%u",
                  (unsigned)u->port_id, (unsigned)u->hub_port,
                  cls, (unsigned)u->slot_id);
    }
}

bool xhci_detach_slot(uint8_t slot_id) {
    if (!g_xhci.bound) return false;
    struct xhci_dev_state *st = get_dev_state(slot_id);
    if (!st || !st->in_use) return false;

    char info[ABI_HOT_INFO_MAX];
    xhci_format_dev_info(&st->usb, info, sizeof(info));
    uint8_t hub_depth = st->usb.hub_depth;
    uint8_t hub_port  = hub_depth ? st->usb.hub_port : st->usb.port_id;
    bool    was_hub   = st->usb.is_hub;

    kprintf("[xhci] detach slot %u (%s)\n", slot_id, info);

    /* 1. If this slot is a hub, recursively tear down everything that
     *    hangs off it. Without this, ripping the parent hub leaves
     *    orphan slots whose parent_slot_id points at a freed entry,
     *    breaking xhci_slot_for_hub_port() lookups and (more
     *    importantly) leaving the xHC believing it can still route
     *    setup tokens through the dead hub. We walk in two passes:
     *    snapshot the matching slot ids, then detach them, so the
     *    recursion doesn't trip over its own table mutations. */
    if (was_hub) {
        uint8_t kids[XHCI_MAX_SLOTS];
        int kn = 0;
        for (int s = 1; s <= XHCI_MAX_SLOTS; s++) {
            if (s == slot_id) continue;
            if (!g_xhci.devs[s].in_use) continue;
            if (g_xhci.devs[s].usb.parent_slot_id == slot_id) {
                kids[kn++] = (uint8_t)s;
            }
        }
        for (int k = 0; k < kn; k++) {
            (void)xhci_detach_slot(kids[k]);
        }
    }

    /* 2. Drop class-driver references *before* we free the slot. The
     *    unbinds null their back-pointers so any in-flight transfer
     *    completion that arrives mid-teardown sees ->hid_state==NULL
     *    or ->msc_state==NULL and bails out cleanly. */
    if (st->usb.hid_state || st->usb.int_complete) {
        usb_hid_unbind(&st->usb);
    }
    if (st->usb.msc_state || st->usb.bulk_in_dci) {
        usb_msc_unbind(&st->usb);
    }

    /* 3. Issue Disable Slot. The xHC frees its own context
     *    bookkeeping; we still hold the input/device context pages
     *    on our side (we leak them today -- pmm_free_page() is not
     *    yet exposed; M26C accepts a small per-cycle leak in
     *    exchange for not introducing a brand-new free path). */
    uint8_t cc = xhci_cmd(0, 0,
                          TRB_TYPE(TRB_TYPE_DISABLE_SLOT) | TRB_SLOT(slot_id),
                          0);
    if (cc != CC_SUCCESS && cc != CC_SLOT_NOT_ENABLED) {
        /* SLOT_NOT_ENABLED on a slot we thought was live just means
         * the controller already retired it (likely because the cable
         * was already gone before we got here). */
        kprintf("[xhci] Disable Slot %u failed cc=%u (continuing teardown)\n",
                slot_id, cc);
    }

    /* 4. Forget the slot from our side. The DCBAA entry has to be
     *    cleared *after* Disable Slot (the controller may still read
     *    it during the command completion). */
    g_xhci.dcbaa[slot_id] = 0;
    memset(st, 0, sizeof(*st));

    /* 5. Notify userland. */
    hotplug_post_detach(ABI_DEVT_BUS_USB, slot_id, hub_depth, hub_port, info);
    return true;
}

uint8_t xhci_attach_root_port(uint8_t port_id) {
    if (!g_xhci.bound) return 0;
    if (port_id == 0 || port_id > g_xhci.max_ports) return 0;
    /* If a slot already exists for this port we're being asked to
     * re-attach a port that's still occupied -- almost always a
     * stale CSC from before a previous successful enumeration. Just
     * report the existing slot and let the caller decide. */
    uint8_t existing = xhci_slot_for_root_port(port_id);
    if (existing) return existing;

    /* enumerate_port() already does the full Enable Slot + Address
     * Device + descriptor walk + class probe path. Wrap it so the
     * caller doesn't have to know about the 0-based port index. */
    uint32_t portsc = portsc_read((uint8_t)(port_id - 1u));
    if (!(portsc & PORTSC_CCS)) return 0;     /* nothing actually plugged */

    enumerate_port((uint8_t)(port_id - 1u));
    uint8_t s = xhci_slot_for_root_port(port_id);
    if (s) {
        const struct usb_device *u = &g_xhci.devs[s].usb;
        char info[ABI_HOT_INFO_MAX];
        xhci_format_dev_info(u, info, sizeof(info));
        hotplug_post_attach(ABI_DEVT_BUS_USB, s, 0, port_id, info);
    }
    return s;
}

/* Service deferred port-change work. Called from the kernel idle
 * loop; cheap when port_change_pending == 0. Walks every bit set in
 * the bitmap, atomically clearing it before processing so a fresh
 * change arriving mid-service triggers a follow-up pass. */
void xhci_service_port_changes(void) {
    if (!g_xhci.bound) return;
    uint32_t pending;
    /* Snapshot + clear under cli to avoid losing a bit set by a
     * concurrent xhci_irq_handler. */
    uint64_t flags;
    __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    pending = g_xhci.port_change_pending;
    g_xhci.port_change_pending = 0;
    __asm__ __volatile__("push %0; popfq" :: "r"(flags) : "memory");

    if (!pending) return;

    for (uint8_t p = 1; p <= g_xhci.max_ports; p++) {
        if (!(pending & (1u << (p - 1u)))) continue;
        uint32_t portsc = portsc_read((uint8_t)(p - 1u));
        bool ccs = (portsc & PORTSC_CCS) != 0;
        uint8_t cur_slot = xhci_slot_for_root_port(p);
        if (ccs && !cur_slot) {
            /* Cable just inserted -- bring the device up. */
            kprintf("[xhci] hot-attach root port %u (PORTSC=0x%08x)\n",
                    p, portsc);
            (void)xhci_attach_root_port(p);
        } else if (!ccs && cur_slot) {
            /* Cable just yanked -- tear the slot down. */
            kprintf("[xhci] hot-detach root port %u (slot %u)\n",
                    p, cur_slot);
            (void)xhci_detach_slot(cur_slot);
        }
        /* ccs && cur_slot -- already addressed, just a transient
         * change (e.g. PEC after PED toggle); nothing to do.
         * !ccs && !cur_slot -- empty port saw a glitch; ignore. */
    }
}

/* ============================================================== */
/* Event ring drain (xhci_poll)                                    */
/* ============================================================== */

void xhci_poll(void) {
    if (!g_xhci.bound) return;

    /* Save RFLAGS + cli so the body is atomic w.r.t. our own MSI
     * handler. Works correctly from both kernel context (IF=1: cli
     * masks, popfq re-enables) and IRQ context (IF=0: cli no-op,
     * popfq leaves IF=0). Without this, an MSI arriving while the
     * idle loop is mid-drain would double-consume an event. */
    uint64_t flags;
    __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    for (;;) {
        struct trb *e = &g_xhci.evt_ring[g_xhci.evt_idx];
        uint32_t ctl = e->control;
        if ((ctl & TRB_CYCLE) != g_xhci.evt_cycle) break;   /* nothing new */

        uint8_t type = (uint8_t)TRB_GET_TYPE(ctl);
        uint8_t cc   = (uint8_t)TRB_CC(e->status);

        if (type == TRB_TYPE_CMD_COMPLETION) {
            g_xhci.cmd_completion_code = cc;
            g_xhci.cmd_slot_id         = (uint8_t)TRB_GET_SLOT(ctl);
            g_xhci.cmd_complete        = true;
        } else if (type == TRB_TYPE_TRANSFER_EVENT) {
            uint8_t slot_id = (uint8_t)TRB_GET_SLOT(ctl);
            uint8_t ep_dci  = (uint8_t)TRB_GET_EP(ctl);
            struct xhci_dev_state *st = get_dev_state(slot_id);
            uint32_t bytes_left = e->status & 0x00FFFFFFu;
            if (st && st->in_use) {
                if (ep_dci == 1) {
                    /* EP0 control transfer Status-Stage event. */
                    g_ep0_complete[slot_id] = true;
                    g_ep0_cc[slot_id]       = cc;
                } else if (ep_dci == st->usb.int_dci) {
                    /* HID interrupt-IN transfer event. */
                    if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
                        uint32_t got = (st->usb.int_buf_size > bytes_left)
                            ? (st->usb.int_buf_size - bytes_left) : 0;
                        if (st->usb.int_complete && st->usb.int_buf) {
                            st->usb.int_complete(&st->usb,
                                                 st->usb.int_buf, got);
                        }
                    }
                    st->usb.int_armed = false;
                    /* Re-arm immediately for the next report. */
                    xhci_submit_int_in(&st->usb);
                } else if ((ep_dci == st->usb.bulk_in_dci ||
                            ep_dci == st->usb.bulk_out_dci) &&
                           ep_dci > 0 && ep_dci < XHCI_MAX_DCI) {
                    /* USB Mass-Storage bulk transfer completion. The
                     * synchronous xhci_bulk_xfer_sync() spinner reads
                     * cc + residue back out of these arrays. */
                    g_bulk_cc      [slot_id][ep_dci] = cc;
                    g_bulk_residue [slot_id][ep_dci] = bytes_left;
                    g_bulk_complete[slot_id][ep_dci] = true;
                }
            }
        } else if (type == TRB_TYPE_PORT_STATUS) {
            /* Param[31:24] = port number that changed. We RW1C the
             * change bits *here* (in IRQ context) but defer the
             * actual attach/detach work to xhci_service_port_changes(),
             * which is called from the kernel idle loop. Issuing
             * Enable/Disable Slot synchronously from MSI context would
             * deadlock our own xhci_cmd spin. */
            uint8_t port = (uint8_t)((e->param_lo >> 24) & 0xFFu);
            if (port >= 1 && port <= g_xhci.max_ports) {
                portsc_write(port - 1u, PORTSC_RW1C_MASK);
                /* Mark the deferred service work; xhci_service_port_changes
                 * checks the bit, drains any work, and atomically resets
                 * it. The OR is safe under the cli around xhci_poll(). */
                g_xhci.port_change_pending |= (1u << (port - 1u));
            }
        }
        /* (Other event types -- bandwidth request, MFINDEX wrap, etc. --
         * we just ignore. They're not generated for our usage.) */

        /* Advance the dequeue pointer + cycle on wrap. */
        g_xhci.evt_idx++;
        if (g_xhci.evt_idx >= XHCI_EVT_TRBS) {
            g_xhci.evt_idx = 0;
            g_xhci.evt_cycle ^= 1u;
        }
    }

    /* Update the controller's view of where we are. EHB (bit 3) is
     * RW1C, set to acknowledge any latched event interrupt. */
    uint64_t deq_phys = g_xhci.evt_ring_phys +
                        (uint64_t)g_xhci.evt_idx * sizeof(struct trb);
    rt_w32(XHCI_RT_IR0 + XHCI_IR_ERDP_LO,
           (uint32_t)((deq_phys & 0xFFFFFFF0u) | (1u << 3)));
    rt_w32(XHCI_RT_IR0 + XHCI_IR_ERDP_HI, (uint32_t)(deq_phys >> 32));

    __asm__ __volatile__("push %0; popfq" :: "r"(flags) : "memory");
}

/* MSI / MSI-X handler. The chip raises IR0.IMAN.IP (bit 0) on every
 * event ring entry; we ack it AND USBSTS.EINT before draining, so a
 * burst of events that arrive while we're inside xhci_poll() re-arms
 * cleanly on the next vector edge. apic_eoi is sent by the dyn-vec
 * trampoline after we return. */
static void xhci_irq_handler(void *ctx) {
    (void)ctx;
    if (!g_xhci.bound) return;
    g_xhci.irq_count++;

    /* Ack the controller-level interrupt latch. USBSTS.EINT is RW1C. */
    uint32_t sts = op_r32(XHCI_OP_USBSTS);
    if (sts & USBSTS_EINT) op_w32(XHCI_OP_USBSTS, USBSTS_EINT);

    /* Ack the per-interrupter pending bit. IMAN.IP is RW1C; preserve IE. */
    uint32_t iman = rt_r32(XHCI_RT_IR0 + XHCI_IR_IMAN);
    rt_w32(XHCI_RT_IR0 + XHCI_IR_IMAN, iman | IMAN_IP);

    xhci_poll();
}

/* ============================================================== */
/* Controller init                                                 */
/* ============================================================== */

static bool xhci_controller_init(void) {
    /* Halt + reset. CNR (Controller Not Ready) clears when the chip
     * has finished initialising its internal state. */
    op_w32(XHCI_OP_USBCMD, 0);
    for (int i = 0; i < 100; i++) {
        if (op_r32(XHCI_OP_USBSTS) & USBSTS_HCH) break;
        pit_sleep_ms(10);
    }
    op_w32(XHCI_OP_USBCMD, USBCMD_HCRST);
    for (int i = 0; i < 200; i++) {
        if (!(op_r32(XHCI_OP_USBCMD) & USBCMD_HCRST)) break;
        pit_sleep_ms(10);
    }
    if (op_r32(XHCI_OP_USBCMD) & USBCMD_HCRST) {
        kprintf("[xhci] reset timeout (USBCMD=0x%08x)\n",
                op_r32(XHCI_OP_USBCMD));
        return false;
    }
    for (int i = 0; i < 200; i++) {
        if (!(op_r32(XHCI_OP_USBSTS) & USBSTS_CNR)) break;
        pit_sleep_ms(10);
    }
    if (op_r32(XHCI_OP_USBSTS) & USBSTS_CNR) {
        kprintf("[xhci] CNR never cleared\n");
        return false;
    }

    /* Tell the controller how many slots we'll use. */
    uint8_t want_slots = g_xhci.max_slots < XHCI_MAX_SLOTS
                         ? g_xhci.max_slots : XHCI_MAX_SLOTS;
    op_w32(XHCI_OP_CONFIG,
           (op_r32(XHCI_OP_CONFIG) & ~0xFFu) | want_slots);

    /* DCBAA: one slot per device (0..MaxSlotsEn). One page is plenty. */
    g_xhci.dcbaa = (uint64_t *)alloc_zero_page(&g_xhci.dcbaa_phys);
    if (!g_xhci.dcbaa) return false;

    /* Shared DMA scratch page for descriptor reads. HHDM-mapped, so
     * pmm_phys_to_virt + the matching phys give us a buffer we can
     * feed straight into a Data Stage TRB without page-table lookups. */
    g_xhci.desc_buf = (uint8_t *)alloc_zero_page(&g_xhci.desc_buf_phys);
    if (!g_xhci.desc_buf) return false;

    /* Scratchpad buffers (HCSPARAMS2 fields). DCBAA[0] points at an
     * array of pointers to OS-allocated, controller-private pages.
     * Even if num_scratch == 0 we leave dcbaa[0] = 0 (the spec says
     * the controller treats that as "no scratchpad"). */
    if (g_xhci.num_scratch > 0) {
        if (g_xhci.num_scratch > 64) g_xhci.num_scratch = 64;
        g_xhci.spad_array =
            (uint64_t *)alloc_zero_page(&g_xhci.spad_array_phys);
        if (!g_xhci.spad_array) return false;
        for (uint32_t i = 0; i < g_xhci.num_scratch; i++) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) return false;
            memset(pmm_phys_to_virt(phys), 0, PAGE_SIZE);
            g_xhci.spad_array[i] = phys;
        }
        g_xhci.dcbaa[0] = g_xhci.spad_array_phys;
    }

    op_w32(XHCI_OP_DCBAAP_LO, (uint32_t)(g_xhci.dcbaa_phys & 0xFFFFFFFFu));
    op_w32(XHCI_OP_DCBAAP_HI, (uint32_t)(g_xhci.dcbaa_phys >> 32));

    /* Command ring (single segment + LINK). */
    g_xhci.cmd_ring  = make_ring(&g_xhci.cmd_ring_phys);
    if (!g_xhci.cmd_ring) return false;
    g_xhci.cmd_idx   = 0;
    g_xhci.cmd_cycle = 1;
    /* CRCR: phys | RCS=1 (Ring Cycle State, matches our producer). */
    op_w32(XHCI_OP_CRCR_LO, (uint32_t)(g_xhci.cmd_ring_phys & 0xFFFFFFC0u) | 1u);
    op_w32(XHCI_OP_CRCR_HI, (uint32_t)(g_xhci.cmd_ring_phys >> 32));

    /* Event ring (single segment) + ERST. */
    g_xhci.evt_ring  = (struct trb *)alloc_zero_page(&g_xhci.evt_ring_phys);
    g_xhci.erst      = (struct xhci_erst_entry *)alloc_zero_page(&g_xhci.erst_phys);
    if (!g_xhci.evt_ring || !g_xhci.erst) return false;
    g_xhci.evt_idx   = 0;
    g_xhci.evt_cycle = 1;
    g_xhci.erst[0].base = g_xhci.evt_ring_phys;
    g_xhci.erst[0].size = XHCI_EVT_TRBS;
    /* Primary interrupter (IR0). */
    rt_w32(XHCI_RT_IR0 + XHCI_IR_ERSTSZ, 1);
    rt_w32(XHCI_RT_IR0 + XHCI_IR_ERDP_LO,
           (uint32_t)((g_xhci.evt_ring_phys & 0xFFFFFFF0u) | (1u << 3)));
    rt_w32(XHCI_RT_IR0 + XHCI_IR_ERDP_HI, (uint32_t)(g_xhci.evt_ring_phys >> 32));
    rt_w32(XHCI_RT_IR0 + XHCI_IR_ERSTBA_LO, (uint32_t)(g_xhci.erst_phys & 0xFFFFFFFFu));
    rt_w32(XHCI_RT_IR0 + XHCI_IR_ERSTBA_HI, (uint32_t)(g_xhci.erst_phys >> 32));
    /* Mask the primary interrupter (IE=0). We poll. */
    rt_w32(XHCI_RT_IR0 + XHCI_IR_IMAN, 0);

    /* Run! INTE off, HSEE off; we drive the event ring purely from
     * software polling. */
    op_w32(XHCI_OP_USBCMD, USBCMD_RUN);

    /* Wait for HCH to clear (controller actually running). */
    for (int i = 0; i < 100; i++) {
        if (!(op_r32(XHCI_OP_USBSTS) & USBSTS_HCH)) break;
        pit_sleep_ms(10);
    }
    if (op_r32(XHCI_OP_USBSTS) & USBSTS_HCH) {
        kprintf("[xhci] controller never started running\n");
        return false;
    }
    return true;
}

/* ============================================================== */
/* PCI probe                                                       */
/* ============================================================== */

static int xhci_probe(struct pci_dev *dev) {
    if (g_xhci.bound) {
        kprintf("[xhci] already bound -- ignoring %02x:%02x.%x\n",
                dev->bus, dev->slot, dev->fn);
        return -1;
    }

    kprintf("[xhci] probing %02x:%02x.%x  (vid:did %04x:%04x  class %02x:%02x:%02x)\n",
            dev->bus, dev->slot, dev->fn, dev->vendor, dev->device,
            dev->class_code, dev->subclass, dev->prog_if);

    pci_dev_enable(dev, PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);

    /* xHCI lives at BAR0; it's always 64-bit MMIO on real silicon.
     * Map the full BAR (size auto-detected by pci_init). */
    void *bar0 = pci_map_bar(dev, 0, 0);
    if (!bar0) {
        kprintf("[xhci] BAR0 map failed (phys=%p)\n", (void *)dev->bar[0]);
        return -2;
    }
    g_xhci.cap_regs = (volatile uint8_t *)bar0;
    kprintf("[xhci] MMIO BAR0 phys=%p virt=%p\n",
            (void *)dev->bar[0], (void *)g_xhci.cap_regs);

    /* QEMU's qemu-xhci only handles dword-aligned reads of the cap
     * registers (its xhci_cap_read switches on a list of dword
     * offsets); a sub-dword read at 0x02 returns 0. Read the dword
     * at 0x00 and extract CAPLENGTH (low byte) + HCIVERSION (high
     * 16 bits) so we get a sane version string on both QEMU and
     * real hardware. */
    uint32_t cap0     = cap_r32(0);
    uint8_t  caplength = (uint8_t)(cap0 & 0xFFu);
    g_xhci.version    = (uint16_t)(cap0 >> 16);
    uint32_t hcs1     = cap_r32(XHCI_CAP_HCSPARAMS1);
    uint32_t hcs2     = cap_r32(XHCI_CAP_HCSPARAMS2);
    g_xhci.hccparams1 = cap_r32(XHCI_CAP_HCCPARAMS1);
    uint32_t dboff    = cap_r32(XHCI_CAP_DBOFF)  & ~0x3u;
    uint32_t rtsoff   = cap_r32(XHCI_CAP_RTSOFF) & ~0x1Fu;

    g_xhci.op_regs = g_xhci.cap_regs + caplength;
    g_xhci.db_regs = g_xhci.cap_regs + dboff;
    g_xhci.rt_regs = g_xhci.cap_regs + rtsoff;

    g_xhci.max_slots   = HCSPARAMS1_MAX_SLOTS(hcs1);
    g_xhci.max_intrs   = HCSPARAMS1_MAX_INTRS(hcs1);
    g_xhci.max_ports   = HCSPARAMS1_MAX_PORTS(hcs1);
    g_xhci.num_scratch = HCSPARAMS2_MAX_SCRATCH(hcs2);
    g_xhci.ctx64       = HCCPARAMS1_CSZ(g_xhci.hccparams1);
    g_xhci.xecp_off    = (uint16_t)(HCCPARAMS1_XECP(g_xhci.hccparams1) * 4u);

    kprintf("[xhci] HCIVERSION=0x%04x  CAPLENGTH=0x%02x  CSZ=%u  AC64=%u\n",
            g_xhci.version, caplength, g_xhci.ctx64,
            HCCPARAMS1_AC64(g_xhci.hccparams1));
    kprintf("[xhci] MaxSlots=%u  MaxIntrs=%u  MaxPorts=%u  Scratch=%u\n",
            g_xhci.max_slots, g_xhci.max_intrs,
            g_xhci.max_ports, g_xhci.num_scratch);
    kprintf("[xhci] op_regs=+0x%02x  rt_regs=+0x%x  db_regs=+0x%x  xECP=+0x%x\n",
            caplength, rtsoff, dboff, g_xhci.xecp_off);

    if (g_xhci.ctx64) {
        kprintf("[xhci] WARN: CSZ=1 (64-byte contexts) NOT YET SUPPORTED -- aborting\n");
        return -3;
    }
    if (g_xhci.max_ports == 0 || g_xhci.max_ports > XHCI_MAX_PORTS) {
        kprintf("[xhci] silly MaxPorts=%u -- aborting\n", g_xhci.max_ports);
        return -4;
    }

    xhci_bios_handoff();

    if (!xhci_controller_init()) return -5;

    g_xhci.bound = true;
    kprintf("[xhci] controller online -- enumerating %u root-hub port(s)\n",
            g_xhci.max_ports);

    /* Walk every port. Devices that are present at boot get probed
     * here; later hot-plug events go through PORT_STATUS_CHANGE in
     * xhci_poll (which currently just clears change bits). All of
     * this still runs in pure-polled mode -- the busy-waits in
     * xhci_cmd / xhci_control_class drive xhci_poll directly. We
     * only flip the IR0 IE bit AFTER everything is enumerated, so
     * the interrupt-IN reports for HID devices are the very first
     * events we receive via MSI. */
    for (uint8_t p = 0; p < g_xhci.max_ports; p++) {
        enumerate_port(p);
    }

    /* Try MSI-X first (xHCI 1.1 mandates it on the primary
     * interrupter), fall back to plain MSI. We only need a single
     * vector -- IR0 carries every event in our event-ring topology.
     * If neither cap exists we leave the controller in pure-polled
     * mode (xhci_poll() in idle_loop() keeps it serviced). */
    uint8_t vec = irq_alloc_vector(xhci_irq_handler, &g_xhci);
    if (vec == 0) {
        kprintf("[xhci] no IDT vectors free -- staying polled\n");
    } else if (pci_msix_enable(dev, vec, (uint8_t)apic_read_id(), 1u)) {
        g_xhci.irq_vector  = vec;
        g_xhci.irq_enabled = true;
    } else if (pci_msi_enable(dev, vec, (uint8_t)apic_read_id())) {
        g_xhci.irq_vector  = vec;
        g_xhci.irq_enabled = true;
    } else {
        kprintf("[xhci] no MSI / MSI-X cap -- staying polled "
                "(vec 0x%02x is now idle)\n", (unsigned)vec);
    }

    if (g_xhci.irq_enabled) {
        /* Ack any stale pending bit from boot, then unmask. The
         * sequence here matters: clear EINT first, IMAN.IP second,
         * then set IMAN.IE last so the very first edge after we
         * write USBCMD.INTE is one we actually catch. */
        op_w32(XHCI_OP_USBSTS, USBSTS_EINT);
        rt_w32(XHCI_RT_IR0 + XHCI_IR_IMAN, IMAN_IP | IMAN_IE);
        op_w32(XHCI_OP_USBCMD, op_r32(XHCI_OP_USBCMD) | USBCMD_INTE);
        kprintf("[xhci] IRQ live on vec 0x%02x  (IR0.IE + USBCMD.INTE set)\n",
                (unsigned)g_xhci.irq_vector);
    }

    dev->driver_data = &g_xhci;
    return 0;
}

/* xHCI is identified by class triple Serial Bus / USB / xHCI prog-if.
 * That covers every real-hardware xHCI controller AND QEMU's
 * `-device qemu-xhci` (which advertises 1b36:000d but is also picked
 * up by the class match). Keeping it class-only means we don't have
 * to chase per-vendor PCI ids -- there are dozens (Intel, AMD, NEC,
 * ASMedia, Renesas, VIA, Fresco Logic, ...). */
static const struct pci_match g_xhci_matches[] = {
    { PCI_ANY_ID, PCI_ANY_ID,
      PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB, PCI_PROGIF_XHCI },
    PCI_MATCH_END,
};

static struct pci_driver g_xhci_driver = {
    .name    = "xhci",
    .matches = g_xhci_matches,
    .probe   = xhci_probe,
    .remove  = 0,
};

void xhci_register(void) {
    pci_register_driver(&g_xhci_driver);
}

/* ============================================================
 * M26A introspection -- read-only snapshot for devtest harness.
 * ============================================================
 *
 * Iteration model: dense index `idx` over IN_USE slot entries.
 * Slot 0 is reserved by the xHCI spec, so we skip it; everything
 * after is sparse (any slot with .in_use == false is empty).
 *
 * We translate the per-slot state into the kernel's
 * struct abi_dev_info layout. The xhci.c-internal device structure
 * tracks which class driver claimed the device (HID via int_complete,
 * MSC via msc_state) -- that lets us name-tag the bound driver
 * without exposing class-driver internals.
 */

#include <tobyos/abi/abi.h>
#include <tobyos/klibc.h>

bool xhci_present(void) { return g_xhci.bound; }

static const char *xhci_dev_class_name(const struct usb_device *u) {
    if (u->is_hub)                                         return "usb_hub";
    if (u->msc_state || u->bulk_in_dci || u->bulk_out_dci) return "usb_msc";
    if (u->int_complete || u->hid_state)                   return "usb_hid";
    return "";
}

static int find_nth_in_use(int idx) {
    int seen = 0;
    for (int s = 1; s <= XHCI_MAX_SLOTS; s++) {
        if (!g_xhci.devs[s].in_use) continue;
        if (seen == idx) return s;
        seen++;
    }
    return -1;
}

int xhci_introspect_count(void) {
    if (!g_xhci.bound) return 0;
    int n = 0;
    for (int s = 1; s <= XHCI_MAX_SLOTS; s++) {
        if (g_xhci.devs[s].in_use) n++;
    }
    return n;
}

int xhci_introspect_at(int idx, struct abi_dev_info *out) {
    if (!out || !g_xhci.bound) return 0;
    int s = find_nth_in_use(idx);
    if (s < 0) return 0;
    const struct usb_device *u = &g_xhci.devs[s].usb;

    memset(out, 0, sizeof(*out));
    out->bus       = ABI_DEVT_BUS_USB;
    out->status    = ABI_DEVT_PRESENT | ABI_DEVT_BOUND;
    /* M26B: hub topology -- depth=0 for direct root-hub devices,
     *                       depth=N+1 / hub_port=K for "behind hub on port K".
     * For root-hub devices we fall back to port_id so 'devlist usb' is still
     * usefully labelled even before any hubs land in the tree. */
    out->hub_depth = u->hub_depth;
    out->hub_port  = u->hub_depth ? u->hub_port : u->port_id;
    out->vendor    = 0;             /* USB IDs aren't kept on the slot today */
    out->device    = 0;
    /* class_code: we report 0xFF here -- USB device class is read from
     * the device descriptor at probe time and not cached on usb_device.
     * The bound-driver name (below) is what userland actually wants. */
    out->class_code = 0xFF;
    out->index      = (uint8_t)s;

    /* "usb1-2" for root devices, "usb1-2.3" for behind-hub devices.
     * Bus is always 1 (we only support one xHCI). */
    if (u->hub_depth == 0) {
        ksnprintf(out->name, ABI_DEVT_NAME_MAX, "usb1-%u",
                  (unsigned)u->port_id);
    } else {
        ksnprintf(out->name, ABI_DEVT_NAME_MAX, "usb1-%u.%u",
                  (unsigned)u->port_id,
                  (unsigned)u->hub_port);
    }
    const char *dn = xhci_dev_class_name(u);
    size_t n = 0;
    while (dn[n] && n + 1 < ABI_DEVT_DRIVER_MAX) {
        out->driver[n] = dn[n]; n++;
    }
    out->driver[n] = '\0';

    /* extra: speed code + slot. */
    const char *sp = "?";
    switch (u->speed) {
    case 1: sp = "FS"; break;       /* full-speed (12 Mbit) */
    case 2: sp = "LS"; break;       /* low-speed   (1.5 M) */
    case 3: sp = "HS"; break;       /* high-speed (480 M)  */
    case 4: sp = "SS"; break;       /* super-speed (5 G)   */
    }
    if (u->hub_depth) {
        ksnprintf(out->extra, ABI_DEVT_EXTRA_MAX,
                  "slot=%u speed=%s mps0=%u parent=%u route=0x%05x",
                  (unsigned)u->slot_id, sp, (unsigned)u->mps0,
                  (unsigned)u->parent_slot_id,
                  (unsigned)u->route_string);
    } else if (u->is_hub) {
        ksnprintf(out->extra, ABI_DEVT_EXTRA_MAX,
                  "slot=%u speed=%s mps0=%u hub nports=%u",
                  (unsigned)u->slot_id, sp, (unsigned)u->mps0,
                  (unsigned)u->hub_nports);
    } else {
        ksnprintf(out->extra, ABI_DEVT_EXTRA_MAX,
                  "slot=%u speed=%s mps0=%u",
                  (unsigned)u->slot_id, sp, (unsigned)u->mps0);
    }
    return 1;
}

int xhci_irq_count(void) {
    if (!g_xhci.bound) return 0;
    /* Cast through unsigned to avoid losing the high bits on overflow
     * (a long-running session would only ever lose count if irq_count
     * exceeds INT_MAX, which won't happen in any realistic boot). */
    return (int)(g_xhci.irq_count & 0x7FFFFFFFu);
}

int xhci_selftest(char *msg, size_t cap) {
    if (!g_xhci.bound) {
        ksnprintf(msg, cap,
                  "no xHCI controller present (run with -device qemu-xhci)");
        return ABI_DEVT_SKIP;
    }
    if (!g_xhci.cap_regs || !g_xhci.op_regs) {
        ksnprintf(msg, cap, "xHCI bound but BAR0 not mapped (init failure)");
        return -ABI_EIO;
    }
    /* USBSTS.HCH must NOT be set when the controller is running. */
    uint32_t usbsts = op_r32(XHCI_OP_USBSTS);
    if (usbsts & USBSTS_HCH) {
        ksnprintf(msg, cap,
                  "xHCI HALTED (USBSTS=0x%x), HCRESET sequence failed?",
                  (unsigned)usbsts);
        return -ABI_EIO;
    }
    ksnprintf(msg, cap,
              "xHCI v%u.%x slots=%u ports=%u irq=%s/%lu",
              (unsigned)((g_xhci.version >> 8) & 0xFF),
              (unsigned)(g_xhci.version & 0xFF),
              (unsigned)g_xhci.max_slots,
              (unsigned)g_xhci.max_ports,
              g_xhci.irq_enabled ? "on" : "off",
              (unsigned long)g_xhci.irq_count);
    return 0;
}

int xhci_devices_selftest(char *msg, size_t cap) {
    if (!g_xhci.bound) {
        ksnprintf(msg, cap, "no xHCI controller, no USB devices");
        return ABI_DEVT_SKIP;
    }
    int n = xhci_introspect_count();
    if (n == 0) {
        ksnprintf(msg, cap,
                  "xHCI present but no USB devices enumerated this boot");
        return ABI_DEVT_SKIP;
    }
    /* Sanity: each in_use slot should have a real slot_id and a
     * non-NULL ep0 ring. If not, that's a hard failure. */
    int hid = 0, msc = 0, other = 0;
    for (int s = 1; s <= XHCI_MAX_SLOTS; s++) {
        const struct xhci_dev_state *ds = &g_xhci.devs[s];
        if (!ds->in_use) continue;
        const struct usb_device *u = &ds->usb;
        if (u->slot_id == 0 || !u->ep0_ring) {
            ksnprintf(msg, cap,
                      "slot %d marked in_use but slot_id=%u ep0_ring=%p",
                      s, (unsigned)u->slot_id, u->ep0_ring);
            return -ABI_EIO;
        }
        if (u->msc_state || u->bulk_in_dci || u->bulk_out_dci) msc++;
        else if (u->int_complete || u->hid_state)              hid++;
        else                                                   other++;
    }
    ksnprintf(msg, cap, "%d USB device(s): hid=%d msc=%d other=%d",
              n, hid, msc, other);
    return 0;
}
