/* blk_nvme.c -- NVMe 1.x driver (admin queue + one I/O queue, polled).
 *
 * Bound through the milestone-21 PCI driver registry. Matches any
 * mass-storage NVMe controller (class 0x01, subclass 0x08). On QEMU
 * `-device nvme` exposes a Red Hat / KVM device (1b36:0010). On real
 * hardware it's whatever NVMe SSD is plugged in (Samsung, WD, Intel,
 * Phison, ...).
 *
 * Architecture
 * ------------
 *
 *   1. BAR0 is mapped via pci_map_bar(NOCACHE). It covers the
 *      controller registers (CAP, VS, INTMS/INTMC, CC, CSTS, AQA,
 *      ASQ, ACQ) at offset 0x00..0xFFC, plus the doorbell array at
 *      0x1000+. Doorbell stride is (4 << CAP.DSTRD) bytes (4 on QEMU
 *      and most real controllers).
 *
 *   2. Reset + initialisation:
 *        - Clear CC.EN, wait for CSTS.RDY = 0.
 *        - pmm_alloc_page() one page each for the Admin Submission
 *          Queue (64 entries x 64 B = 4 KiB), Admin Completion Queue
 *          (64 entries x 16 B = 1 KiB, page over-covers it), and a
 *          shared PRP-list scratch page.
 *        - Program AQA / ASQ / ACQ with those phys addresses.
 *        - Mask all interrupts (INTMS = ~0).
 *        - Write CC = MPS=0 (4 KiB) | CSS=NVM | IOSQES=6 | IOCQES=4
 *          | EN=1 in a single transaction (per spec init flow).
 *        - Wait for CSTS.RDY = 1.
 *
 *   3. IDENTIFY CONTROLLER (admin opcode 0x06, CNS=1) -- learn NN
 *      (number of namespaces) and the controller serial number.
 *
 *   4. CREATE I/O CQ (0x05) + CREATE I/O SQ (0x01) for queue id 1.
 *      One page each, 64 entries each. PC=1 (physically contiguous),
 *      IEN=0 (no interrupts -- polling).
 *
 *   5. For each NSID 1..NN (capped at NVME_MAX_NS_PER_CTRL):
 *      IDENTIFY NAMESPACE (0x06, CNS=0). On success, look up the
 *      current LBA format (FLBAS index into LBAF[0..NLBAF]) and read
 *      its LBADS. Skip namespaces with non-512 LBA size for now --
 *      tobyfs and the installer assume 512-byte sectors and we don't
 *      yet do block-size translation in the hot path.
 *
 *   6. blk_register() with name "nvme<H>:n<N>" (H = HBA index, N =
 *      namespace id). Reads/writes funnel through the I/O queue:
 *        - Build a 64-byte SQE for READ (0x02) or WRITE (0x01).
 *        - PRP1/PRP2 from the caller's kernel-virt buffer via
 *          vmm_translate. <= 1 page after PRP1's offset = PRP2 unused;
 *          <= 2 pages = PRP2 is page 2's phys; > 2 pages = PRP2 is the
 *          phys of the controller's PRP-list page (1 page = 512
 *          entries = 2 MiB max), and we cap I/O at 256 KiB / call.
 *        - Submit by writing SQ tail doorbell.
 *        - Poll CQ for matching CID + phase tag flip; advance head;
 *          ring CQ head doorbell.
 *
 *   7. IRQs are masked at every level (INTMS = ~0). We poll. Same
 *      model as blk_ata, blk_ahci. NVMe IRQ steering / MSI-X comes
 *      with the milestone-21 IRQ overhaul, not this step.
 *
 *   8. 64-bit DMA always. NVMe is PCIe-only, no 32-bit-only legacy.
 */

#include <tobyos/blk.h>
#include <tobyos/pci.h>
#include <tobyos/pmm.h>
#include <tobyos/vmm.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/cpu.h>
#include <tobyos/pit.h>
#include <tobyos/irq.h>
#include <tobyos/apic.h>

/* ---- controller register offsets ------------------------------- */

#define NVME_CAP            0x0000      /* 64-bit */
#define NVME_VS             0x0008
#define NVME_INTMS          0x000C
#define NVME_INTMC          0x0010
#define NVME_CC             0x0014
#define NVME_CSTS           0x001C
#define NVME_AQA            0x0024
#define NVME_ASQ            0x0028      /* 64-bit */
#define NVME_ACQ            0x0030      /* 64-bit */

#define NVME_CC_EN          (1u << 0)
#define NVME_CSTS_RDY       (1u << 0)
#define NVME_CSTS_CFS       (1u << 1)

/* CC field encodings used for our (NVM, 4 KiB MPS) controller. */
#define NVME_CC_CSS_NVM     (0u << 4)
#define NVME_CC_MPS_4K      (0u << 7)
#define NVME_CC_AMS_RR      (0u << 11)
#define NVME_CC_IOSQES_64   (6u << 16)
#define NVME_CC_IOCQES_16   (4u << 20)

/* Doorbell math. Doorbells live at 0x1000 onward, in pairs (SQyTDBL
 * then CQyHDBL), each (4 << DSTRD) bytes wide. */
#define NVME_DBL_SQ(qid, dstrd)  (0x1000u + (2u * (qid))     * (4u << (dstrd)))
#define NVME_DBL_CQ(qid, dstrd)  (0x1000u + (2u * (qid) + 1u) * (4u << (dstrd)))

/* Admin opcodes. */
#define NVME_ADM_CREATE_SQ  0x01
#define NVME_ADM_CREATE_CQ  0x05
#define NVME_ADM_IDENTIFY   0x06

/* CREATE I/O CQ CDW11 fields. */
#define NVME_CQ_PC          (1u << 0)       /* physically contiguous */
#define NVME_CQ_IEN         (1u << 1)       /* interrupts enabled    */

/* I/O opcodes. */
#define NVME_IO_WRITE       0x01
#define NVME_IO_READ        0x02

/* IDENTIFY CNS values. */
#define NVME_CNS_NAMESPACE  0x00
#define NVME_CNS_CONTROLLER 0x01

/* ---- driver capacities ----------------------------------------- */

#define NVME_QDEPTH             64u
#define NVME_PRP_LIST_ENTRIES   (PAGE_SIZE / 8u)        /* 512 */
#define NVME_MAX_BYTES_PER_OP   (256u * 1024u)          /* 256 KiB */
#define NVME_MAX_SECTORS_PER_OP (NVME_MAX_BYTES_PER_OP / BLK_SECTOR_SIZE)
#define NVME_MAX_CONTROLLERS    4
#define NVME_MAX_NS_PER_CTRL    8
#define NVME_MAX_DRIVES         8

/* ---- on-the-wire structures ------------------------------------ */

struct __attribute__((packed)) nvme_sqe {
    uint8_t  opc;            /* DW0 [7:0]   = opcode */
    uint8_t  flags;          /* DW0 [15:8]  = FUSE/PSDT/etc. (0 for our use) */
    uint16_t cid;            /* DW0 [31:16] = command id */
    uint32_t nsid;           /* DW1                                          */
    uint64_t reserved;       /* DW2/3                                        */
    uint64_t mptr;           /* DW4/5  metadata pointer (unused: no PI)      */
    uint64_t prp1;           /* DW6/7  data buffer page 1                    */
    uint64_t prp2;           /* DW8/9  data buffer page 2 OR PRP list        */
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
};

struct __attribute__((packed)) nvme_cqe {
    uint32_t result;         /* DW0  command-specific result */
    uint32_t reserved;       /* DW1                            */
    uint16_t sqhd;           /* DW2 [15:0]   SQ head pointer  */
    uint16_t sqid;           /* DW2 [31:16]  SQ id             */
    uint16_t cid;            /* DW3 [15:0]   command id        */
    uint16_t status;         /* DW3 [31:16]: bit 0 = phase, bits 15:1 = SF */
};

_Static_assert(sizeof(struct nvme_sqe) == 64, "NVMe SQE must be 64 bytes");
_Static_assert(sizeof(struct nvme_cqe) == 16, "NVMe CQE must be 16 bytes");

/* ---- driver state --------------------------------------------- */

struct nvme_queue {
    uint16_t           qid;
    uint16_t           depth;
    uint16_t           sq_tail;
    uint16_t           cq_head;
    uint8_t            cq_phase;       /* expected phase: starts at 1 */
    uint64_t           sq_phys;
    struct nvme_sqe   *sq;
    uint64_t           cq_phys;
    struct nvme_cqe   *cq;
    volatile uint32_t *sq_dbl;
    volatile uint32_t *cq_dbl;
};

struct nvme_controller {
    int                idx;             /* slot in g_ctrls (= HBA index) */
    volatile uint8_t  *bar;
    uint32_t           dstrd;
    uint16_t           next_cid;
    struct nvme_queue  admin;
    struct nvme_queue  io;
    uint64_t           prp_list_phys;
    uint64_t          *prp_list;

    /* MSI bring-up state. irq_enabled is set if pci_msi_enable() and
     * irq_alloc_vector() both succeeded; we only flip IEN in CREATE
     * I/O CQ when this is true. irq_count is bumped by the ISR -- the
     * shell + regression sweep can use it as a "yes, the controller
     * really did fire an MSI" sanity check. */
    uint8_t            irq_vector;
    bool               irq_enabled;
    volatile uint64_t  irq_count;
};

struct nvme_namespace {
    struct nvme_controller *ctrl;
    uint32_t                nsid;
    uint64_t                nsze;        /* in 512-byte sectors */
    uint32_t                lba_size;    /* always 512 in this driver */
};

struct nvme_drive {
    struct nvme_namespace ns;
    char                  name[32];
    struct blk_dev        blk;
};

static struct nvme_controller g_ctrls[NVME_MAX_CONTROLLERS];
static size_t                 g_ctrl_count;
static struct nvme_drive      g_drives[NVME_MAX_DRIVES];
static size_t                 g_drive_count;

/* Forward decl so nvme_init_controller can wire it into blk_dev.ops
 * even though the function bodies are defined further down. */
static const struct blk_ops g_nvme_ops;

/* ---- MMIO helpers --------------------------------------------- */

static inline uint32_t reg_r32(volatile uint8_t *bar, uint32_t off) {
    return *(volatile uint32_t *)(bar + off);
}
static inline void reg_w32(volatile uint8_t *bar, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(bar + off) = v;
}
static inline uint64_t reg_r64(volatile uint8_t *bar, uint32_t off) {
    /* Some platforms (and the QEMU virt-mmio NVMe path) only allow 32-bit
     * MMIO access to the controller bar. Reading 64-bit registers as two
     * 32-bit accesses is always safe; reading them as one 64-bit access
     * is not. So we do the conservative thing. */
    uint32_t lo = *(volatile uint32_t *)(bar + off);
    uint32_t hi = *(volatile uint32_t *)(bar + off + 4);
    return ((uint64_t)hi << 32) | lo;
}
static inline void reg_w64(volatile uint8_t *bar, uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(bar + off)     = (uint32_t)(v & 0xFFFFFFFFu);
    *(volatile uint32_t *)(bar + off + 4) = (uint32_t)(v >> 32);
}

/* ---- timing helpers ------------------------------------------- */

static int wait_until32(volatile uint8_t *reg, uint32_t mask,
                        uint32_t want, uint32_t timeout_ms) {
    uint32_t hz = pit_hz();
    if (hz == 0) hz = 100;
    uint64_t ticks = ((uint64_t)timeout_ms * hz + 999u) / 1000u;
    if (ticks == 0) ticks = 1;
    uint64_t deadline = pit_ticks() + ticks;
    for (;;) {
        uint32_t v = *(volatile uint32_t *)reg;
        if ((v & mask) == want) return 0;
        if (pit_ticks() >= deadline) return -1;
        __asm__ volatile ("pause");
    }
}

/* ---- PRP list construction ------------------------------------ */

/* Convert a kernel virtual buffer + length into PRP1/PRP2. Walks page
 * by page using vmm_translate, so heap-allocated buffers (which can
 * span page boundaries with arbitrary phys layout) Just Work. The
 * controller's prp_list scratch page is reused for every command --
 * safe because we only ever have one outstanding I/O. */
static int build_prp(struct nvme_controller *c, void *buf, uint32_t bytes,
                     uint64_t *out_prp1, uint64_t *out_prp2) {
    uint64_t v        = (uint64_t)buf;
    uint64_t page_off = v & (PAGE_SIZE - 1);
    uint32_t in_first = (uint32_t)(PAGE_SIZE - page_off);

    *out_prp1 = vmm_translate(v);
    if (*out_prp1 == 0) return -1;

    if (bytes <= in_first) {
        *out_prp2 = 0;
        return 0;
    }

    uint32_t after = bytes - in_first;
    uint64_t v2    = v + in_first;          /* page-aligned */

    if (after <= PAGE_SIZE) {
        *out_prp2 = vmm_translate(v2);
        if (*out_prp2 == 0) return -2;
        return 0;
    }

    /* > 2 pages -- need a PRP list. The list contains one 8-byte phys
     * per remaining page. With one 4 KiB list page = 512 entries we
     * support up to 512 + 1 = 513 pages = ~2 MiB per command, which
     * comfortably bounds NVME_MAX_BYTES_PER_OP (256 KiB = 64 pages). */
    uint32_t n = 0;
    uint64_t v_iter = v2;
    while (after > 0) {
        if (n >= NVME_PRP_LIST_ENTRIES) return -3;
        uint64_t phys = vmm_translate(v_iter);
        if (phys == 0) return -4;
        c->prp_list[n++] = phys;
        v_iter += PAGE_SIZE;
        after = after > PAGE_SIZE ? after - PAGE_SIZE : 0;
    }
    *out_prp2 = c->prp_list_phys;
    return 0;
}

/* ---- core submit + poll ---------------------------------------- */

/* Submit `sqe` to `q` and busy-wait for its completion. Returns 0 on
 * success, <0 on timeout / error. The caller fills opc / nsid / CDWs
 * / PRPs; we stamp the CID, ring the SQ tail doorbell, then poll the
 * CQ for a phase-tag flip with our CID. */
static int nvme_submit_sync(struct nvme_controller *c, struct nvme_queue *q,
                            struct nvme_sqe *sqe) {
    uint16_t cid = c->next_cid++;
    sqe->cid = cid;

    q->sq[q->sq_tail] = *sqe;
    q->sq_tail = (uint16_t)((q->sq_tail + 1u) % q->depth);
    *q->sq_dbl = q->sq_tail;

    uint32_t hz = pit_hz();
    if (hz == 0) hz = 100;
    uint64_t ticks = ((uint64_t)30000u * hz + 999u) / 1000u;
    uint64_t deadline = pit_ticks() + ticks;

    for (;;) {
        struct nvme_cqe *cqe = &q->cq[q->cq_head];
        uint16_t status = cqe->status;
        if ((status & 1u) == q->cq_phase) {
            uint16_t got_cid = cqe->cid;
            uint16_t sf      = (uint16_t)(status >> 1);

            q->cq_head = (uint16_t)((q->cq_head + 1u) % q->depth);
            if (q->cq_head == 0) q->cq_phase ^= 1u;
            *q->cq_dbl = q->cq_head;

            if (got_cid != cid) {
                /* In single-outstanding-command mode this should never
                 * happen -- log loudly so we notice if it ever does. */
                kprintf("[nvme] WARN: cqe cid=%u != expected %u\n",
                        got_cid, cid);
            }
            if (sf != 0) {
                kprintf("[nvme] command failed: opc=0x%02x cid=%u sf=0x%04x\n",
                        sqe->opc, cid, sf);
                return -2;
            }
            return 0;
        }
        if (pit_ticks() >= deadline) {
            kprintf("[nvme] command timeout: opc=0x%02x cid=%u\n",
                    sqe->opc, cid);
            return -1;
        }
        __asm__ volatile ("pause");
    }
}

/* ---- read / write --------------------------------------------- */

static int nvme_io(struct nvme_namespace *ns, uint64_t lba,
                   uint32_t count, void *buf, bool is_write) {
    struct nvme_controller *c = ns->ctrl;
    uint32_t bytes = count * ns->lba_size;

    /* Build PRPs into locals first; assigning them into a packed SQE
     * field via &sqe.prp1 trips the compiler's "address of packed
     * member may be unaligned" warning even though our SQEs always
     * land on 8-byte boundaries (stack alloc + 64 B struct). */
    uint64_t prp1 = 0, prp2 = 0;
    if (build_prp(c, buf, bytes, &prp1, &prp2) != 0) {
        kprintf("[nvme] build_prp failed (buf=%p bytes=%u)\n", buf, bytes);
        return -1;
    }

    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opc   = is_write ? NVME_IO_WRITE : NVME_IO_READ;
    sqe.nsid  = ns->nsid;
    sqe.prp1  = prp1;
    sqe.prp2  = prp2;
    sqe.cdw10 = (uint32_t)(lba & 0xFFFFFFFFu);
    sqe.cdw11 = (uint32_t)(lba >> 32);
    sqe.cdw12 = (uint32_t)((count - 1u) & 0xFFFFu);   /* NLB = count-1 */

    return nvme_submit_sync(c, &c->io, &sqe);
}

static int nvme_blk_read(struct blk_dev *dev, uint64_t lba,
                         uint32_t count, void *buf) {
    struct nvme_namespace *ns = (struct nvme_namespace *)dev->priv;
    uint8_t *out = (uint8_t *)buf;
    while (count > 0) {
        uint32_t chunk = count > NVME_MAX_SECTORS_PER_OP
                             ? NVME_MAX_SECTORS_PER_OP : count;
        int rc = nvme_io(ns, lba, chunk, out, /*write*/ false);
        if (rc != 0) return rc;
        out   += chunk * ns->lba_size;
        lba   += chunk;
        count -= chunk;
    }
    return 0;
}

static int nvme_blk_write(struct blk_dev *dev, uint64_t lba,
                          uint32_t count, const void *buf) {
    struct nvme_namespace *ns = (struct nvme_namespace *)dev->priv;
    uint8_t *in = (uint8_t *)buf;        /* HBA never writes through it */
    while (count > 0) {
        uint32_t chunk = count > NVME_MAX_SECTORS_PER_OP
                             ? NVME_MAX_SECTORS_PER_OP : count;
        int rc = nvme_io(ns, lba, chunk, in, /*write*/ true);
        if (rc != 0) return rc;
        in    += chunk * ns->lba_size;
        lba   += chunk;
        count -= chunk;
    }
    return 0;
}

static const struct blk_ops g_nvme_ops = {
    .read  = nvme_blk_read,
    .write = nvme_blk_write,
};

/* ---- name formatting ----------------------------------------- */

/* Tiny printf-free formatter for "nvme<H>:n<N>". H + N are both
 * bounded (<= NVME_MAX_CONTROLLERS, <= NVME_MAX_NS_PER_CTRL), so no
 * heap or sprintf needed. */
static char *fmt_uint(char *p, uint32_t v) {
    if (v >= 100) *p++ = (char)('0' + (v / 100) % 10);
    if (v >= 10)  *p++ = (char)('0' + (v / 10) % 10);
    *p++ = (char)('0' + v % 10);
    return p;
}
static void build_drive_name(char *buf, int hba, uint32_t nsid) {
    char *p = buf;
    *p++ = 'n'; *p++ = 'v'; *p++ = 'm'; *p++ = 'e';
    p = fmt_uint(p, (uint32_t)hba);
    *p++ = ':'; *p++ = 'n';
    p = fmt_uint(p, nsid);
    *p   = '\0';
}

/* ---- controller init ----------------------------------------- */

/* Issue IDENTIFY into a 4 KiB scratch buffer (page-aligned phys means
 * PRP1 alone is enough -- the transfer is exactly one page). */
static int nvme_identify(struct nvme_controller *c, uint32_t cns,
                         uint32_t nsid, uint64_t buf_phys) {
    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opc   = NVME_ADM_IDENTIFY;
    sqe.nsid  = nsid;
    sqe.prp1  = buf_phys;
    sqe.cdw10 = cns & 0xFFu;
    return nvme_submit_sync(c, &c->admin, &sqe);
}

/* MSI handler. Called from the dyn-vector trampoline (which already
 * sent apic_eoi). We don't drain CQEs here -- nvme_submit_sync still
 * polls the phase tag and rings the head doorbell. We only bump the
 * diagnostic counter so test sweeps can prove the controller actually
 * fired its MSI. (Once the M22 step-5 scheduler arrives, this is
 * where we'd wake any task sleeping on the queue.) */
static void nvme_irq_handler(void *ctx) {
    struct nvme_controller *c = (struct nvme_controller *)ctx;
    if (!c) return;
    c->irq_count++;
}

/* CREATE I/O CQ (qid 1). PC=1 always; IEN=1 only when MSI is live so
 * a degraded poll-only boot never asks the controller to assert an
 * interrupt that nothing is listening for. */
static int nvme_create_io_cq(struct nvme_controller *c) {
    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opc   = NVME_ADM_CREATE_CQ;
    sqe.prp1  = c->io.cq_phys;
    sqe.cdw10 = ((uint32_t)(NVME_QDEPTH - 1u) << 16) | 1u;   /* QSIZE | QID=1 */
    sqe.cdw11 = NVME_CQ_PC | (c->irq_enabled ? NVME_CQ_IEN : 0u);
    /* IV=0 (admin + I/O share vector 0 in our single-MSI setup). */
    return nvme_submit_sync(c, &c->admin, &sqe);
}

/* CREATE I/O SQ (qid 1, CQID=1, PC=1). */
static int nvme_create_io_sq(struct nvme_controller *c) {
    struct nvme_sqe sqe;
    memset(&sqe, 0, sizeof(sqe));
    sqe.opc   = NVME_ADM_CREATE_SQ;
    sqe.prp1  = c->io.sq_phys;
    sqe.cdw10 = ((uint32_t)(NVME_QDEPTH - 1u) << 16) | 1u;   /* QSIZE | QID=1 */
    sqe.cdw11 = (1u << 16) | 1u;                             /* CQID=1 | PC=1 */
    return nvme_submit_sync(c, &c->admin, &sqe);
}

/* Allocate a queue's SQ + CQ + doorbells. Each queue takes one page
 * for SQ (NVME_QDEPTH * 64 = 4096 B) and one for CQ (NVME_QDEPTH * 16
 * = 1024 B; the rest of the page is wasted but the alignment is
 * cleaner than packing SQ + CQ into the same page). */
static int alloc_queue(struct nvme_controller *c, struct nvme_queue *q,
                       uint16_t qid) {
    q->qid       = qid;
    q->depth     = NVME_QDEPTH;
    q->sq_tail   = 0;
    q->cq_head   = 0;
    q->cq_phase  = 1;
    q->sq_phys   = pmm_alloc_page();
    q->cq_phys   = pmm_alloc_page();
    if (!q->sq_phys || !q->cq_phys) return -1;
    q->sq        = (struct nvme_sqe *)pmm_phys_to_virt(q->sq_phys);
    q->cq        = (struct nvme_cqe *)pmm_phys_to_virt(q->cq_phys);
    memset(q->sq, 0, PAGE_SIZE);
    memset(q->cq, 0, PAGE_SIZE);
    q->sq_dbl    = (volatile uint32_t *)(c->bar + NVME_DBL_SQ(qid, c->dstrd));
    q->cq_dbl    = (volatile uint32_t *)(c->bar + NVME_DBL_CQ(qid, c->dstrd));
    return 0;
}

/* Reset, configure, IDENTIFY, build I/O queue, register namespaces.
 * Returns the number of namespaces that successfully became blk_devs;
 * 0 means "controller is alive but has nothing usable" (e.g. all
 * namespaces report a non-512 LBA size). <0 means hard init failure. */
static int nvme_init_controller(struct nvme_controller *c) {
    /* CAP gives us the doorbell stride, page-size range, MQES, and the
     * timeout-to-ready hint. */
    uint64_t cap   = reg_r64(c->bar, NVME_CAP);
    uint32_t mqes  = (uint32_t)((cap & 0xFFFFu) + 1u);
    uint32_t dstrd = (uint32_t)((cap >> 32) & 0xFu);
    uint32_t to    = (uint32_t)((cap >> 24) & 0xFFu);   /* in 500 ms units */
    uint32_t mpsmin = (uint32_t)((cap >> 48) & 0xFu);
    uint32_t mpsmax = (uint32_t)((cap >> 52) & 0xFu);
    c->dstrd = dstrd;

    if (mpsmin > 0) {
        kprintf("[nvme] controller requires page > 4K (MPSMIN=%u) -- skipping\n",
                mpsmin);
        return -1;
    }
    if (mqes < NVME_QDEPTH - 1u) {
        kprintf("[nvme] controller MQES=%u < requested depth %u -- skipping\n",
                mqes, NVME_QDEPTH);
        return -2;
    }
    (void)mpsmax;

    /* Disable the controller (idempotent on a fresh controller, but we
     * may have inherited an enabled one from BIOS/UEFI). */
    uint32_t cc = reg_r32(c->bar, NVME_CC);
    if (cc & NVME_CC_EN) {
        reg_w32(c->bar, NVME_CC, cc & ~NVME_CC_EN);
        if (wait_until32(c->bar + NVME_CSTS, NVME_CSTS_RDY, 0,
                         to * 500u + 500u) != 0) {
            kprintf("[nvme] controller did not clear CSTS.RDY on disable\n");
            return -3;
        }
    }

    /* Allocate admin queues + the shared PRP-list scratch page. */
    if (alloc_queue(c, &c->admin, 0) != 0) {
        kprintf("[nvme] PMM out of memory for admin queues\n");
        return -4;
    }
    c->prp_list_phys = pmm_alloc_page();
    if (!c->prp_list_phys) {
        kprintf("[nvme] PMM out of memory for PRP list page\n");
        return -5;
    }
    c->prp_list = (uint64_t *)pmm_phys_to_virt(c->prp_list_phys);
    memset(c->prp_list, 0, PAGE_SIZE);

    /* Program the admin queue base addresses + sizes. AQA encodes
     * SQ_size_minus_1 (low 12 bits) and CQ_size_minus_1 (bits 27:16). */
    reg_w32(c->bar, NVME_AQA,
            (uint32_t)(((NVME_QDEPTH - 1u) << 16) | (NVME_QDEPTH - 1u)));
    reg_w64(c->bar, NVME_ASQ, c->admin.sq_phys);
    reg_w64(c->bar, NVME_ACQ, c->admin.cq_phys);

    /* Mask all interrupts at the controller while we set CC.EN; we'll
     * unmask vector 0 immediately below if MSI is available. Even if
     * MSI fails entirely INTMS stays at all-ones, matching the polled
     * behaviour we shipped through M21. */
    reg_w32(c->bar, NVME_INTMS, 0xFFFFFFFFu);

    /* Enable + configure in a single transaction (per spec init flow). */
    uint32_t cc_new = NVME_CC_EN | NVME_CC_CSS_NVM | NVME_CC_MPS_4K
                    | NVME_CC_AMS_RR | NVME_CC_IOSQES_64 | NVME_CC_IOCQES_16;
    reg_w32(c->bar, NVME_CC, cc_new);

    if (wait_until32(c->bar + NVME_CSTS, NVME_CSTS_RDY, NVME_CSTS_RDY,
                     to * 500u + 500u) != 0) {
        kprintf("[nvme] controller never became ready  CSTS=0x%08x\n",
                reg_r32(c->bar, NVME_CSTS));
        return -6;
    }

    kprintf("[nvme] controller online  CAP=0x%016lx VS=0x%08x  "
            "DSTRD=%u TO=%u s MQES=%u\n",
            cap, reg_r32(c->bar, NVME_VS), dstrd, to / 2u, mqes);

    /* IDENTIFY CONTROLLER -- learn the namespace count + serial. */
    uint64_t id_phys = pmm_alloc_page();
    if (!id_phys) return -7;
    void *id_data = pmm_phys_to_virt(id_phys);
    memset(id_data, 0, PAGE_SIZE);

    if (nvme_identify(c, NVME_CNS_CONTROLLER, 0, id_phys) != 0) {
        kprintf("[nvme] IDENTIFY CONTROLLER failed\n");
        pmm_free_page(id_phys);
        return -8;
    }

    uint32_t nn = 0;
    memcpy(&nn, (uint8_t *)id_data + 516, sizeof(nn));

    char sn[21] = {0};
    memcpy(sn, (uint8_t *)id_data + 4, 20);
    /* Trim trailing spaces (NVMe pads SN with 0x20). */
    for (int i = 19; i >= 0 && sn[i] == ' '; i--) sn[i] = '\0';

    kprintf("[nvme] IDENTIFY CTRL: NN=%u  serial='%s'\n", nn, sn);

    if (nn == 0) {
        kprintf("[nvme] no namespaces present\n");
        pmm_free_page(id_phys);
        return 0;
    }

    /* Build the I/O queue (qid 1) -- CREATE_CQ first, then CREATE_SQ
     * (the SQ refers to the CQ via CQID, so the CQ must exist first). */
    if (alloc_queue(c, &c->io, 1) != 0) {
        kprintf("[nvme] PMM out of memory for I/O queues\n");
        pmm_free_page(id_phys);
        return -9;
    }
    if (nvme_create_io_cq(c) != 0) {
        kprintf("[nvme] CREATE I/O CQ failed\n");
        pmm_free_page(id_phys);
        return -10;
    }
    if (nvme_create_io_sq(c) != 0) {
        kprintf("[nvme] CREATE I/O SQ failed\n");
        pmm_free_page(id_phys);
        return -11;
    }

    /* IDENTIFY each namespace and register the ones we can support. */
    int registered = 0;
    uint32_t to_scan = nn;
    if (to_scan > NVME_MAX_NS_PER_CTRL) {
        kprintf("[nvme] WARN: NN=%u exceeds NVME_MAX_NS_PER_CTRL=%u "
                "-- only first %u will be probed\n",
                nn, NVME_MAX_NS_PER_CTRL, NVME_MAX_NS_PER_CTRL);
        to_scan = NVME_MAX_NS_PER_CTRL;
    }
    for (uint32_t nsid = 1; nsid <= to_scan; nsid++) {
        memset(id_data, 0, PAGE_SIZE);
        if (nvme_identify(c, NVME_CNS_NAMESPACE, nsid, id_phys) != 0) {
            kprintf("[nvme] IDENTIFY NS %u failed\n", nsid);
            continue;
        }

        uint64_t nsze   = 0;
        uint8_t  flbas  = 0;
        uint8_t  nlbaf  = 0;
        memcpy(&nsze,  (uint8_t *)id_data + 0,  sizeof(nsze));
        memcpy(&nlbaf, (uint8_t *)id_data + 25, 1);
        memcpy(&flbas, (uint8_t *)id_data + 26, 1);
        if (nsze == 0) {
            kprintf("[nvme] NS %u: NSZE=0 (inactive) -- skipping\n", nsid);
            continue;
        }

        uint32_t lbaf_idx   = flbas & 0x0Fu;
        uint32_t lbaf_entry = 0;
        memcpy(&lbaf_entry,
               (uint8_t *)id_data + 128 + lbaf_idx * 4u,
               sizeof(lbaf_entry));
        uint32_t lbads    = (lbaf_entry >> 16) & 0xFFu;
        uint32_t lba_size = 1u << lbads;

        kprintf("[nvme] NS %u: NSZE=%lu lba_size=%u  "
                "(LBAF[%u]=0x%08x, NLBAF=%u)\n",
                nsid, (unsigned long)nsze, lba_size,
                lbaf_idx, lbaf_entry, nlbaf + 1u);

        if (lba_size != BLK_SECTOR_SIZE) {
            kprintf("[nvme] NS %u: lba_size=%u != %u -- skipping (not "
                    "supported in this milestone)\n",
                    nsid, lba_size, BLK_SECTOR_SIZE);
            continue;
        }
        if (g_drive_count >= NVME_MAX_DRIVES) {
            kprintf("[nvme] WARN: g_drives full -- ignoring NS %u\n", nsid);
            break;
        }

        struct nvme_drive *d = &g_drives[g_drive_count];
        memset(d, 0, sizeof(*d));
        d->ns.ctrl     = c;
        d->ns.nsid     = nsid;
        d->ns.nsze     = nsze;
        d->ns.lba_size = lba_size;
        build_drive_name(d->name, c->idx, nsid);

        d->blk.name         = d->name;
        d->blk.ops          = &g_nvme_ops;
        d->blk.sector_count = nsze;
        d->blk.priv         = &d->ns;

        kprintf("[nvme] NS %u registered as '%s'  (%lu sectors, %lu KiB)\n",
                nsid, d->name,
                (unsigned long)nsze, (unsigned long)(nsze / 2u));
        blk_register(&d->blk);
        g_drive_count++;
        registered++;
    }

    pmm_free_page(id_phys);
    return registered;
}

/* ---- PCI driver glue ----------------------------------------- */

static int nvme_pci_probe(struct pci_dev *dev) {
    if (g_ctrl_count >= NVME_MAX_CONTROLLERS) {
        kprintf("[nvme] too many controllers (%zu) -- skipping %02x:%02x.%x\n",
                g_ctrl_count, dev->bus, dev->slot, dev->fn);
        return -1;
    }

    kprintf("[nvme] probing %02x:%02x.%x  (vid:did %04x:%04x)\n",
            dev->bus, dev->slot, dev->fn, dev->vendor, dev->device);

    pci_dev_enable(dev, PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);

    void *bar0_v = pci_map_bar(dev, 0, 0);
    if (!bar0_v) {
        kprintf("[nvme] BAR0 map failed (phys=%p)\n", (void *)dev->bar[0]);
        return -2;
    }

    struct nvme_controller *c = &g_ctrls[g_ctrl_count];
    memset(c, 0, sizeof(*c));
    c->idx       = (int)g_ctrl_count;
    c->bar       = (volatile uint8_t *)bar0_v;
    c->next_cid  = 1;

    kprintf("[nvme] BAR0 phys=%p virt=%p (%lu KiB UC)\n",
            (void *)dev->bar[0], bar0_v,
            (unsigned long)(dev->bar_size[0] / 1024u));

    /* IRQ bring-up runs BEFORE nvme_init_controller because CREATE
     * I/O CQ needs to know whether to set IEN. Single-vector delivery
     * is sufficient: admin and the one I/O queue both target vector 0
     * (which is the only one we own).
     *
     * Try MSI-X first (QEMU `-device nvme` advertises only MSI-X by
     * default; most modern real NVMe SSDs likewise prefer MSI-X), then
     * fall back to legacy MSI for older controllers. On both-fail we
     * stay polled -- submit_sync's busy-wait works either way. */
    uint8_t vec = irq_alloc_vector(nvme_irq_handler, c);
    if (vec == 0) {
        kprintf("[nvme] %02x:%02x.%x: no IDT vectors free -- staying polled\n",
                dev->bus, dev->slot, dev->fn);
    } else if (pci_msix_enable(dev, vec, (uint8_t)apic_read_id(), 1u)) {
        c->irq_vector  = vec;
        c->irq_enabled = true;
    } else if (pci_msi_enable(dev, vec, (uint8_t)apic_read_id())) {
        c->irq_vector  = vec;
        c->irq_enabled = true;
    } else {
        kprintf("[nvme] %02x:%02x.%x: no MSI / MSI-X cap -- staying "
                "polled (vec 0x%02x is now idle)\n",
                dev->bus, dev->slot, dev->fn, (unsigned)vec);
    }

    int registered = nvme_init_controller(c);
    if (registered <= 0) {
        kprintf("[nvme] %02x:%02x.%x: no namespaces registered -- declining\n",
                dev->bus, dev->slot, dev->fn);
        return -3;
    }

    /* Controller is up and the I/O CQ was built with IEN as configured.
     * Unmask vector 0 in INTMC (W1C of the INTMS bit) so the controller
     * can actually deliver completions. INTMS is the per-vector mask
     * register, NVMe-spec section 3.1.6 / 3.1.7. */
    if (c->irq_enabled) {
        reg_w32(c->bar, NVME_INTMC, 0x1u);
        kprintf("[nvme] %02x:%02x.%x: IRQ live on vec 0x%02x "
                "(INTMC bit 0 cleared)\n",
                dev->bus, dev->slot, dev->fn, (unsigned)c->irq_vector);
    }

    g_ctrl_count++;
    dev->driver_data = bar0_v;
    return 0;
}

static const struct pci_match g_nvme_matches[] = {
    /* class 0x01 / subclass 0x08 = NVMe. We accept any prog_if so
     * vendor-specific encodings (0x02 = NVM Express I/O Cmd Set, 0x03
     * = NVMe Administrative, ...) all bind. */
    { PCI_ANY_ID, PCI_ANY_ID,
      PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_NVME, PCI_ANY_CLASS },
    PCI_MATCH_END,
};

static struct pci_driver g_nvme_driver = {
    .name    = "blk_nvme",
    .matches = g_nvme_matches,
    .probe   = nvme_pci_probe,
    .remove  = 0,
};

void blk_nvme_register(void) {
    pci_register_driver(&g_nvme_driver);
}
