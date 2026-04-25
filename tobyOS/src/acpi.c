/* acpi.c -- RSDP -> XSDT -> MADT walk.
 *
 * What we extract from MADT (ACPI 6.x § 5.2.12):
 *   - LAPIC MMIO physical address (32-bit field, possibly overridden
 *     to 64-bit by a type-5 entry).
 *   - One entry per usable Local APIC (type 0). Each gives us the
 *     ACPI processor id, the APIC id, and the "enabled / online-capable"
 *     flag bits.
 *   - One entry per IO APIC (type 1): id, MMIO phys, GSI base.
 *     ioapic.c uses these to map MMIO and learn which GSIs each chip
 *     owns (the redirection-entry count comes from IOAPICVER, read at
 *     init time -- not stored in the MADT).
 *   - Interrupt Source Overrides (type 2): legacy ISA IRQ -> GSI
 *     remappings + polarity/trigger overrides. The classic case is
 *     QEMU mapping IRQ0 to GSI 2 (because the i8259 cascaded IRQ2
 *     historically chained the slave PIC, freeing GSI 0 for whatever
 *     the firmware wants).
 *   - NMI sources (type 3) and LAPIC NMI entries (type 4). Type 4 is
 *     the common one (firmware says "wire LINT1 to NMI"); type 3 is
 *     mostly historical.
 *
 * What we still ignore:
 *   - x2APIC (type 9) and x2APIC NMI (type 10). xAPIC is plenty.
 *   - Platform Interrupt Sources (type 8) -- only used by very old
 *     server boards.
 *
 * RSDP / table layout reference: ACPI 6.x spec, sections 5.2.5 .. 5.2.12.
 */

#include <tobyos/acpi.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/pmm.h>
#include <tobyos/cpu.h>
#include <tobyos/pit.h>

/* ---- ACPI structures (packed, just the fields we use) ---- */

struct acpi_rsdp_v1 {
    char     signature[8];      /* "RSD PTR " */
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;          /* 0 = ACPI 1.0, 2 = ACPI 2.0+ */
    uint32_t rsdt_phys;
} __attribute__((packed));

struct acpi_rsdp_v2 {
    struct acpi_rsdp_v1 v1;
    uint32_t length;
    uint64_t xsdt_phys;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_xsdt {
    struct acpi_sdt_header h;
    uint64_t entries[];     /* count = (length - sizeof(header)) / 8 */
} __attribute__((packed));

struct acpi_rsdt {
    struct acpi_sdt_header h;
    uint32_t entries[];     /* fall-back when only an RSDT exists */
} __attribute__((packed));

struct acpi_madt {
    struct acpi_sdt_header h;
    uint32_t lapic_phys_32;
    uint32_t flags;         /* bit0 = legacy PICs present */
    uint8_t  entries[];
} __attribute__((packed));

/* MADT entry header */
struct madt_entry_hdr {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

#define MADT_TYPE_LAPIC                 0
#define MADT_TYPE_IOAPIC                1
#define MADT_TYPE_ISO                   2   /* Interrupt Source Override */
#define MADT_TYPE_NMI_SOURCE            3   /* IO APIC NMI source */
#define MADT_TYPE_LAPIC_NMI             4   /* Local APIC NMI source */
#define MADT_TYPE_LAPIC_ADDR_OVERRIDE   5
#define MADT_TYPE_LAPIC_X2APIC          9

struct madt_lapic {
    struct madt_entry_hdr hdr;  /* type=0, length=8 */
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;             /* bit0 = enabled, bit1 = online-capable */
} __attribute__((packed));

struct madt_ioapic {
    struct madt_entry_hdr hdr;  /* type=1, length=12 */
    uint8_t  id;
    uint8_t  reserved;
    uint32_t mmio_phys;         /* always 32-bit in MADT (yes, really) */
    uint32_t gsi_base;
} __attribute__((packed));

struct madt_iso {
    struct madt_entry_hdr hdr;  /* type=2, length=10 */
    uint8_t  bus;               /* always 0 (ISA) */
    uint8_t  source;            /* ISA IRQ pin 0..15 */
    uint32_t gsi;
    uint16_t flags;             /* MPS INTI flags */
} __attribute__((packed));

struct madt_nmi_source {
    struct madt_entry_hdr hdr;  /* type=3, length=8 */
    uint16_t flags;
    uint32_t gsi;
} __attribute__((packed));

struct madt_lapic_nmi {
    struct madt_entry_hdr hdr;  /* type=4, length=6 */
    uint8_t  acpi_processor_id; /* 0xFF = applies to all CPUs */
    uint16_t flags;
    uint8_t  lint;              /* 0 or 1 */
} __attribute__((packed));

struct madt_lapic_addr_override {
    struct madt_entry_hdr hdr;  /* type=5, length=12 */
    uint16_t reserved;
    uint64_t lapic_phys;
} __attribute__((packed));

/* ---- FADT (FACP) ----
 *
 * The FADT is the ACPI 1.0 / 2.0+ Fixed ACPI Description Table.  It
 * tells us where the firmware put the power-management I/O blocks
 * (PM1_CNT, SMI_CMD), how to switch to ACPI mode (ACPI_ENABLE byte +
 * SMI_CMD port), and -- on revision 2+ -- where the FADT reset
 * register lives. We read the 1.0-compatible 32-bit fields first and
 * only fall back to the X_* (GAS) variants if the 32-bit ones are 0
 * (which happens on some ACPI 5+ platforms that mark them deprecated).
 *
 * Layout reference: ACPI 6.5 spec, section 5.2.9, table 5-9.
 *
 * We define the struct up to byte 244 (just past X_GPE1_BLK) which
 * covers every field we want.  Real FADTs may extend further with the
 * Sleep Control / Status registers (ACPI 5+); we ignore those. */
struct acpi_fadt {
    struct acpi_sdt_header h;     /* 0x00, 36 bytes */
    uint32_t firmware_ctrl;       /* 0x24 */
    uint32_t dsdt;                /* 0x28 */
    uint8_t  reserved1;           /* 0x2C */
    uint8_t  preferred_pm_profile;/* 0x2D */
    uint16_t sci_int;             /* 0x2E */
    uint32_t smi_cmd;             /* 0x30 */
    uint8_t  acpi_enable;         /* 0x34 */
    uint8_t  acpi_disable;        /* 0x35 */
    uint8_t  s4bios_req;          /* 0x36 */
    uint8_t  pstate_cnt;          /* 0x37 */
    uint32_t pm1a_evt_blk;        /* 0x38 */
    uint32_t pm1b_evt_blk;        /* 0x3C */
    uint32_t pm1a_cnt_blk;        /* 0x40 */
    uint32_t pm1b_cnt_blk;        /* 0x44 */
    uint32_t pm2_cnt_blk;         /* 0x48 */
    uint32_t pm_tmr_blk;          /* 0x4C */
    uint32_t gpe0_blk;            /* 0x50 */
    uint32_t gpe1_blk;            /* 0x54 */
    uint8_t  pm1_evt_len;         /* 0x58 */
    uint8_t  pm1_cnt_len;         /* 0x59 */
    uint8_t  pm2_cnt_len;         /* 0x5A */
    uint8_t  pm_tmr_len;          /* 0x5B */
    uint8_t  gpe0_blk_len;        /* 0x5C */
    uint8_t  gpe1_blk_len;        /* 0x5D */
    uint8_t  gpe1_base;           /* 0x5E */
    uint8_t  cst_cnt;             /* 0x5F */
    uint16_t p_lvl2_lat;          /* 0x60 */
    uint16_t p_lvl3_lat;          /* 0x62 */
    uint16_t flush_size;          /* 0x64 */
    uint16_t flush_stride;        /* 0x66 */
    uint8_t  duty_offset;         /* 0x68 */
    uint8_t  duty_width;          /* 0x69 */
    uint8_t  day_alarm;           /* 0x6A */
    uint8_t  mon_alarm;           /* 0x6B */
    uint8_t  century;             /* 0x6C */
    uint16_t iapc_boot_arch;      /* 0x6D (ACPI 2.0+) */
    uint8_t  reserved2;           /* 0x6F */
    uint32_t flags;               /* 0x70 */
    struct acpi_gas reset_reg;    /* 0x74 (ACPI 2.0+) */
    uint8_t  reset_value;         /* 0x80 */
    uint16_t arm_boot_arch;       /* 0x81 */
    uint8_t  fadt_minor;          /* 0x83 */
    uint64_t x_firmware_ctrl;     /* 0x84 */
    uint64_t x_dsdt;              /* 0x8C */
    struct acpi_gas x_pm1a_evt_blk; /* 0x94 */
    struct acpi_gas x_pm1b_evt_blk; /* 0xA0 */
    struct acpi_gas x_pm1a_cnt_blk; /* 0xAC */
    struct acpi_gas x_pm1b_cnt_blk; /* 0xB8 */
    struct acpi_gas x_pm2_cnt_blk;  /* 0xC4 */
    struct acpi_gas x_pm_tmr_blk;   /* 0xD0 */
    struct acpi_gas x_gpe0_blk;     /* 0xDC */
    struct acpi_gas x_gpe1_blk;     /* 0xE8 */
} __attribute__((packed));

/* FADT::FLAGS bits we care about (ACPI 6.x table 5-10). */
#define FADT_FLAG_RESET_REG_SUP   (1u << 10)

/* PM1_CNT bits (ACPI 6.x table 4-13). */
#define PM1_CNT_SCI_EN     (1u << 0)
#define PM1_CNT_SLP_TYPx_SHIFT   10
#define PM1_CNT_SLP_TYPx_MASK   (0x7u << PM1_CNT_SLP_TYPx_SHIFT)
#define PM1_CNT_SLP_EN     (1u << 13)

/* ---- module state ---- */

static struct acpi_info g_info;

const struct acpi_info *acpi_get(void) { return &g_info; }

/* ---- helpers ---- */

static bool sdt_checksum_ok(const void *sdt, uint32_t len) {
    const uint8_t *p = sdt;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

static bool sig_eq(const char *sig, const char *want, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (sig[i] != want[i]) return false;
    }
    return true;
}

/* Convert a phys address (from an ACPI table entry) into a kernel-virt
 * pointer via HHDM. All ACPI tables live in ACPI_RECLAIMABLE memory
 * which we mirror in the HHDM during vmm_init, so this is a free deref. */
static const void *phys_to_kv(uint64_t phys) {
    return pmm_phys_to_virt(phys);
}

/* ---- MADT walk ---- */

static void parse_madt(const struct acpi_madt *m) {
    g_info.lapic_phys         = (uint64_t)m->lapic_phys_32;
    g_info.legacy_pic_present = (m->flags & 1u) != 0u;

    kprintf("[acpi] MADT: lapic_phys (32-bit field) = %p, legacy_pic=%s\n",
            (void *)g_info.lapic_phys,
            g_info.legacy_pic_present ? "yes" : "no");

    const uint8_t *cur = m->entries;
    const uint8_t *end = (const uint8_t *)m + m->h.length;

    while (cur + sizeof(struct madt_entry_hdr) <= end) {
        const struct madt_entry_hdr *eh = (const void *)cur;
        if (eh->length == 0) {
            kprintf("[acpi] MADT: zero-length entry, bailing\n");
            break;
        }
        if (cur + eh->length > end) break;

        switch (eh->type) {
        case MADT_TYPE_LAPIC: {
            const struct madt_lapic *lp = (const void *)cur;
            if (g_info.cpu_count >= ACPI_MAX_CPUS) {
                kprintf("[acpi]   LAPIC entry skipped (>%u CPUs)\n",
                        ACPI_MAX_CPUS);
                break;
            }
            struct acpi_cpu_entry *c = &g_info.cpus[g_info.cpu_count++];
            c->acpi_processor_id = lp->acpi_processor_id;
            c->apic_id           = lp->apic_id;
            c->enabled           = (lp->flags & 1u) != 0u;
            c->online_capable    = (lp->flags & 2u) != 0u;
            kprintf("[acpi]   CPU%u: acpi_id=%u apic_id=%u flags=0x%x %s%s\n",
                    g_info.cpu_count - 1,
                    c->acpi_processor_id, c->apic_id,
                    (unsigned)lp->flags,
                    c->enabled ? "enabled " : "",
                    c->online_capable ? "online-capable" : "");
            break;
        }
        case MADT_TYPE_IOAPIC: {
            const struct madt_ioapic *io = (const void *)cur;
            if (g_info.ioapic_count >= ACPI_MAX_IOAPICS) {
                kprintf("[acpi]   IOAPIC entry skipped (>%u IO APICs)\n",
                        ACPI_MAX_IOAPICS);
                break;
            }
            struct acpi_ioapic_entry *e =
                &g_info.ioapics[g_info.ioapic_count++];
            e->id        = io->id;
            e->mmio_phys = (uint64_t)io->mmio_phys;
            e->gsi_base  = io->gsi_base;
            kprintf("[acpi]   IOAPIC %u: mmio=%p gsi_base=%u\n",
                    (unsigned)e->id, (void *)e->mmio_phys,
                    (unsigned)e->gsi_base);
            break;
        }
        case MADT_TYPE_ISO: {
            const struct madt_iso *iso = (const void *)cur;
            if (g_info.iso_count >= ACPI_MAX_ISOS) {
                kprintf("[acpi]   ISO entry skipped (>%u overrides)\n",
                        ACPI_MAX_ISOS);
                break;
            }
            struct acpi_iso_entry *e =
                &g_info.isos[g_info.iso_count++];
            e->bus     = iso->bus;
            e->isa_irq = iso->source;
            e->gsi     = iso->gsi;
            e->flags   = iso->flags;
            kprintf("[acpi]   ISO: ISA IRQ%u -> GSI %u (flags=0x%x)\n",
                    (unsigned)e->isa_irq, (unsigned)e->gsi,
                    (unsigned)e->flags);
            break;
        }
        case MADT_TYPE_NMI_SOURCE: {
            const struct madt_nmi_source *ns = (const void *)cur;
            if (g_info.nmi_count >= ACPI_MAX_NMIS) {
                kprintf("[acpi]   NMI entry skipped (>%u nmis)\n",
                        ACPI_MAX_NMIS);
                break;
            }
            struct acpi_nmi_entry *e = &g_info.nmis[g_info.nmi_count++];
            e->is_lapic_nmi      = false;
            e->gsi               = ns->gsi;
            e->acpi_processor_id = 0;
            e->lint              = 0;
            e->flags             = ns->flags;
            kprintf("[acpi]   NMI source: GSI %u (flags=0x%x)\n",
                    (unsigned)e->gsi, (unsigned)e->flags);
            break;
        }
        case MADT_TYPE_LAPIC_NMI: {
            const struct madt_lapic_nmi *ln = (const void *)cur;
            if (g_info.nmi_count >= ACPI_MAX_NMIS) {
                kprintf("[acpi]   LAPIC NMI entry skipped (>%u nmis)\n",
                        ACPI_MAX_NMIS);
                break;
            }
            struct acpi_nmi_entry *e = &g_info.nmis[g_info.nmi_count++];
            e->is_lapic_nmi      = true;
            e->gsi               = 0;
            e->acpi_processor_id = ln->acpi_processor_id;
            e->lint              = ln->lint;
            e->flags             = ln->flags;
            kprintf("[acpi]   LAPIC NMI: acpi_proc=%u lint=%u flags=0x%x\n",
                    (unsigned)e->acpi_processor_id, (unsigned)e->lint,
                    (unsigned)e->flags);
            break;
        }
        case MADT_TYPE_LAPIC_ADDR_OVERRIDE: {
            const struct madt_lapic_addr_override *ov = (const void *)cur;
            kprintf("[acpi]   LAPIC addr override -> %p\n",
                    (void *)ov->lapic_phys);
            g_info.lapic_phys = ov->lapic_phys;
            break;
        }
        case MADT_TYPE_LAPIC_X2APIC:
            kprintf("[acpi]   x2APIC LAPIC entry (ignored, xAPIC only)\n");
            break;
        default:
            /* unhandled type -- silently skip */
            break;
        }
        cur += eh->length;
    }
}

/* ---- DSDT _S5_ scan ----------------------------------------------- *
 *
 * We don't ship an AML interpreter.  Instead we use the well-known
 * trick of grepping the DSDT bytes for the "_S5_" name, then decoding
 * the small AML package literal that follows.  The encoding we look
 * for (ACPI 6.x AML grammar):
 *
 *     08            NameOp (DefName: pkg under root scope)
 *     5F 53 35 5F   "_S5_"
 *     12            PackageOp
 *     <PkgLength>   one to four bytes (top 2 bits of byte 0 = extra bytes)
 *     <NumElements> single byte; should be >= 2
 *     <SLP_TYPa>    either ByteOp(0A) + value or a literal byte
 *     <SLP_TYPb>    same
 *     ...           remaining elements (PM1a/b reserved fields)
 *
 * On QEMU the DSDT typically has SLP_TYPa = 0x05, SLP_TYPb = 0x05.
 * On real PCs the values vary but the encoding is the same.  We
 * accept either form (literal byte or ByteOp + byte) for each of the
 * first two elements.
 *
 * Returns true and fills *typa / *typb on success.  Returns false if
 * we never find "_S5_" or the surrounding bytes don't match the
 * expected NameOp + PackageOp pattern. */
static bool find_s5(const uint8_t *aml, size_t len,
                    uint8_t *typa, uint8_t *typb) {
    if (!aml || len < 8) return false;

    /* Walk every "_S5_" occurrence.  AML defines NameOp (08) as the
     * byte immediately preceding a DefName, but real DSDTs sometimes
     * wrap _S5_ in a Scope/Method, so we also accept a 0x14 (MethodOp)
     * preamble with a heuristic skip. */
    for (size_t i = 1; i + 5 < len; i++) {
        if (aml[i]   != '_' || aml[i+1] != 'S' ||
            aml[i+2] != '5' || aml[i+3] != '_') continue;

        uint8_t pre = aml[i - 1];
        size_t  p;
        if (pre == 0x08 /* NameOp */) {
            p = i + 4;
        } else {
            /* Not the NameOp form -- skip this match.  The DefName
             * byte that immediately precedes "_S5_" is what tells
             * us this is a power-state package and not an arbitrary
             * string in some other AML object. */
            continue;
        }

        if (p >= len || aml[p++] != 0x12 /* PackageOp */) continue;
        if (p >= len) continue;

        /* PkgLength: top 2 bits of byte 0 say how many extra bytes
         * follow (0..3). The actual length value is the lower 4 bits
         * of byte 0 plus the next 0..3 bytes shifted left by 4 each.
         * We don't need the value -- just need to skip past it. */
        uint8_t pl0 = aml[p++];
        size_t  extra = (size_t)((pl0 >> 6) & 0x3);
        p += extra;

        if (p >= len) continue;
        uint8_t nelem = aml[p++];
        if (nelem < 2) continue;

        /* SLP_TYPa: ByteOp + byte, or just literal byte if the value
         * fits without explicit type (some ASL compilers do this). */
        if (p >= len) continue;
        uint8_t b = aml[p++];
        uint8_t a_val;
        if (b == 0x0A /* ByteOp */) {
            if (p >= len) continue;
            a_val = aml[p++];
        } else {
            a_val = b;
        }

        /* SLP_TYPb: same encoding. */
        if (p >= len) continue;
        b = aml[p++];
        uint8_t b_val;
        if (b == 0x0A) {
            if (p >= len) continue;
            b_val = aml[p++];
        } else {
            b_val = b;
        }

        *typa = a_val;
        *typb = b_val;
        return true;
    }
    return false;
}

/* ---- FADT parse ---------------------------------------------------- */

static void parse_fadt(const struct acpi_fadt *f) {
    g_info.sci_int        = f->sci_int;
    g_info.smi_cmd        = f->smi_cmd;
    g_info.acpi_enable    = f->acpi_enable;
    g_info.acpi_disable   = f->acpi_disable;
    g_info.iapc_boot_arch = f->iapc_boot_arch;
    g_info.fadt_flags     = f->flags;
    g_info.pm1_cnt_len    = f->pm1_cnt_len;

    /* Prefer the X_PM1*_CNT_BLK GAS pointers when they look valid;
     * fall back to the 32-bit fields otherwise. ACPI 5+ deprecates
     * the 32-bit fields but every real BIOS still fills them in. */
    if (f->h.length >= 0xB4 + sizeof(struct acpi_gas) &&
        f->x_pm1a_cnt_blk.address != 0 &&
        f->x_pm1a_cnt_blk.address_space == ACPI_GAS_AS_IO) {
        g_info.pm1a_cnt = (uint32_t)f->x_pm1a_cnt_blk.address;
    } else {
        g_info.pm1a_cnt = f->pm1a_cnt_blk;
    }
    if (f->h.length >= 0xC0 + sizeof(struct acpi_gas) &&
        f->x_pm1b_cnt_blk.address != 0 &&
        f->x_pm1b_cnt_blk.address_space == ACPI_GAS_AS_IO) {
        g_info.pm1b_cnt = (uint32_t)f->x_pm1b_cnt_blk.address;
    } else {
        g_info.pm1b_cnt = f->pm1b_cnt_blk;
    }

    /* Reset register: only valid if FADT::FLAGS bit 10 is set AND the
     * FADT is large enough to actually contain the GAS. We accept
     * either I/O or memory address spaces. */
    if ((f->flags & FADT_FLAG_RESET_REG_SUP) &&
        f->h.length >= 0x80 + 1) {
        g_info.reset_supported = true;
        g_info.reset_reg       = f->reset_reg;
        g_info.reset_value     = f->reset_value;
    }

    g_info.pm_ok = (g_info.pm1a_cnt != 0);

    kprintf("[acpi] FADT: pm1a_cnt=0x%x pm1b_cnt=0x%x smi_cmd=0x%x "
            "acpi_enable=0x%02x sci_int=%u\n",
            (unsigned)g_info.pm1a_cnt, (unsigned)g_info.pm1b_cnt,
            (unsigned)g_info.smi_cmd, (unsigned)g_info.acpi_enable,
            (unsigned)g_info.sci_int);
    if (g_info.reset_supported) {
        kprintf("[acpi] FADT: reset_reg space=%u addr=0x%lx value=0x%02x\n",
                (unsigned)g_info.reset_reg.address_space,
                (unsigned long)g_info.reset_reg.address,
                (unsigned)g_info.reset_value);
    } else {
        kprintf("[acpi] FADT: reset_reg not supported (flags=0x%x)\n",
                (unsigned)f->flags);
    }

    /* Now grab the DSDT and grep for _S5_. Prefer X_DSDT (64-bit)
     * if it's set and the FADT is large enough to contain it. */
    uint64_t dsdt_phys = 0;
    if (f->h.length >= 0x8C + 8 && f->x_dsdt != 0) {
        dsdt_phys = f->x_dsdt;
    } else if (f->dsdt != 0) {
        dsdt_phys = (uint64_t)f->dsdt;
    }
    if (!dsdt_phys) {
        kprintf("[acpi] FADT: no DSDT pointer -- shutdown unavailable\n");
        return;
    }

    const struct acpi_sdt_header *dh = phys_to_kv(dsdt_phys);
    if (!sig_eq(dh->signature, "DSDT", 4)) {
        kprintf("[acpi] DSDT @ %p has bad signature -- shutdown unavailable\n",
                (void *)dsdt_phys);
        return;
    }
    /* Don't checksum the DSDT -- many BIOSes ship broken DSDT
     * checksums and Linux/uACPI tolerate them too.  Just parse. */
    const uint8_t *aml = (const uint8_t *)dh + sizeof(struct acpi_sdt_header);
    size_t aml_len = (size_t)dh->length - sizeof(struct acpi_sdt_header);

    /* Stash DSDT body for M26G heuristic-AML scans (battery, etc). */
    g_info.dsdt.phys = dsdt_phys + sizeof(struct acpi_sdt_header);
    g_info.dsdt.aml  = aml;
    g_info.dsdt.len  = (uint32_t)aml_len;

    uint8_t typa, typb;
    if (find_s5(aml, aml_len, &typa, &typb)) {
        g_info.s5_ok   = true;
        g_info.s5_typa = typa;
        g_info.s5_typb = typb;
        kprintf("[acpi] DSDT _S5_: SLP_TYPa=%u SLP_TYPb=%u (DSDT %u bytes)\n",
                (unsigned)typa, (unsigned)typb, (unsigned)dh->length);
    } else {
        kprintf("[acpi] DSDT _S5_ not found (DSDT %u bytes) -- "
                "shutdown unavailable\n", (unsigned)dh->length);
    }
}

/* Walk the XSDT/RSDT a second time and stash every SSDT we find into
 * g_info.ssdts[]. Real BIOSes load extra ASL into SSDTs (PCI hotplug,
 * thermal zones, batteries on some machines), so M26G needs to scan
 * them too -- not just the DSDT. We cap at ACPI_MAX_SSDTS; overflow
 * is silently dropped after a kprintf warning, which matches what
 * Linux does on truly bizarre firmwares. */
static void collect_ssdts_xsdt(const struct acpi_xsdt *xsdt) {
    uint32_t n = (xsdt->h.length - sizeof(struct acpi_sdt_header)) / 8u;
    for (uint32_t i = 0; i < n; i++) {
        const struct acpi_sdt_header *h = phys_to_kv(xsdt->entries[i]);
        if (!sig_eq(h->signature, "SSDT", 4)) continue;
        if (g_info.ssdt_count >= ACPI_MAX_SSDTS) {
            kprintf("[acpi] more than %u SSDTs -- dropping rest\n",
                    (unsigned)ACPI_MAX_SSDTS);
            return;
        }
        struct acpi_aml_table *t = &g_info.ssdts[g_info.ssdt_count++];
        t->phys = xsdt->entries[i] + sizeof(struct acpi_sdt_header);
        t->aml  = (const uint8_t *)h + sizeof(struct acpi_sdt_header);
        t->len  = (uint32_t)((size_t)h->length - sizeof(struct acpi_sdt_header));
        kprintf("[acpi] SSDT[%u] %u bytes\n",
                (unsigned)(g_info.ssdt_count - 1), (unsigned)t->len);
    }
}

static void collect_ssdts_rsdt(const struct acpi_rsdt *rsdt) {
    uint32_t n = (rsdt->h.length - sizeof(struct acpi_sdt_header)) / 4u;
    for (uint32_t i = 0; i < n; i++) {
        const struct acpi_sdt_header *h = phys_to_kv((uint64_t)rsdt->entries[i]);
        if (!sig_eq(h->signature, "SSDT", 4)) continue;
        if (g_info.ssdt_count >= ACPI_MAX_SSDTS) {
            kprintf("[acpi] more than %u SSDTs -- dropping rest\n",
                    (unsigned)ACPI_MAX_SSDTS);
            return;
        }
        struct acpi_aml_table *t = &g_info.ssdts[g_info.ssdt_count++];
        t->phys = (uint64_t)rsdt->entries[i] + sizeof(struct acpi_sdt_header);
        t->aml  = (const uint8_t *)h + sizeof(struct acpi_sdt_header);
        t->len  = (uint32_t)((size_t)h->length - sizeof(struct acpi_sdt_header));
        kprintf("[acpi] SSDT[%u] %u bytes\n",
                (unsigned)(g_info.ssdt_count - 1), (unsigned)t->len);
    }
}

/* Generic "find table by signature" -- scans either the XSDT (preferred,
 * 64-bit pointers) or the RSDT (32-bit pointers, fallback). Returns
 * nullptr on miss. */
static const struct acpi_sdt_header *find_table_xsdt(const struct acpi_xsdt *xsdt,
                                                     const char *sig) {
    uint32_t n = (xsdt->h.length - sizeof(struct acpi_sdt_header)) / 8u;
    for (uint32_t i = 0; i < n; i++) {
        const struct acpi_sdt_header *h = phys_to_kv(xsdt->entries[i]);
        if (sig_eq(h->signature, sig, 4)) return h;
    }
    return 0;
}

static const struct acpi_sdt_header *find_table_rsdt(const struct acpi_rsdt *rsdt,
                                                     const char *sig) {
    uint32_t n = (rsdt->h.length - sizeof(struct acpi_sdt_header)) / 4u;
    for (uint32_t i = 0; i < n; i++) {
        const struct acpi_sdt_header *h = phys_to_kv((uint64_t)rsdt->entries[i]);
        if (sig_eq(h->signature, sig, 4)) return h;
    }
    return 0;
}

/* ---- public entry ---- */

const struct acpi_info *acpi_init(void *limine_rsdp_address) {
    memset(&g_info, 0, sizeof(g_info));

    if (!limine_rsdp_address) {
        kprintf("[acpi] Limine gave us no RSDP -- ACPI disabled\n");
        return &g_info;
    }

    const struct acpi_rsdp_v1 *rsdp1 = limine_rsdp_address;
    if (!sig_eq(rsdp1->signature, "RSD PTR ", 8)) {
        kprintf("[acpi] RSDP signature mismatch -- giving up\n");
        return &g_info;
    }

    /* v2+ RSDP carries the XSDT (64-bit pointers). v1 has only RSDT. */
    const struct acpi_sdt_header *madt_hdr = 0;
    const struct acpi_sdt_header *fadt_hdr = 0;
    const struct acpi_xsdt *xsdt = 0;
    const struct acpi_rsdt *rsdt = 0;
    if (rsdp1->revision >= 2) {
        const struct acpi_rsdp_v2 *rsdp2 = limine_rsdp_address;
        xsdt = phys_to_kv(rsdp2->xsdt_phys);
        if (!sig_eq(xsdt->h.signature, "XSDT", 4)) {
            kprintf("[acpi] bad XSDT signature\n");
            return &g_info;
        }
        if (!sdt_checksum_ok(xsdt, xsdt->h.length)) {
            kprintf("[acpi] XSDT checksum bad (revision=%u, length=%u)\n",
                    (unsigned)rsdp1->revision, (unsigned)xsdt->h.length);
        }
        kprintf("[acpi] using XSDT @ %p (entries=%u)\n",
                (void *)rsdp2->xsdt_phys,
                (unsigned)((xsdt->h.length - sizeof(xsdt->h)) / 8u));
        madt_hdr = find_table_xsdt(xsdt, "APIC");
        fadt_hdr = find_table_xsdt(xsdt, "FACP");
    } else {
        rsdt = phys_to_kv((uint64_t)rsdp1->rsdt_phys);
        if (!sig_eq(rsdt->h.signature, "RSDT", 4)) {
            kprintf("[acpi] bad RSDT signature\n");
            return &g_info;
        }
        kprintf("[acpi] using RSDT @ %p (entries=%u)\n",
                (void *)(uint64_t)rsdp1->rsdt_phys,
                (unsigned)((rsdt->h.length - sizeof(rsdt->h)) / 4u));
        madt_hdr = find_table_rsdt(rsdt, "APIC");
        fadt_hdr = find_table_rsdt(rsdt, "FACP");
    }

    if (!madt_hdr) {
        kprintf("[acpi] MADT (\"APIC\") not found -- single-CPU only\n");
        return &g_info;
    }
    if (!sdt_checksum_ok(madt_hdr, madt_hdr->length)) {
        kprintf("[acpi] MADT checksum bad -- continuing anyway\n");
    }

    parse_madt((const struct acpi_madt *)madt_hdr);
    g_info.ok = true;

    kprintf("[acpi] enumerated %u CPU(s) (%s), %u IO APIC(s), "
            "%u override(s), %u NMI source(s)\n",
            g_info.cpu_count, g_info.cpu_count > 1 ? "SMP" : "uniprocessor",
            g_info.ioapic_count, g_info.iso_count, g_info.nmi_count);

    /* FADT is optional from the kernel's POV -- we'll boot fine
     * without it, just no clean shutdown. Most platforms have one. */
    if (fadt_hdr) {
        if (!sdt_checksum_ok(fadt_hdr, fadt_hdr->length)) {
            kprintf("[acpi] FADT checksum bad -- continuing anyway\n");
        }
        parse_fadt((const struct acpi_fadt *)fadt_hdr);
    } else {
        kprintf("[acpi] FADT (\"FACP\") not found -- "
                "shutdown/reboot fall back to PCI 0xCF9 / 8042\n");
    }

    /* M26G: also harvest every SSDT for later AML byte scans. We do
     * this AFTER parse_fadt so the DSDT stash is populated first --
     * the order matters only for kprintf readability. */
    if (xsdt) collect_ssdts_xsdt(xsdt);
    else if (rsdt) collect_ssdts_rsdt(rsdt);
    kprintf("[acpi] AML inventory: dsdt=%u bytes, %u SSDT(s)\n",
            (unsigned)g_info.dsdt.len, (unsigned)g_info.ssdt_count);

    return &g_info;
}

/* ---- M26G AML byte search ---------------------------------------- *
 *
 * Used by drivers that do "presence by name" detection without an
 * AML interpreter -- they search for a literal byte sequence (often
 * a 7-byte _HID like "PNP0C0A") across DSDT + every SSDT. This is a
 * naive O(N*M) scan, which is fine because tables top out at maybe
 * 100 KiB total and needles are <16 B. If we ever need fuzzier
 * matching we'd build a real AML namespace walker, but for M26G the
 * literal-match heuristic is sufficient.
 */
static bool scan_one_table(const uint8_t *aml, uint32_t len,
                           const uint8_t *needle, size_t nlen,
                           uint32_t *off_out) {
    if (!aml || !needle || nlen == 0 || (size_t)len < nlen) return false;
    for (uint32_t i = 0; i + nlen <= len; i++) {
        if (aml[i] != needle[0]) continue;
        bool ok = true;
        for (size_t k = 1; k < nlen; k++) {
            if (aml[i + k] != needle[k]) { ok = false; break; }
        }
        if (ok) { if (off_out) *off_out = i; return true; }
    }
    return false;
}

bool acpi_aml_find_bytes(const void *needle, size_t needle_len,
                         int *which_table_out,
                         uint32_t *offset_out) {
    if (!g_info.ok && g_info.dsdt.len == 0 && g_info.ssdt_count == 0) {
        return false;
    }
    const uint8_t *n = needle;
    if (g_info.dsdt.len &&
        scan_one_table(g_info.dsdt.aml, g_info.dsdt.len, n, needle_len,
                       offset_out)) {
        if (which_table_out) *which_table_out = 0;
        return true;
    }
    for (uint32_t i = 0; i < g_info.ssdt_count; i++) {
        if (scan_one_table(g_info.ssdts[i].aml, g_info.ssdts[i].len,
                           n, needle_len, offset_out)) {
            if (which_table_out) *which_table_out = (int)(i + 1);
            return true;
        }
    }
    return false;
}

/* ---- power-management entry points -------------------------------- *
 *
 * acpi_shutdown() and acpi_reboot() are intentionally noisy: they
 * print a one-liner explaining which hardware path they're taking
 * before they pull the trigger.  This makes serial.log debugging
 * trivial when one path doesn't work on a particular machine.
 */

/* If FADT advertised an SMI handshake to switch from legacy to ACPI
 * mode and SCI_EN isn't already set, run the handshake.  This is a
 * no-op on QEMU (which boots straight into ACPI mode) but matters on
 * real PCs where the BIOS leaves the OS to do the transition. */
static void enter_acpi_mode(void) {
    if (g_info.smi_cmd == 0 || g_info.acpi_enable == 0) return;
    uint16_t cnt = inw((uint16_t)g_info.pm1a_cnt);
    if (cnt & PM1_CNT_SCI_EN) return;          /* already in ACPI mode */
    outb((uint16_t)g_info.smi_cmd, g_info.acpi_enable);
    /* Spec gives the BIOS up to 3 seconds to react; in practice it's
     * microseconds. We poll PM1a_CNT.SCI_EN with a generous loop so
     * we don't end up writing SLP_EN before ACPI mode kicks in. */
    for (int i = 0; i < 1000000; i++) {
        if (inw((uint16_t)g_info.pm1a_cnt) & PM1_CNT_SCI_EN) {
            kprintf("[acpi] entered ACPI mode after %d polls\n", i);
            return;
        }
        io_wait();
    }
    kprintf("[acpi] WARN: SCI_EN never asserted after writing 0x%02x to "
            "SMI_CMD 0x%x -- proceeding anyway\n",
            g_info.acpi_enable, g_info.smi_cmd);
}

void acpi_shutdown(void) {
    if (!g_info.pm_ok || !g_info.s5_ok) {
        kprintf("[acpi] shutdown unavailable: pm_ok=%d s5_ok=%d -- "
                "halting CPU instead\n",
                (int)g_info.pm_ok, (int)g_info.s5_ok);
        cli();
        hlt_forever();
    }

    /* Disable interrupts -- once we write SLP_EN the platform will be
     * powering down rails, and a stray IRQ during that window is
     * unhelpful.  We never need them re-enabled. */
    cli();
    enter_acpi_mode();

    uint16_t pm1a_val = (uint16_t)((g_info.s5_typa << PM1_CNT_SLP_TYPx_SHIFT) |
                                   PM1_CNT_SLP_EN);
    kprintf("[acpi] shutdown: writing PM1a_CNT(0x%x) <- 0x%04x "
            "(SLP_TYPa=%u | SLP_EN)\n",
            (unsigned)g_info.pm1a_cnt, (unsigned)pm1a_val,
            (unsigned)g_info.s5_typa);
    outw((uint16_t)g_info.pm1a_cnt, pm1a_val);

    if (g_info.pm1b_cnt) {
        uint16_t pm1b_val = (uint16_t)((g_info.s5_typb << PM1_CNT_SLP_TYPx_SHIFT) |
                                       PM1_CNT_SLP_EN);
        kprintf("[acpi] shutdown: writing PM1b_CNT(0x%x) <- 0x%04x\n",
                (unsigned)g_info.pm1b_cnt, (unsigned)pm1b_val);
        outw((uint16_t)g_info.pm1b_cnt, pm1b_val);
    }

    /* If we got here, the platform refused our shutdown request.
     * Halt visibly so the user knows something's wrong. */
    kprintf("[acpi] shutdown: hardware did not power off -- halting\n");
    hlt_forever();
}

/* Try the FADT reset register.  Returns true if we actually wrote it
 * (which usually means we never came back -- this is the "successful"
 * path).  Returns false if reset_supported was false or the GAS used
 * an address space we don't know how to write. */
static bool try_fadt_reset(void) {
    if (!g_info.reset_supported) return false;

    const struct acpi_gas *r = &g_info.reset_reg;
    uint8_t  v = g_info.reset_value;
    if (r->address_space == ACPI_GAS_AS_IO) {
        kprintf("[acpi] reboot: FADT reset I/O 0x%lx <- 0x%02x\n",
                (unsigned long)r->address, (unsigned)v);
        outb((uint16_t)r->address, v);
        return true;
    }
    if (r->address_space == ACPI_GAS_AS_MEMORY) {
        /* Single-byte MMIO write via HHDM. We don't bother mapping a
         * fresh page -- the FADT reset register is always in the
         * BIOS-mapped range that HHDM mirrors at boot. */
        volatile uint8_t *p = (volatile uint8_t *)pmm_phys_to_virt(r->address);
        kprintf("[acpi] reboot: FADT reset MMIO %p <- 0x%02x\n",
                (void *)r->address, (unsigned)v);
        *p = v;
        return true;
    }
    kprintf("[acpi] reboot: FADT reset_reg uses unsupported space %u\n",
            (unsigned)r->address_space);
    return false;
}

void acpi_reboot(void) {
    /* Disable interrupts so a pending IRQ doesn't preempt us mid-
     * reset -- we want the reset write to be the last memory op. */
    cli();

    /* 1. ACPI FADT reset register (preferred). */
    if (try_fadt_reset()) {
        for (volatile int i = 0; i < 1000000; i++) { /* settle */ }
    }

    /* 2. PCI Reset Control Register. Bit 1 = system reset, bit 2 =
     *    pulse it.  Honoured by every QEMU machine type and by every
     *    Intel/AMD chipset since ICH4-ish. */
    kprintf("[acpi] reboot: PCI 0xCF9 <- 0x06\n");
    outb(0xCF9, 0x06);
    for (volatile int i = 0; i < 1000000; i++) { /* settle */ }

    /* 3. 8042 keyboard controller pulse-reset (system reset line). */
    kprintf("[acpi] reboot: 8042 0x64 <- 0xFE (pulse RESET#)\n");
    outb(0x64, 0xFE);
    for (volatile int i = 0; i < 1000000; i++) { /* settle */ }

    /* 4. Triple fault as a last resort.  Loading a NULL IDT and
     *    raising any interrupt forces the CPU into shutdown which
     *    most chipsets translate to a reset. */
    kprintf("[acpi] reboot: every reset path failed -- triple fault\n");
    struct { uint16_t limit; uint64_t base; } __attribute__((packed))
        null_idt = { 0, 0 };
    __asm__ volatile ("lidt %0; int $0x03" : : "m"(null_idt));

    hlt_forever();
}

/* ---- M22 boot self-test --------------------------------------------- *
 *
 * Out-of-the-box this is a stub. Building the kernel with
 *   -DACPI_M22_SELFTEST
 * (via `make m22shutdowntest`) flips it into "arm a deadline now,
 * trigger acpi_shutdown when the deadline passes" mode.  idle_loop()
 * polls acpi_m22_selftest_tick() on every wake, so the trigger fires
 * within ~10 ms of the deadline (PIT @ 100 Hz). */
#ifdef ACPI_M22_SELFTEST
/* Seconds to wait after kmain finishes before pulling the trigger.
 * 12s gives the GUI/login plenty of time to come up so we exercise
 * the full driver-running shutdown path, not just early init. */
#define M22_SELFTEST_DELAY_SEC   12u
static volatile uint64_t s_m22_deadline_ticks = 0;

void acpi_m22_selftest_arm(void) {
    uint32_t hz = pit_hz();
    if (hz == 0) hz = 100;          /* paranoia: pit may not be init yet */
    s_m22_deadline_ticks = pit_ticks() + (uint64_t)hz * M22_SELFTEST_DELAY_SEC;
    kprintf("[m22-selftest] armed: will acpi_shutdown() after %u s "
            "(deadline=%lu ticks @ %u Hz)\n",
            (unsigned)M22_SELFTEST_DELAY_SEC,
            (unsigned long)s_m22_deadline_ticks, (unsigned)hz);
}

void acpi_m22_selftest_tick(void) {
    if (s_m22_deadline_ticks == 0) return;
    if (pit_ticks() < s_m22_deadline_ticks) return;
    /* Disarm so the kprintf below doesn't fire twice if QEMU
     * for some reason returns from the shutdown write (e.g. if
     * the user ran with -no-shutdown). */
    s_m22_deadline_ticks = 0;
    kprintf("[m22-selftest] deadline reached -- triggering acpi_shutdown()\n");
    acpi_shutdown();   /* noreturn under normal conditions */
}
#else  /* !ACPI_M22_SELFTEST */
void acpi_m22_selftest_arm(void)  { /* no-op in default builds */ }
void acpi_m22_selftest_tick(void) { /* no-op in default builds */ }
#endif
