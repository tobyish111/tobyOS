/* selftest.h -- Kernel self-test framework (Phase 8). */

#ifndef TOBYOS_SELFTEST_H
#define TOBYOS_SELFTEST_H

#define SELFTEST_PASS  0
#define SELFTEST_FAIL  1

typedef int (*selftest_fn)(void);

struct selftest {
    const char *name;
    selftest_fn func;
};

/* Run all registered self-tests. Returns the count of failures. */
int selftest_run_all(void);

/* Individual subsystem tests */
int selftest_pmm(void);        /* physical memory allocator */
int selftest_kheap(void);      /* kernel heap allocator */
int selftest_sched(void);      /* scheduler (create tasks, verify) */
int selftest_spinlock(void);   /* concurrent lock correctness */
int selftest_vmm(void);        /* virtual memory map/unmap/remap */

#endif /* TOBYOS_SELFTEST_H */
