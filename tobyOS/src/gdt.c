/* gdt.c -- install our 64-bit GDT and reload all segment registers.
 *
 * Each entry is encoded as a 64-bit value matching the legacy 8-byte
 * descriptor layout. In long mode the CPU mostly ignores base / limit,
 * but the access byte and the L (long-mode code) flag are still honoured.
 *
 * Encoding cheat-sheet (low-to-high bytes):
 *
 *   limit_low  : 16  | base_low : 16 | base_mid : 8 | access : 8
 *   limit_hi:4 + flags:4 (G,D/B,L,AVL)         | base_hi  : 8
 *
 *   access byte: P=1 DPL S Type
 *     0x9A = present, DPL 0, code segment, executable + readable
 *     0x92 = present, DPL 0, data segment, writable
 *     0xFA = present, DPL 3, code (executable + readable)
 *     0xF2 = present, DPL 3, data (writable)
 *     0x89 = present, DPL 0, system segment, type=9 (available 64-bit TSS)
 *
 *   flags nibble (upper 4 bits of byte 6):
 *     0xA  = G=1 (4 KiB granularity), L=1 (long-mode code)
 *     0xC  = G=1, D/B=1 (32-bit op size; ignored for data in long mode)
 *
 * Slot order is fixed by SYSCALL/SYSRET (see gdt.h):
 *
 *   1: kernel CS  (0x08)
 *   2: kernel DS  (0x10)
 *   3: user   DS  (0x1B)   <- user-data BEFORE user-code is mandatory
 *   4: user   CS  (0x23)
 *   5: TSS lo (8 bytes)    <- patched by gdt_install_tss
 *   6: TSS hi (8 bytes)    <- "
 *
 * Reloading CS in long mode: there's no `ljmp imm64`, so we synthesise a
 * far return -- push the new CS, push the return RIP, and execute lretq.
 * The label `1f` resolves to the next instruction after the lretq, so
 * execution resumes there with CS = GDT_KERNEL_CS.
 */

#include <tobyos/gdt.h>
#include <tobyos/printk.h>

#define GDT_NULL        0x0000000000000000ULL
#define GDT_KCODE       0x00AF9A000000FFFFULL  /* kernel code: L=1, DPL=0 */
#define GDT_KDATA       0x00CF92000000FFFFULL  /* kernel data: DPL=0      */
#define GDT_UDATA       0x00CFF2000000FFFFULL  /* user   data: DPL=3      */
#define GDT_UCODE       0x00AFFA000000FFFFULL  /* user   code: L=1, DPL=3 */

/* 7 slots: null + kcs + kds + uds + ucs + tss_lo + tss_hi.
 * The TSS descriptor is two 8-byte entries because long-mode system
 * descriptors are 128 bits wide (it stores a 64-bit base address). */
#define GDT_ENTRY_COUNT 7

static uint64_t g_gdt[GDT_ENTRY_COUNT] __attribute__((aligned(16)));

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct gdtr g_gdtr;

const void *gdt_pseudo_descriptor(void) { return &g_gdtr; }

uint16_t cpu_read_cs(void) {
    uint16_t v;
    __asm__ volatile ("mov %%cs, %0" : "=r"(v));
    return v;
}

uint16_t cpu_read_ds(void) {
    uint16_t v;
    __asm__ volatile ("mov %%ds, %0" : "=r"(v));
    return v;
}

void gdt_install_tss(uint64_t tss_base, uint32_t tss_limit) {
    /* Long-mode 64-bit TSS descriptor (16 bytes). Layout:
     *
     *   bytes 0..1 : limit[15:0]
     *   bytes 2..3 : base[15:0]
     *   byte  4    : base[23:16]
     *   byte  5    : access (0x89 = present, DPL=0, type=9 64-bit TSS)
     *   byte  6    : limit[19:16] (low 4) | flags (high 4) -- flags = 0
     *   byte  7    : base[31:24]
     *   bytes 8..11: base[63:32]
     *   bytes 12..15: reserved (zero)
     */
    uint64_t lo = 0;
    lo |= (tss_limit       & 0xFFFFULL);
    lo |= ((tss_base       & 0xFFFFULL)        << 16);
    lo |= (((tss_base >> 16) & 0xFFULL)        << 32);
    lo |= ((uint64_t)0x89                       << 40);
    lo |= (((uint64_t)tss_limit >> 16) & 0xFULL) << 48;
    lo |= (((tss_base >> 24) & 0xFFULL)         << 56);

    uint64_t hi = (tss_base >> 32) & 0xFFFFFFFFULL;

    g_gdt[5] = lo;
    g_gdt[6] = hi;
}

void gdt_init(void) {
    g_gdt[0] = GDT_NULL;
    g_gdt[1] = GDT_KCODE;
    g_gdt[2] = GDT_KDATA;
    g_gdt[3] = GDT_UDATA;       /* user data MUST be slot 3 (SYSRET wired) */
    g_gdt[4] = GDT_UCODE;
    g_gdt[5] = 0;               /* TSS lo -- patched by gdt_install_tss */
    g_gdt[6] = 0;               /* TSS hi */

    g_gdtr.limit = (uint16_t)(sizeof(g_gdt) - 1);
    g_gdtr.base  = (uint64_t)&g_gdt[0];

    /* Load the new GDT, then perform a far return to reload CS. After
     * lretq, execution continues at label `1` with CS = 0x08 and all
     * data segments set to 0x10. Marked memory clobber so the compiler
     * doesn't reorder anything around the segment switch. */
    __asm__ volatile (
        "lgdt %[gdtr]                \n\t"
        "pushq $%c[kcs]              \n\t"   /* new CS selector */
        "leaq 1f(%%rip), %%rax       \n\t"   /* return RIP      */
        "pushq %%rax                 \n\t"
        "lretq                       \n\t"
        "1:                          \n\t"
        "movw $%c[kds], %%ax         \n\t"
        "movw %%ax, %%ds             \n\t"
        "movw %%ax, %%es             \n\t"
        "movw %%ax, %%fs             \n\t"
        "movw %%ax, %%gs             \n\t"
        "movw %%ax, %%ss             \n\t"
        :
        : [gdtr] "m"(g_gdtr),
          [kcs]  "i"(GDT_KERNEL_CS),
          [kds]  "i"(GDT_KERNEL_DS)
        : "rax", "memory"
    );

    kprintf("[gdt] loaded: base=%p limit=%u entries=%u\n",
            (void *)g_gdtr.base,
            (unsigned)g_gdtr.limit,
            (unsigned)GDT_ENTRY_COUNT);
    kprintf("[gdt] CS=0x%02x DS=0x%02x (expected CS=0x%02x DS=0x%02x)\n",
            cpu_read_cs(), cpu_read_ds(),
            GDT_KERNEL_CS, GDT_KERNEL_DS);
    kprintf("[gdt] user CS=0x%02x DS=0x%02x  TSS sel=0x%02x (TSS slot empty until tss_init)\n",
            GDT_USER_CS, GDT_USER_DS, GDT_TSS_SEL);
}
