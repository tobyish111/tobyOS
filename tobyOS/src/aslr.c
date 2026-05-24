/* aslr.c -- Address Space Layout Randomization (Phase 7 M7.1).
 *
 * Randomizes the base addresses of:
 *   - User stack (within a range near the top of user space)
 *   - Heap (mmap base address)
 *   - Program load address (for PIE binaries, future)
 *
 * Uses the kernel's RNG subsystem for entropy. Applied during
 * process creation in spawn_internal.
 */

#include <tobyos/proc.h>
#include <tobyos/vmm.h>
#include <tobyos/types.h>
#include <tobyos/printk.h>

/* ASLR entropy: number of random bits for each region.
 * More bits = more randomization but more virtual address space usage. */
#define ASLR_STACK_BITS   16  /* 16 bits -> 64K pages -> 256MB range */
#define ASLR_MMAP_BITS    20  /* 20 bits -> 1M pages -> 4GB range */
#define ASLR_EXE_BITS     12  /* 12 bits -> 4K pages -> 16MB range (PIE only) */

static int g_aslr_enabled = 1;

void aslr_init(void) {
    kprintf("[aslr] ASLR enabled (stack=%d bits, mmap=%d bits, exe=%d bits)\n",
            ASLR_STACK_BITS, ASLR_MMAP_BITS, ASLR_EXE_BITS);
}

void aslr_set_enabled(int enabled) {
    g_aslr_enabled = enabled;
}

int aslr_is_enabled(void) {
    return g_aslr_enabled;
}

/* Generate a random offset aligned to page boundaries.
 * Returns a value in [0, (1 << bits) * PAGE_SIZE). */
static uint64_t aslr_offset(int bits) {
    if (!g_aslr_enabled || bits <= 0) return 0;

    uint32_t r = 0;
    extern void rng_fill(void *buf, size_t n);
    rng_fill(&r, sizeof(r));

    uint64_t mask = ((uint64_t)1 << bits) - 1;
    return (r & mask) * 4096ULL;
}

/* Randomize stack base. Called from spawn_internal to offset the user
 * stack within a range below the canonical user-space ceiling. */
uint64_t aslr_stack_offset(void) {
    return aslr_offset(ASLR_STACK_BITS);
}

/* Randomize mmap base. Applied when initializing the per-process
 * VMA table's hint address. */
uint64_t aslr_mmap_offset(void) {
    return aslr_offset(ASLR_MMAP_BITS);
}

/* Randomize executable base for PIE binaries. */
uint64_t aslr_exe_offset(void) {
    return aslr_offset(ASLR_EXE_BITS);
}
