/* blk_ahci.c -- AHCI 1.0 SATA driver (MSI-driven completion, polled fallback).
 *
 * Bound through the milestone-21 PCI driver registry. Matches any
 * mass-storage SATA controller (class 0x01, subclass 0x06). On QEMU's
 * q35 machine that's the ICH9 SATA HBA at 00:1F.2 (8086:2922). On real
 * hardware it's whatever AHCI HBA the chipset exposes (Intel PCH /
 * AMD FCH SATA in AHCI mode, Marvell, ASMedia, ...).
 *
 * Architecture
 * ------------
 *
 *   1. BAR5 (ABAR) is mapped via pci_map_bar(NOCACHE). The first
 *      0x100 bytes are the generic host control regs; per-port reg
 *      blocks start at 0x100 + portIdx*0x80.
 *
 *   2. We do a soft HBA reset (GHC.HR), set GHC.AE = 1, and respect
 *      BIOS/OS handoff (CAP2.BOH) -- no-op under QEMU but required
 *      on real boards whose firmware pre-touched the HBA.
 *
 *   3. For each port the PI bitmask says is implemented:
 *        - Stop CMD.ST + CMD.FRE, wait for CR + FR to clear.
 *        - pmm_alloc_page() three pages for command list (1 KiB at
 *          offset 0), FIS receive area (256 B at offset 0), and
 *          command table 0 (128 B header + 64 PRDT entries at offset
 *          0). 4 KiB pages over-cover all three -- simple + correct.
 *        - Set CLB/CLBU + FB/FBU, enable FRE + spin-up, poll
 *          SSTS.DET = 3 (device present + PHY up).
 *        - Clear SERR + IS, wait for BSY|DRQ in TFD, set CMD.ST.
 *        - Issue ATA IDENTIFY DEVICE via slot 0, read LBA48 max-
 *          sector count from words 100..103.
 *        - blk_register() with name "ahciN:pK".
 *
 *   4. read/write ops use slot 0 only (one outstanding command at a
 *      time, just like blk_ata.c). They build a Host-to-Device
 *      Register FIS (READ/WRITE DMA EXT, LBA48), populate the PRDT
 *      from the caller's buffer (split at 4 KiB boundaries via
 *      vmm_translate so heap-allocated buffers Just Work), set PxCI
 *      bit 0, and wait for completion.
 *
 *   5. Completion path (M22 step 3a):
 *        - At probe time we allocate an IDT vector via
 *          irq_alloc_vector() and try pci_msi_enable(). If that
 *          succeeds we set GHC.IE + per-port PxIE so the HBA fires
 *          the MSI on every D2H Register FIS / completion.
 *        - The ISR reads IS, walks each port's PxIS (write-1-to-clear
 *          on both), and sets a per-port done_slots bit + sticky
 *          err_is for the wait path.
 *        - ahci_wait_completion() spins on (done_slots | PxCI cleared
 *          | error in PxIS). This means polling still works as a
 *          guaranteed fallback if the IRQ never fires (e.g. the device
 *          has no MSI cap or the legacy INTx pin would be needed and
 *          we punt) -- the wait still exits, just by reading the same
 *          register the polled cut used to.
 *
 *   6. 64-bit DMA is always used (CAP.S64A is set on every modern
 *      HBA including QEMU's). pmm_alloc_page returns 4 KiB-aligned
 *      phys we hand to the HBA verbatim.
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

/* ---- generic host control register offsets (in ABAR) -------------- */

#define AHCI_CAP            0x00
#define AHCI_GHC            0x04
#define AHCI_IS             0x08
#define AHCI_PI             0x0C
#define AHCI_VS             0x10
#define AHCI_CAP2           0x24
#define AHCI_BOHC           0x28

#define AHCI_GHC_HR         (1u << 0)
#define AHCI_GHC_IE         (1u << 1)
#define AHCI_GHC_AE         (1u << 31)

#define AHCI_CAP2_BOH       (1u << 0)
#define AHCI_BOHC_BOS       (1u << 0)
#define AHCI_BOHC_OOS       (1u << 1)
#define AHCI_BOHC_BB        (1u << 4)

/* ---- per-port register offsets ----------------------------------- */

#define AHCI_PORT_OFF(idx)  (0x100u + (uint32_t)(idx) * 0x80u)

#define AHCI_PxCLB          0x00
#define AHCI_PxCLBU         0x04
#define AHCI_PxFB           0x08
#define AHCI_PxFBU          0x0C
#define AHCI_PxIS           0x10
#define AHCI_PxIE           0x14
#define AHCI_PxCMD          0x18
#define AHCI_PxTFD          0x20
#define AHCI_PxSIG          0x24
#define AHCI_PxSSTS         0x28
#define AHCI_PxSCTL         0x2C
#define AHCI_PxSERR         0x30
#define AHCI_PxSACT         0x34
#define AHCI_PxCI           0x38

#define AHCI_PxCMD_ST       (1u << 0)
#define AHCI_PxCMD_SUD      (1u << 1)
#define AHCI_PxCMD_POD      (1u << 2)
#define AHCI_PxCMD_FRE      (1u << 4)
#define AHCI_PxCMD_FR       (1u << 14)
#define AHCI_PxCMD_CR       (1u << 15)

#define AHCI_TFD_BSY        (1u << 7)
#define AHCI_TFD_DRQ        (1u << 3)
#define AHCI_TFD_ERR        (1u << 0)

#define AHCI_PxIS_TFES      (1u << 30)
#define AHCI_PxIS_HBFS      (1u << 29)
#define AHCI_PxIS_HBDS      (1u << 28)
#define AHCI_PxIS_IFS       (1u << 27)

#define AHCI_PxIS_ERR_MASK  (AHCI_PxIS_TFES | AHCI_PxIS_HBFS | \
                             AHCI_PxIS_HBDS | AHCI_PxIS_IFS)

/* PxIS bits worth treating as "command completion" for slot 0. We
 * issue every command via the H2D Register FIS path, so the device
 * always replies with a D2H Register FIS (DHRS = bit 0). PSS (1) and
 * DSS (2) cover PIO Setup / DMA Setup that some commands also raise.
 * We OR them all together because we only ever have one slot in
 * flight, so any of these = "your command finished". */
#define AHCI_PxIS_DHRS      (1u << 0)
#define AHCI_PxIS_PSS       (1u << 1)
#define AHCI_PxIS_DSS       (1u << 2)
#define AHCI_PxIS_DONE_MASK (AHCI_PxIS_DHRS | AHCI_PxIS_PSS | AHCI_PxIS_DSS)

/* Per-port IRQ enable bits we want active. Match the completion +
 * error mask so we hear about both cases. */
#define AHCI_PxIE_BITS      (AHCI_PxIS_DONE_MASK | AHCI_PxIS_ERR_MASK)

#define AHCI_DET_PRESENT    3u    /* SSTS.DET = device + PHY up */

#define AHCI_SIG_SATA       0x00000101u

/* ---- ATA commands ------------------------------------------------- */

#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_READ_DMA_EXT    0x25
#define ATA_CMD_WRITE_DMA_EXT   0x35

/* ---- driver capacities ------------------------------------------- */

#define AHCI_SLOT               0      /* polled = one slot in use   */
#define AHCI_PRDT_ENTRIES       64
#define AHCI_BYTES_PER_OP       (AHCI_PRDT_ENTRIES * PAGE_SIZE)
#define AHCI_SECTORS_PER_OP     (AHCI_BYTES_PER_OP / BLK_SECTOR_SIZE)
#define AHCI_MAX_DRIVES         8

/* ---- on-the-wire HBA structures --------------------------------- */

struct __attribute__((packed)) ahci_cmd_header {
    /* dw0 packed: bits 0..4 = CFL (FIS length in dwords),
     * bit 5 = A (ATAPI), bit 6 = W (write), bit 7 = P (prefetch),
     * bit 8 = R (reset), bit 9 = B (BIST),
     * bit 10 = C (clear busy on R_OK), bit 11 = rsvd,
     * bits 12..15 = PMP, bits 16..31 = PRDTL. */
    uint16_t flags;
    uint16_t prdtl;
    uint32_t prdbc;            /* PRD byte count (HW writes) */
    uint32_t ctba_lo;
    uint32_t ctba_hi;
    uint32_t reserved[4];
};

struct __attribute__((packed)) ahci_prdt_entry {
    uint32_t dba_lo;
    uint32_t dba_hi;
    uint32_t reserved;
    /* DBC bits 21:0 = byte_count - 1 (must be even),
     * bit 31 = I (interrupt on completion). */
    uint32_t dbc_i;
};

struct __attribute__((packed)) ahci_cmd_table {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  reserved[48];
    struct ahci_prdt_entry prdt[AHCI_PRDT_ENTRIES];
};

/* Sanity: the AHCI spec mandates these layouts byte-for-byte. */
_Static_assert(sizeof(struct ahci_cmd_header) == 32,
               "ahci_cmd_header must be 32 bytes");
_Static_assert(sizeof(struct ahci_prdt_entry) == 16,
               "ahci_prdt_entry must be 16 bytes");
_Static_assert(sizeof(struct ahci_cmd_table)
               == 128 + AHCI_PRDT_ENTRIES * 16,
               "ahci_cmd_table size mismatch");

/* ---- per-port driver state -------------------------------------- */

struct ahci_hba;        /* fwd, for back-pointer below */

struct ahci_port {
    int                       idx;
    volatile uint8_t         *regs;          /* ABAR + 0x100 + idx*0x80 */
    uint64_t                  cl_phys;
    struct ahci_cmd_header   *cl;            /* 32-entry command list  */
    uint64_t                  fis_phys;
    void                     *fis;           /* 256-byte FIS RX area   */
    uint64_t                  ct_phys;
    struct ahci_cmd_table    *ct;            /* command table for slot 0 */
    uint64_t                  sectors;
    /* Set by the ISR; cleared at the start of every issue. Bit s = 1
     * means slot s completed (we currently only use slot 0). 'volatile'
     * is required because the wait loop reads it without taking any
     * lock -- the ISR is the writer. */
    volatile uint32_t         done_slots;
    /* Sticky union of every PxIS error bit observed since the last
     * issue. ahci_issue checks this after wait returns so even an
     * error that arrived via the IRQ path (PxIS already cleared by the
     * ISR) is still visible to the failure log. */
    volatile uint32_t         err_is;
    struct ahci_hba          *hba;           /* back-pointer for ISR */
};

/* Per-disk wrapper. blk_dev.priv -> &ahci_drive::port. */
struct ahci_drive {
    struct ahci_port  port;
    char              name[32];
    struct blk_dev    blk;
    bool              live;
};

/* Per-HBA state -- one of these per bound PCI function. The ISR uses
 * it to walk only that HBA's ports, so two SATA controllers in the
 * same machine don't trip over each other. */
struct ahci_hba {
    volatile uint8_t *abar;
    uint8_t           irq_vector;       /* allocated via irq_alloc_vector */
    bool              irq_enabled;      /* MSI succeeded -> IRQs live */
    int               drive_first;      /* index in g_drives[] */
    int               drive_count;
    /* IRQ counter for diagnostics. Read by the shell + the regression
     * sweep to assert "yes, the IRQ actually fired". */
    volatile uint64_t irq_count;
};

static struct ahci_drive g_drives[AHCI_MAX_DRIVES];
static size_t            g_drive_count;
static int               g_hba_index;        /* one per bound HBA      */

#define AHCI_MAX_HBAS  4
static struct ahci_hba   g_hbas[AHCI_MAX_HBAS];
static int               g_hba_used;

/* ---- MMIO helpers ----------------------------------------------- */

static inline uint32_t hba_r32(volatile uint8_t *abar, uint32_t off) {
    return *(volatile uint32_t *)(abar + off);
}
static inline void hba_w32(volatile uint8_t *abar, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(abar + off) = v;
}
static inline uint32_t prt_r32(volatile uint8_t *prt, uint32_t off) {
    return *(volatile uint32_t *)(prt + off);
}
static inline void prt_w32(volatile uint8_t *prt, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(prt + off) = v;
}

/* ---- timing helpers --------------------------------------------- */

/* Spin until (*reg & mask) == want, or `timeout_ms` elapses on the
 * PIT clock. Returns 0 on success, -1 on timeout. We use the PIT
 * (rather than a fixed iteration count) because cpu speed varies
 * wildly and "wait 1 second" should mean wall-clock seconds. */
static int wait_until(volatile uint8_t *reg, uint32_t mask,
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

/* ---- IRQ handler ------------------------------------------------ */

/* Called from the irq_alloc_vector trampoline; ctx is the ahci_hba *
 * we registered at probe time. apic_eoi() is sent for us by the
 * trampoline. We only do non-blocking work here: snapshot IS, walk
 * each affected port, capture PxIS bits, and W1C both. */
static void ahci_irq_handler(void *ctx) {
    struct ahci_hba *h = (struct ahci_hba *)ctx;
    if (!h || !h->abar) return;

    uint32_t is = hba_r32(h->abar, AHCI_IS);
    if (is == 0) return;     /* shared MSI cap with another device */

    h->irq_count++;

    /* Walk every port owned by this HBA. We could short-circuit by
     * iterating only set bits in `is`, but with at most 8 drives in
     * g_drives the linear scan is cheaper than a __builtin_ffs loop
     * and easier to audit. */
    for (int di = h->drive_first;
         di < h->drive_first + h->drive_count; di++) {
        struct ahci_port *p = &g_drives[di].port;
        if ((is & (1u << p->idx)) == 0) continue;

        uint32_t pis = prt_r32(p->regs, AHCI_PxIS);
        prt_w32(p->regs, AHCI_PxIS, pis);                  /* W1C */

        if (pis & AHCI_PxIS_DONE_MASK) p->done_slots |= 1u;
        if (pis & AHCI_PxIS_ERR_MASK)  {
            p->err_is    |= pis;
            p->done_slots |= 1u;       /* errored, but still wakes */
        }
    }

    hba_w32(h->abar, AHCI_IS, is);                          /* W1C */
}

/* Wait for slot 0 on `p` to complete. Strategy:
 *
 *   - Primary signal: PxCI bit 0 clears (the canonical "command
 *     finished" indicator; works whether or not IRQs are live).
 *   - Secondary signal: p->done_slots gets a bit 0 from the ISR.
 *
 * Whichever signal fires first wins. This means the wait STILL works
 * correctly if MSI was unavailable (irq_enabled = false) or if the
 * IRQ delivery is somehow lost en route.
 *
 * Returns 0 on success, -1 on timeout. We do NOT translate "error
 * recorded by the ISR" into a non-zero return here; the caller
 * (ahci_issue) checks PxIS / err_is itself so the error reporting
 * stays in one place. */
static int ahci_wait_completion(struct ahci_port *p, uint32_t timeout_ms) {
    uint32_t hz = pit_hz();
    if (hz == 0) hz = 100;
    uint64_t ticks = ((uint64_t)timeout_ms * hz + 999u) / 1000u;
    if (ticks == 0) ticks = 1;
    uint64_t deadline = pit_ticks() + ticks;
    for (;;) {
        if (p->done_slots & 1u) return 0;
        if ((prt_r32(p->regs, AHCI_PxCI) & (1u << AHCI_SLOT)) == 0) {
            /* The ISR may not have run yet (or MSI is disabled and
             * never will). Either way the command finished. */
            p->done_slots |= 1u;
            return 0;
        }
        if (pit_ticks() >= deadline) return -1;
        __asm__ volatile ("pause");
    }
}

/* ---- command-table / FIS construction -------------------------- */

/* Build the PRDT from a kernel virtual buffer. Each entry covers the
 * bytes from the current position up to the end of the page that
 * contains it -- so a buffer that crosses page boundaries (very
 * common with kmalloc) gets one PRDT entry per page. vmm_translate
 * gives us the phys for each page; we hand those to the HBA. */
static int build_prdt(struct ahci_cmd_table *ct,
                      void *buf, uint32_t total_bytes) {
    uint8_t *bytes = (uint8_t *)buf;
    uint32_t remaining = total_bytes;
    int n = 0;
    while (remaining > 0) {
        if (n >= AHCI_PRDT_ENTRIES) return -1;
        uint64_t v        = (uint64_t)bytes;
        uint64_t page_off = v & (PAGE_SIZE - 1);
        uint32_t in_page  = (uint32_t)(PAGE_SIZE - page_off);
        uint32_t chunk    = remaining < in_page ? remaining : in_page;
        uint64_t phys     = vmm_translate(v);
        if (phys == 0) return -2;
        ct->prdt[n].dba_lo    = (uint32_t)(phys & 0xFFFFFFFFu);
        ct->prdt[n].dba_hi    = (uint32_t)(phys >> 32);
        ct->prdt[n].reserved  = 0;
        ct->prdt[n].dbc_i     = (chunk - 1u) & 0x3FFFFFu;  /* I bit = 0 */
        bytes     += chunk;
        remaining -= chunk;
        n++;
    }
    return n;
}

/* Build a Host-to-Device Register FIS for an LBA48 command. */
static void build_h2d_fis(uint8_t *cfis, uint8_t cmd,
                          uint64_t lba, uint16_t count) {
    memset(cfis, 0, 20);
    cfis[0]  = 0x27;                /* FIS type: H2D Register */
    cfis[1]  = 0x80;                /* C = command            */
    cfis[2]  = cmd;
    cfis[3]  = 0;                   /* features[7:0]          */

    cfis[4]  = (uint8_t)(lba       & 0xFF);
    cfis[5]  = (uint8_t)(lba >>  8 & 0xFF);
    cfis[6]  = (uint8_t)(lba >> 16 & 0xFF);
    cfis[7]  = 0x40;                /* device: LBA mode bit   */

    cfis[8]  = (uint8_t)(lba >> 24 & 0xFF);
    cfis[9]  = (uint8_t)(lba >> 32 & 0xFF);
    cfis[10] = (uint8_t)(lba >> 40 & 0xFF);
    cfis[11] = 0;                   /* features[15:8]         */

    cfis[12] = (uint8_t)(count & 0xFF);
    cfis[13] = (uint8_t)((count >> 8) & 0xFF);
    cfis[14] = 0;
    cfis[15] = 0;                   /* control                */
}

/* ---- core single-command issue path ----------------------------- */

/* Issue one command on slot 0 and wait for completion. `is_write` sets
 * the W bit in the command header so the HBA knows to DMA host->disk.
 * Returns 0 on success, negative errno-ish on failure. */
static int ahci_issue(struct ahci_port *p, int prdt_count, bool is_write) {
    /* Wait for any previous command to drain (defensive). */
    if (wait_until(p->regs + AHCI_PxTFD,
                   AHCI_TFD_BSY | AHCI_TFD_DRQ, 0, 1000) != 0) {
        kprintf("[ahci] port %d: BSY|DRQ stuck before issue\n", p->idx);
        return -1;
    }

    /* Fill out the command header for slot 0. CFL = 5 (H2D Register
     * FIS = 20 bytes / 4 = 5 dwords). */
    uint16_t flags = 5u;
    if (is_write) flags |= (1u << 6);
    p->cl[AHCI_SLOT].flags    = flags;
    p->cl[AHCI_SLOT].prdtl    = (uint16_t)prdt_count;
    p->cl[AHCI_SLOT].prdbc    = 0;
    p->cl[AHCI_SLOT].ctba_lo  = (uint32_t)(p->ct_phys & 0xFFFFFFFFu);
    p->cl[AHCI_SLOT].ctba_hi  = (uint32_t)(p->ct_phys >> 32);

    /* Reset the wait state. We do this BEFORE clearing the hardware
     * status so a leftover IRQ from a prior command can't sneak in
     * between the two writes and falsely set done_slots. */
    p->done_slots = 0;
    p->err_is     = 0;

    /* Clear any stale port interrupt status (write-1-to-clear). */
    prt_w32(p->regs, AHCI_PxIS, 0xFFFFFFFFu);
    prt_w32(p->regs, AHCI_PxSERR, prt_r32(p->regs, AHCI_PxSERR));

    /* Hand the command to the HBA. */
    prt_w32(p->regs, AHCI_PxCI, 1u << AHCI_SLOT);

    /* Wait for completion. The wait helper checks BOTH the IRQ-set
     * done flag AND PxCI clearing, so it works whether MSI is enabled
     * or not. 30s gives us plenty of slack even on slow spinning rust;
     * QEMU clears it within microseconds. */
    if (ahci_wait_completion(p, 30000) != 0) {
        uint32_t is  = prt_r32(p->regs, AHCI_PxIS);
        uint32_t tfd = prt_r32(p->regs, AHCI_PxTFD);
        kprintf("[ahci] port %d: CI stuck (IS=0x%08x TFD=0x%08x)\n",
                p->idx, is, tfd);
        return -2;
    }

    /* Check for errors. TFES = task file error, others = bus / data /
     * interface errors -- any of which means the command failed. The
     * error status may have arrived two ways: (a) the ISR observed it
     * and OR'd into err_is, or (b) we polled to completion and the
     * bits are still in PxIS. Combine both sources. */
    uint32_t is = (uint32_t)p->err_is | prt_r32(p->regs, AHCI_PxIS);
    if (is & AHCI_PxIS_ERR_MASK) {
        uint32_t tfd = prt_r32(p->regs, AHCI_PxTFD);
        kprintf("[ahci] port %d: command error IS=0x%08x TFD=0x%08x\n",
                p->idx, is, tfd);
        prt_w32(p->regs, AHCI_PxIS, is);
        return -3;
    }
    return 0;
}

/* ---- IDENTIFY ---------------------------------------------------- */

/* Ask the drive for its 256-word identification block and return the
 * LBA48 max-sector count from words 100..103. Caller's buffer must be
 * 512 bytes and 2-byte aligned (we don't otherwise care what backs
 * it -- vmm_translate handles HHDM and heap alike). */
static int ahci_identify(struct ahci_port *p, uint64_t *out_sectors) {
    /* Per-page scratch buffer (so the caller doesn't have to provide
     * one). pmm_alloc_page is the cleanest source -- we know the phys
     * address inverts cleanly via vmm_translate. */
    uint64_t scratch_phys = pmm_alloc_page();
    if (scratch_phys == 0) return -1;
    uint16_t *id = (uint16_t *)pmm_phys_to_virt(scratch_phys);
    memset(id, 0, BLK_SECTOR_SIZE);

    int n = build_prdt(p->ct, id, BLK_SECTOR_SIZE);
    if (n <= 0) {
        pmm_free_page(scratch_phys);
        return -2;
    }
    build_h2d_fis(p->ct->cfis, ATA_CMD_IDENTIFY, 0, 1);

    int rc = ahci_issue(p, n, /*is_write*/ false);
    if (rc != 0) {
        pmm_free_page(scratch_phys);
        return rc;
    }

    /* Words 100..103 = 64-bit max LBA. Word 83 bit 10 says LBA48 is
     * supported; we trust QEMU + every modern SATA disk supports it.
     * Fall back to words 60..61 (LBA28 max) if 100..103 are zero. */
    uint64_t lba48 = ((uint64_t)id[100])       |
                     ((uint64_t)id[101] << 16) |
                     ((uint64_t)id[102] << 32) |
                     ((uint64_t)id[103] << 48);
    if (lba48 == 0) {
        uint32_t lba28 = ((uint32_t)id[61] << 16) | id[60];
        lba48 = lba28;
    }
    *out_sectors = lba48;

    pmm_free_page(scratch_phys);
    return 0;
}

/* ---- read / write ----------------------------------------------- */

static int ahci_one_xfer(struct ahci_port *p, uint64_t lba,
                         uint16_t count, void *buf, bool is_write) {
    if (count == 0) return 0;
    int n = build_prdt(p->ct, buf, (uint32_t)count * BLK_SECTOR_SIZE);
    if (n <= 0) return -1;
    build_h2d_fis(p->ct->cfis,
                  is_write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT,
                  lba, count);
    return ahci_issue(p, n, is_write);
}

static int ahci_blk_read(struct blk_dev *dev, uint64_t lba,
                         uint32_t count, void *buf) {
    struct ahci_port *p = (struct ahci_port *)dev->priv;
    uint8_t *out = (uint8_t *)buf;
    while (count > 0) {
        uint32_t chunk = count > AHCI_SECTORS_PER_OP
                             ? AHCI_SECTORS_PER_OP : count;
        int rc = ahci_one_xfer(p, lba, (uint16_t)chunk, out, /*write*/ false);
        if (rc != 0) return rc;
        out   += chunk * BLK_SECTOR_SIZE;
        lba   += chunk;
        count -= chunk;
    }
    return 0;
}

static int ahci_blk_write(struct blk_dev *dev, uint64_t lba,
                          uint32_t count, const void *buf) {
    struct ahci_port *p = (struct ahci_port *)dev->priv;
    uint8_t *in = (uint8_t *)buf;        /* HBA never writes through it */
    while (count > 0) {
        uint32_t chunk = count > AHCI_SECTORS_PER_OP
                             ? AHCI_SECTORS_PER_OP : count;
        int rc = ahci_one_xfer(p, lba, (uint16_t)chunk, in, /*write*/ true);
        if (rc != 0) return rc;
        in    += chunk * BLK_SECTOR_SIZE;
        lba   += chunk;
        count -= chunk;
    }
    return 0;
}

static const struct blk_ops g_ahci_ops = {
    .read  = ahci_blk_read,
    .write = ahci_blk_write,
};

/* ---- per-port + per-HBA init ----------------------------------- */

/* Stop ST + FRE and wait for CR + FR to clear. Required before
 * reprogramming CLB / FB or doing port-level reset. */
static int port_stop(volatile uint8_t *prt) {
    uint32_t cmd = prt_r32(prt, AHCI_PxCMD);
    cmd &= ~(AHCI_PxCMD_ST | AHCI_PxCMD_FRE);
    prt_w32(prt, AHCI_PxCMD, cmd);
    if (wait_until(prt + AHCI_PxCMD, AHCI_PxCMD_CR, 0, 500) != 0) return -1;
    if (wait_until(prt + AHCI_PxCMD, AHCI_PxCMD_FR, 0, 500) != 0) return -2;
    return 0;
}

static void port_start(volatile uint8_t *prt) {
    /* Wait for BSY|DRQ to clear before flipping ST -- spec § 10.3.1. */
    (void)wait_until(prt + AHCI_PxTFD,
                     AHCI_TFD_BSY | AHCI_TFD_DRQ, 0, 1000);
    uint32_t cmd = prt_r32(prt, AHCI_PxCMD);
    cmd |= AHCI_PxCMD_FRE;
    prt_w32(prt, AHCI_PxCMD, cmd);
    cmd |= AHCI_PxCMD_ST;
    prt_w32(prt, AHCI_PxCMD, cmd);
}

/* Allocate command list, FIS RX area, and command table 0 from the
 * PMM. Each fits comfortably inside one 4 KiB page, and the alignment
 * of pmm_alloc_page (4 KiB) satisfies all three (1 KiB / 256 B / 128 B
 * are all factors of 4 KiB). On OOM we leak whatever pages we already
 * grabbed -- the system is in a fairly hopeless state by then anyway. */
static int port_alloc(struct ahci_port *p) {
    p->cl_phys = pmm_alloc_page();
    if (!p->cl_phys) return -1;
    p->cl = (struct ahci_cmd_header *)pmm_phys_to_virt(p->cl_phys);
    memset(p->cl, 0, PAGE_SIZE);

    p->fis_phys = pmm_alloc_page();
    if (!p->fis_phys) return -2;
    p->fis = pmm_phys_to_virt(p->fis_phys);
    memset(p->fis, 0, PAGE_SIZE);

    p->ct_phys = pmm_alloc_page();
    if (!p->ct_phys) return -3;
    p->ct = (struct ahci_cmd_table *)pmm_phys_to_virt(p->ct_phys);
    memset(p->ct, 0, PAGE_SIZE);
    return 0;
}

/* Bring up one port. Returns 0 on success (drive present + IDENTIFY
 * worked), <0 if no drive is attached or init failed. */
static int port_init(struct ahci_port *p) {
    if (port_stop(p->regs) != 0) {
        kprintf("[ahci] port %d: stop failed\n", p->idx);
        return -1;
    }
    if (port_alloc(p) != 0) {
        kprintf("[ahci] port %d: PMM out of memory\n", p->idx);
        return -2;
    }

    prt_w32(p->regs, AHCI_PxCLB,  (uint32_t)(p->cl_phys & 0xFFFFFFFFu));
    prt_w32(p->regs, AHCI_PxCLBU, (uint32_t)(p->cl_phys >> 32));
    prt_w32(p->regs, AHCI_PxFB,   (uint32_t)(p->fis_phys & 0xFFFFFFFFu));
    prt_w32(p->regs, AHCI_PxFBU,  (uint32_t)(p->fis_phys >> 32));

    /* Reset wait state and clear any stale IS / SERR bits the BIOS or
     * a previous OS may have left lying around. CRITICALLY, leave PxIE
     * MASKED for now -- empty ports must never be unmasked or they'll
     * cause an MSI storm via PxIS bits like CPDS / PCS that fire on
     * PHY events the HBA generates spontaneously.  We re-enable PxIE
     * just before the success return below, once we've confirmed there
     * really is a SATA drive on this port. */
    p->done_slots = 0;
    p->err_is     = 0;
    prt_w32(p->regs, AHCI_PxIE, 0);
    prt_w32(p->regs, AHCI_PxIS, 0xFFFFFFFFu);
    prt_w32(p->regs, AHCI_PxSERR, 0xFFFFFFFFu);

    /* Power on + spin up. POD/SUD are no-ops on devices without the
     * feature (mechanical presence detect / staggered spinup); always
     * setting them is harmless and correct. */
    uint32_t cmd = prt_r32(p->regs, AHCI_PxCMD);
    cmd |= AHCI_PxCMD_POD | AHCI_PxCMD_SUD;
    prt_w32(p->regs, AHCI_PxCMD, cmd);

    /* Enable FIS receive BEFORE spinning up the command engine -- the
     * D2H Register FIS that completes IDENTIFY must land in our FB. */
    cmd |= AHCI_PxCMD_FRE;
    prt_w32(p->regs, AHCI_PxCMD, cmd);

    /* Wait for the device to come up. SSTS.DET = 3 means "device
     * detected and PHY communication established". Some real SATA
     * disks take ~100 ms to negotiate; QEMU clears in <1 ms. 1 second
     * is a generous bound. */
    int dev_seen = 0;
    for (int ms = 0; ms < 1000; ms += 10) {
        uint32_t ssts = prt_r32(p->regs, AHCI_PxSSTS);
        if ((ssts & 0xF) == AHCI_DET_PRESENT) { dev_seen = 1; break; }
        pit_sleep_ms(10);
    }
    if (!dev_seen) {
        kprintf("[ahci] port %d: no device (SSTS.DET = 0x%x)\n",
                p->idx, prt_r32(p->regs, AHCI_PxSSTS) & 0xFu);
        return -3;
    }

    /* Verify it's a SATA disk (not ATAPI / port multiplier / enclosure
     * services). We could grow ATAPI support later as a separate driver;
     * for now we punt. */
    uint32_t sig = prt_r32(p->regs, AHCI_PxSIG);
    if (sig != AHCI_SIG_SATA) {
        kprintf("[ahci] port %d: non-SATA signature 0x%08x -- skipping\n",
                p->idx, sig);
        return -4;
    }

    /* Clear SERR after PHY is up (it accumulates errors during init
     * that aren't real failures). */
    prt_w32(p->regs, AHCI_PxSERR, 0xFFFFFFFFu);

    /* Wait for BSY|DRQ to settle, then start the command engine. */
    if (wait_until(p->regs + AHCI_PxTFD,
                   AHCI_TFD_BSY | AHCI_TFD_DRQ, 0, 5000) != 0) {
        kprintf("[ahci] port %d: BSY|DRQ never cleared\n", p->idx);
        return -5;
    }
    port_start(p->regs);

    if (ahci_identify(p, &p->sectors) != 0) {
        kprintf("[ahci] port %d: IDENTIFY failed\n", p->idx);
        return -6;
    }
    if (p->sectors == 0) {
        kprintf("[ahci] port %d: IDENTIFY reports zero sectors\n", p->idx);
        return -7;
    }

    /* Drive is healthy. Unmask the per-port IRQ sources we care about
     * (DHRS / PSS / DSS for command completion + the four error bits)
     * AFTER clearing any latched bits IDENTIFY may have set. The HBA
     * global IS register will only assert for ports whose PxIE has
     * matching bits set, so empty ports stay quiet. */
    prt_w32(p->regs, AHCI_PxIS, 0xFFFFFFFFu);
    prt_w32(p->regs, AHCI_PxIE, AHCI_PxIE_BITS);
    return 0;
}

/* ---- HBA init --------------------------------------------------- */

/* If the firmware claims ownership (BOH/CAP2.BOH = 1, BOHC.BOS = 1),
 * politely take it. Required for boot on real Intel boards -- on QEMU
 * CAP2.BOH is zero and this is a no-op. */
static void bios_handoff(volatile uint8_t *abar) {
    uint32_t cap2 = hba_r32(abar, AHCI_CAP2);
    if (!(cap2 & AHCI_CAP2_BOH)) return;
    hba_w32(abar, AHCI_BOHC, AHCI_BOHC_OOS);
    /* Spec says wait at least 25 ms for firmware to release. */
    pit_sleep_ms(25);
    /* If BIOS still busy, wait up to 2 seconds. */
    if (hba_r32(abar, AHCI_BOHC) & AHCI_BOHC_BB) {
        for (int i = 0; i < 200; i++) {
            if (!(hba_r32(abar, AHCI_BOHC) & AHCI_BOHC_BB)) break;
            pit_sleep_ms(10);
        }
    }
    /* Clear BOS so we definitively own the controller. */
    hba_w32(abar, AHCI_BOHC,
            hba_r32(abar, AHCI_BOHC) & ~AHCI_BOHC_BOS);
}

static int ahci_pci_probe(struct pci_dev *dev) {
    kprintf("[ahci] probing %02x:%02x.%x  (vid:did %04x:%04x)\n",
            dev->bus, dev->slot, dev->fn, dev->vendor, dev->device);

    if (g_hba_used >= AHCI_MAX_HBAS) {
        kprintf("[ahci] g_hbas full (%d) -- declining %02x:%02x.%x\n",
                AHCI_MAX_HBAS, dev->bus, dev->slot, dev->fn);
        return -1;
    }

    pci_dev_enable(dev, PCI_CMD_MEM_SPACE | PCI_CMD_BUS_MASTER);

    /* AHCI registers live in BAR5 -- "ABAR" in the spec. */
    void *abar_v = pci_map_bar(dev, 5, 0);
    if (!abar_v) {
        kprintf("[ahci] BAR5 map failed (phys=%p)\n", (void *)dev->bar[5]);
        return -1;
    }
    volatile uint8_t *abar = (volatile uint8_t *)abar_v;
    kprintf("[ahci] ABAR phys=%p virt=%p (%lu KiB UC)\n",
            (void *)dev->bar[5], (void *)abar,
            (unsigned long)(dev->bar_size[5] / 1024u));

    /* Reserve our HBA slot now so port_init can stash the back-pointer. */
    struct ahci_hba *h = &g_hbas[g_hba_used];
    memset(h, 0, sizeof(*h));
    h->abar         = abar;
    h->drive_first  = (int)g_drive_count;
    h->drive_count  = 0;

    bios_handoff(abar);

    /* Enable AHCI mode (some HBAs default to legacy IDE emulation). */
    uint32_t ghc = hba_r32(abar, AHCI_GHC);
    hba_w32(abar, AHCI_GHC, ghc | AHCI_GHC_AE);

    /* HBA reset. Soft-resets every port + clears all internal state.
     * We could skip this on a clean boot, but it costs <1 ms and
     * guarantees we're starting from a known state regardless of what
     * Limine / BIOS / a previous OS left lying around. */
    hba_w32(abar, AHCI_GHC, hba_r32(abar, AHCI_GHC) | AHCI_GHC_HR);
    if (wait_until(abar + AHCI_GHC, AHCI_GHC_HR, 0, 1000) != 0) {
        kprintf("[ahci] HBA reset (GHC.HR) timed out\n");
        return -2;
    }
    /* AE gets cleared by HR -- re-enable. */
    hba_w32(abar, AHCI_GHC, AHCI_GHC_AE);

    /* Keep GHC.IE clear until we've allocated an MSI vector + bound
     * the handler. Otherwise the very first port_init unmask might
     * race a stray IRQ in. */
    hba_w32(abar, AHCI_GHC, hba_r32(abar, AHCI_GHC) & ~AHCI_GHC_IE);

    uint32_t cap = hba_r32(abar, AHCI_CAP);
    uint32_t pi  = hba_r32(abar, AHCI_PI);
    uint32_t vs  = hba_r32(abar, AHCI_VS);
    int np = (int)(cap & 0x1F) + 1;
    kprintf("[ahci] CAP=0x%08x PI=0x%08x VS=0x%08x  (NP=%d ports)\n",
            cap, pi, vs, np);

    int hba_id = g_hba_index++;
    int found  = 0;

    for (int i = 0; i < 32; i++) {
        if ((pi & (1u << i)) == 0) continue;
        if (g_drive_count >= AHCI_MAX_DRIVES) {
            kprintf("[ahci] WARN: g_drives full (%d) -- ignoring port %d\n",
                    AHCI_MAX_DRIVES, i);
            break;
        }
        struct ahci_drive *d = &g_drives[g_drive_count];
        memset(d, 0, sizeof(*d));
        d->port.idx  = i;
        d->port.regs = abar + AHCI_PORT_OFF(i);
        d->port.hba  = h;

        if (port_init(&d->port) != 0) continue;

        /* Build the registry name "ahci<H>:p<P>". Tiny formatter so we
         * don't drag printf into the driver. */
        char *n = d->name;
        *n++ = 'a'; *n++ = 'h'; *n++ = 'c'; *n++ = 'i';
        if (hba_id < 10) {
            *n++ = (char)('0' + hba_id);
        } else {
            *n++ = (char)('0' + (hba_id / 10) % 10);
            *n++ = (char)('0' + hba_id % 10);
        }
        *n++ = ':'; *n++ = 'p';
        if (i < 10) {
            *n++ = (char)('0' + i);
        } else {
            *n++ = (char)('0' + (i / 10) % 10);
            *n++ = (char)('0' + i % 10);
        }
        *n   = '\0';

        d->blk.name         = d->name;
        d->blk.ops          = &g_ahci_ops;
        d->blk.sector_count = d->port.sectors;
        d->blk.priv         = &d->port;
        d->live             = true;

        kprintf("[ahci] port %d: SATA %lu sectors (%lu KiB) -> %s\n",
                i, (unsigned long)d->port.sectors,
                (unsigned long)(d->port.sectors / 2u), d->name);

        blk_register(&d->blk);
        g_drive_count++;
        found++;
    }

    if (found == 0) {
        kprintf("[ahci] %02x:%02x.%x: no usable SATA drives -- "
                "leaving HBA unbound\n",
                dev->bus, dev->slot, dev->fn);
        return -3;          /* declined -- let another driver try */
    }

    /* Per-HBA MSI bring-up. Allocate one IDT vector for this HBA's
     * "any port wants attention" notification, route it at the BSP
     * LAPIC, then ask the device to deliver MSIs there. If the device
     * doesn't have an MSI cap (rare on AHCI but possible on weird
     * embedded controllers) we leave irq_enabled = false; commands
     * still complete, just via the polled PxCI fallback in
     * ahci_wait_completion.
     *
     * GHC.IE must only be flipped on AFTER the cap is enabled --
     * otherwise an untrapped INTx could arrive and confuse the IDT. */
    h->drive_count = (int)(g_drive_count - (size_t)h->drive_first);
    h->irq_count   = 0;
    h->irq_enabled = false;

    uint8_t vec = irq_alloc_vector(ahci_irq_handler, h);
    if (vec == 0) {
        kprintf("[ahci] %02x:%02x.%x: no IDT vectors free -- staying polled\n",
                dev->bus, dev->slot, dev->fn);
    } else if (!pci_msi_enable(dev, vec, (uint8_t)apic_read_id())) {
        kprintf("[ahci] %02x:%02x.%x: no MSI cap -- staying polled "
                "(allocated vec 0x%02x is now idle)\n",
                dev->bus, dev->slot, dev->fn, (unsigned)vec);
    } else {
        h->irq_vector  = vec;
        h->irq_enabled = true;
        /* Clear any latched IS bits accumulated during port_init's
         * IDENTIFY traffic before flipping the global gate, so the
         * very first MSI is caused by a real command -- not stale
         * residual state. */
        hba_w32(abar, AHCI_IS, 0xFFFFFFFFu);
        hba_w32(abar, AHCI_GHC, hba_r32(abar, AHCI_GHC) | AHCI_GHC_IE);
        kprintf("[ahci] %02x:%02x.%x: IRQ live on vec 0x%02x "
                "(GHC.IE=1, %d port(s) armed)\n",
                dev->bus, dev->slot, dev->fn,
                (unsigned)vec, h->drive_count);
    }

    g_hba_used++;
    dev->driver_data = abar_v;
    return 0;
}

static const struct pci_match g_ahci_matches[] = {
    /* Class 0x01 / Subclass 0x06 = SATA. Most modern HBAs use prog_if
     * 0x01 (AHCI 1.0) but we accept any so vendor-specific encodings
     * (0x02 = serial storage bus, etc.) still bind. */
    { PCI_ANY_ID, PCI_ANY_ID,
      PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_AHCI, PCI_ANY_CLASS },
    PCI_MATCH_END,
};

static struct pci_driver g_ahci_driver = {
    .name    = "blk_ahci",
    .matches = g_ahci_matches,
    .probe   = ahci_pci_probe,
    .remove  = 0,
};

void blk_ahci_register(void) {
    pci_register_driver(&g_ahci_driver);
}
