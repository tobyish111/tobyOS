/* spinlock.h -- minimum-viable test-and-set spinlock.
 *
 * Just enough to keep the BSP and APs from interleaving kprintf or
 * stomping on a small shared "ap_param" page during AP bring-up. We
 * disable IRQs around the critical section so that an IRQ landing in
 * the middle can't deadlock by trying to re-take the lock.
 *
 * Not a ticket lock, no fairness, no priority boost -- this is a
 * teaching kernel.
 *
 * Usage:
 *
 *   static spinlock_t g_console_lock = SPINLOCK_INIT;
 *
 *   uint64_t flags = spin_lock_irqsave(&g_console_lock);
 *   kprintf("CPU %u online\n", id);
 *   spin_unlock_irqrestore(&g_console_lock, flags);
 */

#ifndef TOBYOS_SPINLOCK_H
#define TOBYOS_SPINLOCK_H

#include <tobyos/types.h>
#include <tobyos/cpu.h>

typedef struct {
    volatile uint32_t locked;   /* 0 = free, 1 = held */
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_init(spinlock_t *l) { l->locked = 0; }

static inline void spin_lock(spinlock_t *l) {
    /* xchg returns the previous value and writes 1 atomically -- as long
     * as we keep doing it, we'll eventually see 0 (= we got the lock). */
    while (__atomic_exchange_n(&l->locked, 1u, __ATOMIC_ACQUIRE) != 0u) {
        /* Spin gently: PAUSE is "rep nop" -- a hint to the CPU that we're
         * in a busy wait, useful on hyper-threaded hardware. */
        __asm__ volatile ("pause" ::: "memory");
    }
}

static inline void spin_unlock(spinlock_t *l) {
    __atomic_store_n(&l->locked, 0u, __ATOMIC_RELEASE);
}

static inline uint64_t spin_lock_irqsave(spinlock_t *l) {
    uint64_t flags = read_rflags();
    cli();
    spin_lock(l);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags) {
    spin_unlock(l);
    if (flags & (1ULL << 9)) sti();   /* IF was set on entry -- re-enable */
}

#endif /* TOBYOS_SPINLOCK_H */
