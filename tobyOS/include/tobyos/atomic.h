/* atomic.h -- Atomic operations for SMP-safe programming.
 *
 * Provides compiler-intrinsic atomic operations and memory barriers
 * for lock-free data structures and SMP synchronization.
 */

#ifndef TOBYOS_ATOMIC_H
#define TOBYOS_ATOMIC_H

#include <tobyos/types.h>

/* ---- Memory barriers ---- */

static inline void atomic_mb(void)  { __asm__ volatile("mfence" ::: "memory"); }
static inline void atomic_rmb(void) { __asm__ volatile("lfence" ::: "memory"); }
static inline void atomic_wmb(void) { __asm__ volatile("sfence" ::: "memory"); }
static inline void compiler_barrier(void) { __asm__ volatile("" ::: "memory"); }

/* ---- Atomic load/store ---- */

static inline int32_t atomic_load32(const volatile int32_t *ptr) {
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

static inline void atomic_store32(volatile int32_t *ptr, int32_t val) {
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

static inline int64_t atomic_load64(const volatile int64_t *ptr) {
    return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

static inline void atomic_store64(volatile int64_t *ptr, int64_t val) {
    __atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

/* ---- Atomic arithmetic ---- */

static inline int32_t atomic_add32(volatile int32_t *ptr, int32_t val) {
    return __atomic_fetch_add(ptr, val, __ATOMIC_ACQ_REL);
}

static inline int64_t atomic_add64(volatile int64_t *ptr, int64_t val) {
    return __atomic_fetch_add(ptr, val, __ATOMIC_ACQ_REL);
}

static inline int32_t atomic_sub32(volatile int32_t *ptr, int32_t val) {
    return __atomic_fetch_sub(ptr, val, __ATOMIC_ACQ_REL);
}

static inline uint32_t atomic_inc32(volatile uint32_t *ptr) {
    return __atomic_fetch_add(ptr, 1, __ATOMIC_ACQ_REL);
}

static inline uint32_t atomic_dec32(volatile uint32_t *ptr) {
    return __atomic_fetch_sub(ptr, 1, __ATOMIC_ACQ_REL);
}

/* ---- Compare-and-swap ---- */

static inline int atomic_cmpxchg32(volatile int32_t *ptr, int32_t expected, int32_t desired) {
    return __atomic_compare_exchange_n(ptr, &expected, desired, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline int atomic_cmpxchg64(volatile int64_t *ptr, int64_t expected, int64_t desired) {
    return __atomic_compare_exchange_n(ptr, &expected, desired, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static inline int atomic_cmpxchg_ptr(volatile void **ptr, void *expected, void *desired) {
    return __atomic_compare_exchange_n(ptr, &expected, desired, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* ---- Atomic exchange ---- */

static inline int32_t atomic_xchg32(volatile int32_t *ptr, int32_t val) {
    return __atomic_exchange_n(ptr, val, __ATOMIC_ACQ_REL);
}

static inline void *atomic_xchg_ptr(volatile void **ptr, void *val) {
    return __atomic_exchange_n(ptr, val, __ATOMIC_ACQ_REL);
}

/* ---- Atomic bitwise ---- */

static inline int32_t atomic_or32(volatile int32_t *ptr, int32_t val) {
    return __atomic_fetch_or(ptr, val, __ATOMIC_ACQ_REL);
}

static inline int32_t atomic_and32(volatile int32_t *ptr, int32_t val) {
    return __atomic_fetch_and(ptr, val, __ATOMIC_ACQ_REL);
}

#endif /* TOBYOS_ATOMIC_H */
