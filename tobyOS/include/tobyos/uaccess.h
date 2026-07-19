/* uaccess.h -- SMAP user-memory access helpers (per-copy model).
 *
 * When CR4.SMAP is enabled, any supervisor-mode (ring 0) access to a
 * user-accessible page faults (#PF) unless RFLAGS.AC is set. The kernel
 * toggles AC with the SMAP instructions stac (set) / clac (clear).
 *
 * Model used by tobyOS (Linux-style per-copy accessors as of 2026-06-12;
 * the previous whole-syscall-body stac window is GONE):
 *
 *   - Kernel code never dereferences a user pointer directly. Every copy
 *     in or out of user memory goes through copy_from_user/copy_to_user/
 *     strncpy_from_user/clear_user below, which validate the range and
 *     bracket only the actual copy in a stac/clac window. Large I/O
 *     (read/write/send/recv) bounces through kernel buffers at the syscall
 *     boundary, so the VFS/pipe/net stacks never see user pointers at all.
 *
 *   - The few non-copy paths that legitimately walk user memory (the ELF
 *     loader's segment writes, argv/envp stack packing, the futex word
 *     re-read) bracket the access with uaccess_begin()/uaccess_end(),
 *     which SAVE and RESTORE the prior AC state so they nest correctly.
 *
 * A page fault INSIDE a window (CoW, demand-zero, swapped) resolves through
 * the same page-fault path a ring-3 access would (isr.c routes kernel-mode
 * faults on user-half addresses to the COW/demand handler). A stray user
 * deref anywhere else in the kernel now faults fatally under SMAP -- which
 * is the point.
 *
 * CRITICAL: stac/clac #UD on CPUs that don't implement SMAP. The default
 * QEMU CPU has no SMAP, so we gate every stac/clac on g_smap_on (set by
 * hardening_init only when CR4.SMAP was actually enabled). When SMAP is off
 * the accessors still validate ranges; only the AC toggling is skipped. */

#ifndef TOBYOS_UACCESS_H
#define TOBYOS_UACCESS_H

#include <tobyos/types.h>
#include <tobyos/klibc.h>      /* memcpy/memset for the copy accessors */

/* 1 once CR4.SMAP is live; 0 otherwise. Defined in hardening.c. Also read
 * from syscall_entry.S, so keep it a plain byte. */
extern volatile uint8_t g_smap_on;

static inline void uaccess_stac(void) {
    if (g_smap_on) __asm__ volatile("stac" ::: "cc", "memory");
}
static inline void uaccess_clac(void) {
    if (g_smap_on) __asm__ volatile("clac" ::: "cc", "memory");
}

/* Open a uaccess window, returning the prior RFLAGS for uaccess_end().
 * pushfq/popfq are valid on every CPU; only the stac is SMAP-gated. */
static inline unsigned long uaccess_begin(void) {
    unsigned long flags;
    __asm__ volatile("pushfq\n\t"
                     "popq %0"
                     : "=r"(flags) :: "memory");
    uaccess_stac();
    return flags;
}

/* Restore the RFLAGS saved by uaccess_begin (clearing AC iff it was clear),
 * which correctly closes a nested window without disturbing an outer one. */
static inline void uaccess_end(unsigned long flags) {
    __asm__ volatile("pushq %0\n\t"
                     "popfq"
                     :: "r"(flags) : "cc", "memory");
}

/* ==== Per-copy accessors (the Linux copy_*_user model) ====================
 *
 * Each call validates the user range (user half, no wrap) and brackets ONLY
 * the actual copy in a stac/clac window -- the syscall body no longer runs
 * with a giant open window, so a stray kernel deref of a user pointer
 * anywhere else now faults under SMAP instead of silently succeeding.
 *
 * Return convention: 0 on success, -1 on a bad user range (callers map that
 * to -ABI_EFAULT). strncpy_from_user returns the copied length (excluding
 * the NUL) or -1. A "bad range" is a range check, not a probe: a valid-half
 * pointer to an unmapped page still #PFs inside the window, where the
 * CoW/demand-paging fault path resolves it exactly as a ring-3 access would.
 */

#define UACCESS_USER_END 0x0000800000000000ULL

static inline bool user_range_ok(uint64_t addr, size_t len) {
    if (len == 0) return true;
    if (addr >= UACCESS_USER_END)       return false;
    if (addr + len > UACCESS_USER_END)  return false;
    if (addr + len < addr)              return false;   /* wrap */
    return true;
}

static inline int copy_from_user(void *dst, const void *user_src, size_t n) {
    if (!user_range_ok((uint64_t)(uintptr_t)user_src, n)) return -1;
    if (n == 0) return 0;
    unsigned long f = uaccess_begin();
    memcpy(dst, user_src, n);
    uaccess_end(f);
    return 0;
}

/* Make [addr,addr+len) present + writable (demand-page/COW) or report failure;
 * defined in page_fault.c. Lets the write accessors return -EFAULT on a read-only
 * or unmapped user buffer instead of the kernel #PF'ing mid-copy (-> panic). */
int uaccess_prepare_write(uint64_t addr, uint64_t len);

static inline int copy_to_user(void *user_dst, const void *src, size_t n) {
    if (!user_range_ok((uint64_t)(uintptr_t)user_dst, n)) return -1;
    if (n == 0) return 0;
    if (uaccess_prepare_write((uint64_t)(uintptr_t)user_dst, n) != 0) return -1;
    unsigned long f = uaccess_begin();
    memcpy(user_dst, src, n);
    uaccess_end(f);
    return 0;
}

static inline int clear_user(void *user_dst, size_t n) {
    if (!user_range_ok((uint64_t)(uintptr_t)user_dst, n)) return -1;
    if (n == 0) return 0;
    if (uaccess_prepare_write((uint64_t)(uintptr_t)user_dst, n) != 0) return -1;
    unsigned long f = uaccess_begin();
    memset(user_dst, 0, n);
    uaccess_end(f);
    return 0;
}

/* Copy a NUL-terminated user string into dst (cap >= 1). Always NUL-
 * terminates dst. Stops at cap-1 bytes (silent truncation, matching the
 * kstrdup-style copies this replaces). Never reads past the user half. */
static inline long strncpy_from_user(char *dst, const void *user_src,
                                     size_t cap) {
    uint64_t ua = (uint64_t)(uintptr_t)user_src;
    if (cap == 0) return -1;
    if (ua >= UACCESS_USER_END) return -1;
    size_t max = cap - 1;
    uint64_t room = UACCESS_USER_END - ua;       /* bytes until user-half end */
    if (max > room) max = (size_t)room;
    const char *s = (const char *)user_src;
    size_t i = 0;
    unsigned long f = uaccess_begin();
    for (; i < max; i++) {
        char c = s[i];
        dst[i] = c;
        if (c == '\0') break;
    }
    uaccess_end(f);
    if (i == max) dst[i] = '\0';                 /* truncated / exact fit */
    return (long)((i < max) ? i : max);
}

/* Small scalar helpers for single-value out-params. */
static inline int put_user_u32(void *user_dst, uint32_t v) {
    return copy_to_user(user_dst, &v, sizeof(v));
}
static inline int put_user_u64(void *user_dst, uint64_t v) {
    return copy_to_user(user_dst, &v, sizeof(v));
}
static inline int get_user_u64(uint64_t *out, const void *user_src) {
    return copy_from_user(out, user_src, sizeof(*out));
}

#endif /* TOBYOS_UACCESS_H */
