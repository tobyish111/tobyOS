/* ioapic.c -- I/O APIC driver (82093AA / ICH IOxAPIC).
 *
 * MMIO model: each IO APIC exposes only TWO registers in its 4 KiB MMIO
 * window:
 *
 *   +0x00  IOREGSEL    : write the register index here
 *   +0x10  IOWIN       : read/write the selected register's data here
 *
 * Indirect registers we touch:
 *
 *   0x00  IOAPICID    : id (bits 27:24)
 *   0x01  IOAPICVER   : ver (bits  7:0), max_redir_entry (bits 23:16)
 *   0x10  IOREDTBL[0]_lo  ..   redir[0] low half
 *   0x11  IOREDTBL[0]_hi  ..   redir[0] high half (destination apic id)
 *   ...
 *   0x10 + 2n     IOREDTBL[n]_lo
 *   0x10 + 2n + 1 IOREDTBL[n]_hi
 *
 * Redirection entry low (32 bits) layout (Intel 82093AA datasheet § 3.2.4):
 *
 *   [7:0]   Vector (0x10..0xFE -- below 0x10 is reserved for #DE etc.)
 *   [10:8]  Delivery Mode  (000 = Fixed, 001 = Lowest Priority,
 *                           010 = SMI,   100 = NMI, 101 = INIT,
 *                           111 = ExtINT)
 *   [11]    Destination Mode (0 = Physical APIC ID, 1 = Logical)
 *   [12]    Delivery Status (RO)
 *   [13]    Polarity        (0 = active high, 1 = active low)
 *   [14]    Remote IRR      (RO -- set on accept, cleared on EOI)
 *   [15]    Trigger         (0 = edge, 1 = level)
 *   [16]    Mask            (1 = ignore this line)
 *   [31:17] Reserved
 *
 * Redirection entry high (32 bits): destination APIC ID in bits [31:24]
 * (physical mode; the rest is reserved).
 *
 * Boot dependency: vmm + acpi + apic must be live before ioapic_init.
 */

#include <tobyos/ioapic.h>
#include <tobyos/acpi.h>
#include <tobyos/vmm.h>
#include <tobyos/pmm.h>
#include <tobyos/printk.h>

#define IOAPIC_REG_ID         0x00
#define IOAPIC_REG_VER        0x01
#define IOAPIC_REG_REDTBL(n)  (0x10u + ((uint32_t)(n) * 2u))   /* low half */

/* Low-half bits we ever set. */
#define IOAPIC_REDIR_VECTOR(v)        ((uint32_t)(v) & 0xFFu)
#define IOAPIC_REDIR_DELIVERY_FIXED   (0u << 8)
#define IOAPIC_REDIR_DEST_PHYSICAL    (0u << 11)
#define IOAPIC_REDIR_POLARITY_LOW     (1u << 13)
#define IOAPIC_REDIR_TRIGGER_LEVEL    (1u << 15)
#define IOAPIC_REDIR_MASKED           (1u << 16)

/* One per chip. We keep them in the same order MADT reported them. */
struct ioapic_chip {
    uint8_t           id;            /* informational, from MADT */
    uint32_t          gsi_base;
    uint32_t          gsi_count;     /* (max_redir_entry + 1) */
    volatile uint8_t *mmio;          /* kernel-virt pointer */
};

#define MAX_CHIPS 4

static struct ioapic_chip g_chips[MAX_CHIPS];
static uint32_t           g_chip_count;
static bool               g_active;

/* Each IO APIC gets its own kernel-virt page. We pick increasing slots
 * inside a private 1 MiB window so the layout is deterministic. */
#define IOAPIC_KVIRT_BASE  0xffffd20000010000ULL
#define IOAPIC_KVIRT_STRIDE 0x1000ULL

bool ioapic_active(void) { return g_active; }

/* IOREGSEL selects the indirect register; IOWIN does the data transfer. */
static uint32_t reg_read(struct ioapic_chip *c, uint32_t reg) {
    *(volatile uint32_t *)(c->mmio + 0x00) = reg;
    return *(volatile uint32_t *)(c->mmio + 0x10);
}
static void reg_write(struct ioapic_chip *c, uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(c->mmio + 0x00) = reg;
    *(volatile uint32_t *)(c->mmio + 0x10) = val;
}

/* Find which chip owns this GSI. NULL if none. */
static struct ioapic_chip *chip_for_gsi(uint32_t gsi) {
    for (uint32_t i = 0; i < g_chip_count; i++) {
        struct ioapic_chip *c = &g_chips[i];
        if (gsi >= c->gsi_base && gsi < c->gsi_base + c->gsi_count) {
            return c;
        }
    }
    return 0;
}

bool ioapic_init(void) {
    g_chip_count = 0;
    g_active     = false;

    const struct acpi_info *info = acpi_get();
    if (!info || !info->ok || info->ioapic_count == 0) {
        kprintf("[ioapic] no IO APIC in MADT -- staying on legacy PIC\n");
        return false;
    }

    for (uint32_t i = 0; i < info->ioapic_count; i++) {
        if (g_chip_count >= MAX_CHIPS) break;

        const struct acpi_ioapic_entry *e = &info->ioapics[i];
        uint64_t phys = e->mmio_phys & ~((uint64_t)PAGE_SIZE - 1u);
        uint64_t virt = IOAPIC_KVIRT_BASE +
                        (uint64_t)g_chip_count * IOAPIC_KVIRT_STRIDE;

        if (!vmm_map(virt, phys, PAGE_SIZE,
                     VMM_PRESENT | VMM_WRITE | VMM_NX | VMM_NOCACHE)) {
            kprintf("[ioapic] vmm_map for IOAPIC%u (phys=%p) failed\n",
                    (unsigned)e->id, (void *)phys);
            continue;
        }

        struct ioapic_chip *c = &g_chips[g_chip_count++];
        c->id       = e->id;
        c->gsi_base = e->gsi_base;
        c->mmio     = (volatile uint8_t *)virt;

        uint32_t ver = reg_read(c, IOAPIC_REG_VER);
        c->gsi_count = ((ver >> 16) & 0xFFu) + 1u;

        /* Mask every redirection entry so no stray IRQ fires before a
         * handler is registered. The (vector=0, masked=1, edge, high)
         * pattern is the safest "off" state. */
        for (uint32_t r = 0; r < c->gsi_count; r++) {
            reg_write(c, IOAPIC_REG_REDTBL(r) + 0u, IOAPIC_REDIR_MASKED);
            reg_write(c, IOAPIC_REG_REDTBL(r) + 1u, 0);
        }

        kprintf("[ioapic] chip %u: phys=%p virt=%p gsi=[%u..%u] "
                "ver=0x%x entries=%u\n",
                (unsigned)c->id, (void *)phys, (void *)virt,
                (unsigned)c->gsi_base,
                (unsigned)(c->gsi_base + c->gsi_count - 1),
                (unsigned)(ver & 0xFFu),
                (unsigned)c->gsi_count);
    }

    g_active = (g_chip_count > 0);
    return g_active;
}

struct ioapic_isa_route ioapic_resolve_isa(uint8_t isa_irq) {
    /* ISA default: identity mapping, edge-triggered, active-high. */
    struct ioapic_isa_route r = {
        .gsi        = (uint32_t)isa_irq,
        .level      = false,
        .active_low = false,
    };

    const struct acpi_info *info = acpi_get();
    if (!info || !info->ok) return r;

    for (uint32_t i = 0; i < info->iso_count; i++) {
        const struct acpi_iso_entry *e = &info->isos[i];
        if (e->isa_irq != isa_irq) continue;
        r.gsi = e->gsi;

        uint16_t pol = e->flags & ACPI_MPS_POLARITY_MASK;
        uint16_t trg = e->flags & ACPI_MPS_TRIGGER_MASK;

        /* "Bus default" for ISA = active-high edge. */
        r.active_low = (pol == ACPI_MPS_POLARITY_LOW);
        r.level      = (trg == ACPI_MPS_TRIGGER_LEVEL);
        return r;
    }
    return r;
}

bool ioapic_route(uint32_t gsi, uint8_t vector, uint8_t lapic_id,
                  bool level, bool active_low) {
    struct ioapic_chip *c = chip_for_gsi(gsi);
    if (!c) {
        kprintf("[ioapic] route: GSI %u out of range\n", (unsigned)gsi);
        return false;
    }
    uint32_t entry = gsi - c->gsi_base;

    uint32_t lo = IOAPIC_REDIR_VECTOR(vector)
                | IOAPIC_REDIR_DELIVERY_FIXED
                | IOAPIC_REDIR_DEST_PHYSICAL;
    if (active_low) lo |= IOAPIC_REDIR_POLARITY_LOW;
    if (level)      lo |= IOAPIC_REDIR_TRIGGER_LEVEL;
    /* leave the masked bit clear -- caller wants this active */

    uint32_t hi = (uint32_t)lapic_id << 24;

    /* Per the datasheet write the high half FIRST: the chip latches
     * destination on the low-half write, so a stale high half could
     * deliver one IRQ to the wrong CPU. */
    reg_write(c, IOAPIC_REG_REDTBL(entry) + 1u, hi);
    reg_write(c, IOAPIC_REG_REDTBL(entry) + 0u, lo);
    return true;
}

bool ioapic_mask(uint32_t gsi, bool masked) {
    struct ioapic_chip *c = chip_for_gsi(gsi);
    if (!c) return false;
    uint32_t entry = gsi - c->gsi_base;
    uint32_t lo    = reg_read(c, IOAPIC_REG_REDTBL(entry) + 0u);
    if (masked) lo |=  IOAPIC_REDIR_MASKED;
    else        lo &= ~IOAPIC_REDIR_MASKED;
    reg_write(c, IOAPIC_REG_REDTBL(entry) + 0u, lo);
    return true;
}
