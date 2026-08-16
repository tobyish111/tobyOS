/* aslr.c -- Address Space Layout Randomization (Phase 7 M7.1).
 *
 * WHAT IS ACTUALLY WIRED, as of the mmap-ASLR change:
 *
 *   mmap region  RANDOMISED. src/mmap.c draws its per-address-space base from
 *                aslr_mmap_offset() (find_free_region, on the hint == 0
 *                "fresh address space" path). This is the one that matters
 *                most: every shared library, every anonymous mapping and
 *                every JIT region a dynamic binary allocates lands through
 *                that allocator.
 *   user stack   NOT randomised. The stack tops out at the fixed
 *                USER_STACK_TOP_LIMIT_VA, and page_fault.c decides whether a
 *                fault is stack growth by comparing against the equally fixed
 *                USER_STACK_FLOOR_VA. Moving the top per-process means making
 *                that floor per-process too -- a change to the fault path,
 *                which is not something to do without its own gate.
 *   exe base     NOT randomised. tobyOS's own programs are -fno-pie, and the
 *                PIE/ET_DYN load bases in fork.c are fixed constants that the
 *                Chromium arc's V8 cage layout is sensitive to.
 *
 * This header used to claim all three, while NOTHING consumed any of the
 * three offset functions -- aslr_init() printed an "ASLR enabled" banner at
 * every boot and that was the entire feature. Two of the three are still
 * unwired; they are described as unwired rather than advertised.
 *
 * Uses the kernel's RNG subsystem for entropy (rng_fill self-seeds on first
 * use, so these are safe to call at any point after boot).
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
    /* Report only what is wired. The old banner named all three regions and
     * randomised none of them -- a boot line that read as a security feature
     * and was a print statement. */
    kprintf("[aslr] mmap base randomised (%d bits); stack/exe base fixed "
            "(not wired)\n", ASLR_MMAP_BITS);
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
