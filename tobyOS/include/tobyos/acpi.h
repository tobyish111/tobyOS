/* acpi.h -- ACPI table walker.
 *
 * We extract everything we need from the MADT ("APIC") table:
 *   - LAPIC MMIO phys + per-CPU info (cpu_count, cpus[])
 *   - IO APIC entries (ioapic_count, ioapics[])
 *   - Interrupt Source Overrides (iso_count, isos[])
 *   - NMI source / LAPIC NMI entries (nmi_count, nmis[])
 *
 * Boot dependency: must run after vmm_init (because the RSDP/XSDT
 * pointers Limine gives us are higher-half virtual, valid only once
 * our PML4 mirrors HHDM).
 */

#ifndef TOBYOS_ACPI_H
#define TOBYOS_ACPI_H

#include <tobyos/types.h>

/* Result struct populated by acpi_init: what acpi.c learned about the
 * machine. Other modules read it (apic.c uses lapic_phys, smp.c uses
 * the cpu list, ioapic.c uses the ioapic list + ISO list). */

#define ACPI_MAX_CPUS    32u
#define ACPI_MAX_IOAPICS  4u
#define ACPI_MAX_ISOS    32u
#define ACPI_MAX_NMIS    16u

struct acpi_cpu_entry {
    uint32_t acpi_processor_id;
    uint32_t apic_id;
    bool     enabled;     /* MADT entry flags & 1 -- usable now */
    bool     online_capable;  /* flags & 2 -- can be brought up */
};

/* MADT type 1: I/O APIC. The chip lives at `mmio_phys` and owns GSIs
 * [gsi_base, gsi_base + redir_count). redir_count is read from the
 * IOAPICVER register (computed by ioapic.c, not here). */
struct acpi_ioapic_entry {
    uint8_t  id;            /* IO APIC's own id (informational) */
    uint64_t mmio_phys;     /* physical base of the 4 KiB MMIO region */
    uint32_t gsi_base;      /* first GSI this chip handles */
};

/* MADT type 2: Interrupt Source Override. Tells us where a legacy
 * ISA IRQ (`isa_irq` 0..15) actually got wired up on the IO APIC
 * (`gsi`), plus the polarity / trigger override (MPS-style flags --
 * see ACPI 6.x spec table 5.45 "MPS INTI Flags"). */
struct acpi_iso_entry {
    uint8_t  bus;           /* always 0 = ISA per spec */
    uint8_t  isa_irq;       /* legacy IRQ pin 0..15 */
    uint32_t gsi;           /* where to actually wire it */
    uint16_t flags;         /* MPS INTI flags (polarity + trigger) */
};

/* MADT type 3 (IO APIC NMI source) and type 4 (Local APIC NMI source)
 * collapsed into one record; `is_lapic_nmi` distinguishes them. For
 * type 3 we record the GSI; for type 4 we record the (acpi_proc_id,
 * lint) pair. We program type 4s into LVTLINTn during apic_init_local. */
struct acpi_nmi_entry {
    bool     is_lapic_nmi;       /* true for type 4 */
    uint32_t gsi;                /* type 3 only */
    uint8_t  acpi_processor_id;  /* type 4 only -- 0xFF = all CPUs */
    uint8_t  lint;               /* type 4 only -- 0 or 1 */
    uint16_t flags;              /* MPS INTI flags */
};

/* MPS INTI flag layout (ACPI 6.x section 5.2.12.5). */
#define ACPI_MPS_POLARITY_MASK    0x3u
#define ACPI_MPS_POLARITY_BUS     0x0u   /* conforming to bus default */
#define ACPI_MPS_POLARITY_HIGH    0x1u
#define ACPI_MPS_POLARITY_LOW     0x3u
#define ACPI_MPS_TRIGGER_MASK     0xCu
#define ACPI_MPS_TRIGGER_BUS      0x0u   /* conforming to bus default */
#define ACPI_MPS_TRIGGER_EDGE     0x4u
#define ACPI_MPS_TRIGGER_LEVEL    0xCu

/* ACPI 6.x § 5.2.3.2: Generic Address Structure. We only need the
 * I/O-port (address_space=1) and SystemMemory (address_space=0)
 * variants for now, so the bit_width / bit_offset / access_size
 * fields are mostly informational. */
struct acpi_gas {
    uint8_t  address_space;   /* 0=memory, 1=I/O, 2=PCI cfg, ... */
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;     /* 0=undef, 1=byte, 2=word, 3=dword, 4=qword */
    uint64_t address;
} __attribute__((packed));

#define ACPI_GAS_AS_MEMORY      0u
#define ACPI_GAS_AS_IO          1u
#define ACPI_GAS_AS_PCI_CFG     2u

struct acpi_info {
    bool                  ok;
    uint64_t              lapic_phys;       /* from MADT (or override entry) */
    bool                  legacy_pic_present;

    uint32_t              cpu_count;
    struct acpi_cpu_entry cpus[ACPI_MAX_CPUS];

    uint32_t                 ioapic_count;
    struct acpi_ioapic_entry ioapics[ACPI_MAX_IOAPICS];

    uint32_t                 iso_count;
    struct acpi_iso_entry    isos[ACPI_MAX_ISOS];

    uint32_t                 nmi_count;
    struct acpi_nmi_entry    nmis[ACPI_MAX_NMIS];

    /* ---- power management (parsed from FADT + DSDT) ---- *
     *
     * pm_ok is true once we've parsed an FADT we can use; it does NOT
     * imply we found _S5_ in the DSDT (that's s5_ok).  Without _S5_
     * we can still reboot via the FADT reset register / PCI 0xCF9 /
     * 8042 pulse, but we can't issue a clean shutdown.
     *
     * pm1a_cnt and pm1b_cnt are the I/O ports for PM1_CNT (the 16-bit
     * register that takes SLP_TYPa | SLP_EN(1<<13) for sleep/shutdown).
     * pm1b_cnt == 0 means "no PM1b block on this platform" -- only
     * PM1a needs to be written (QEMU's case).
     *
     * smi_cmd / acpi_enable transition the platform from "legacy IRQ"
     * mode to "ACPI mode" so writes to PM1_CNT actually take effect.
     * If smi_cmd == 0 (or the SCI_EN bit is already set in PM1a_CNT)
     * we skip that handshake.
     */
    bool      pm_ok;
    uint16_t  sci_int;             /* GSI for the SCI -- informational */
    uint32_t  smi_cmd;             /* I/O port for the SMI command reg */
    uint8_t   acpi_enable;         /* byte to write to SMI_CMD */
    uint8_t   acpi_disable;
    uint32_t  pm1a_cnt;            /* I/O port for PM1a_CNT_BLK (always set on QEMU) */
    uint32_t  pm1b_cnt;            /* 0 if not present */
    uint8_t   pm1_cnt_len;         /* width of PM1_CNT (typically 2 bytes) */
    uint16_t  iapc_boot_arch;      /* legacy hardware advertisement bits */
    uint32_t  fadt_flags;

    /* FADT reset register. reset_supported requires both
     * FADT::FLAGS bit 10 (RESET_REG_SUP) AND the GAS to be valid. */
    bool      reset_supported;
    struct acpi_gas reset_reg;
    uint8_t   reset_value;

    /* DSDT _S5_ package: the SLP_TYPa / SLP_TYPb values we need to
     * write to PM1a_CNT / PM1b_CNT to enter the soft-off (S5) state.
     * s5_ok = false if we couldn't grep _S5_ out of the DSDT. */
    bool      s5_ok;
    uint8_t   s5_typa;
    uint8_t   s5_typb;

    /* ---- AML table inventory (M26G) ---- *
     *
     * dsdt_phys / dsdt_len: the DSDT body, NOT including its
     * acpi_sdt_header (i.e. the first byte at *dsdt_aml is already
     * AML opcodes). Both zero if no DSDT was located.
     *
     * ssdt_count + ssdts[]: every SSDT we found while walking the
     * XSDT/RSDT, also stripped to "AML body" form. We store at most
     * ACPI_MAX_SSDTS; if the firmware has more, the overflow is
     * silently dropped (loud kprintf at parse time). On QEMU q35
     * there are typically 1-3 SSDTs (cpus, pci hotplug, etc.); on
     * real laptops 5-10 is common.
     *
     * The phys / aml split exists because some callers want a stable
     * physical pointer (for DMA-backed parsing later), and others
     * just want the linear in-kernel virtual address. Both are
     * pre-mapped via the HHDM. */
#define ACPI_MAX_SSDTS  16u
    struct acpi_aml_table {
        uint64_t        phys;        /* phys address of acpi_sdt_header */
        const uint8_t  *aml;         /* virt of body (after header)     */
        uint32_t        len;         /* body length in bytes            */
    };

    struct acpi_aml_table dsdt;
    uint32_t              ssdt_count;
    struct acpi_aml_table ssdts[ACPI_MAX_SSDTS];
};

/* Parse the firmware tables. Idempotent: safe to call once. Logs what
 * it found via kprintf. Returns false (and leaves info.ok = false) if
 * Limine didn't give us an RSDP, or we didn't find the MADT. The
 * kernel keeps booting on failure -- we simply don't bring up APs. */
const struct acpi_info *acpi_init(void *limine_rsdp_address);

/* Read-only accessor for other modules. */
const struct acpi_info *acpi_get(void);

/* Search the DSDT and every SSDT for an exact byte sequence. Used by
 * pure-heuristic device probes (M26G battery, M26G+ thermal, ...)
 * that don't ship a full AML interpreter but still want to detect
 * presence by name (e.g. the _HID literal "PNP0C0A"). Returns true
 * on first hit. Returns false if acpi_init() hasn't run, no AML
 * tables were located, or `needle` doesn't appear anywhere.
 *
 * `which_table_out` (optional) is set to 0 for DSDT, 1+i for SSDT i
 * on hit. `offset_out` (optional) gets the byte offset within that
 * table's AML body. Both unchanged if false is returned. */
bool acpi_aml_find_bytes(const void *needle, size_t needle_len,
                         int *which_table_out,
                         uint32_t *offset_out);

/* ---- Power management entry points ---- *
 *
 * Both functions disable interrupts, give the platform a moment to
 * settle, and then either succeed (in which case the CPU never
 * returns) or fall through to hlt_forever() if the hardware refused
 * to reset / sleep.  Neither returns under normal circumstances --
 * callers should treat them as noreturn (they're marked as such).
 *
 * acpi_shutdown() requires both pm_ok and s5_ok.  If either is
 * missing we log a clear failure and halt -- the kernel cannot
 * fabricate an S5 sleep type without the DSDT package.
 *
 * acpi_reboot() tries (in order): the FADT reset register, the
 * PCI 0xCF9 reset control register, the 8042 keyboard-controller
 * pulse-reset, and finally a triple fault by loading a NULL IDT
 * and triggering an interrupt.  At least one of these works on
 * every PC/QEMU machine we've ever seen.
 */
__attribute__((noreturn)) void acpi_shutdown(void);
__attribute__((noreturn)) void acpi_reboot(void);

/* ---- Milestone-22 boot self-test (build-flagged) ----
 *
 * Compiled in only when the kernel is built with -DACPI_M22_SELFTEST
 * (via `make m22shutdowntest`).  The default build leaves this as a
 * no-op stub.  When enabled, the kernel arms a one-shot deadline
 * shortly after kmain finishes; once idle_loop() observes the
 * deadline has passed, it calls acpi_shutdown() and -- with QEMU
 * launched WITHOUT -no-shutdown -- the VM exits cleanly.
 *
 * This is the automated test path for verifying that:
 *   1. parse_fadt() found PM1a_CNT and SLP_TYPa
 *   2. acpi_shutdown() correctly writes the magic word
 *   3. The Makefile run-* targets dropped -no-shutdown so QEMU
 *      actually exits on guest-driven ACPI S5 (vs pausing).
 */
void acpi_m22_selftest_arm(void);
void acpi_m22_selftest_tick(void);

#endif /* TOBYOS_ACPI_H */
