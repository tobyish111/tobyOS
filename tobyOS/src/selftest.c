/* src/selftest.c -- Kernel self-test suite (Phase 8).
 *
 * Each test allocates/exercises a subsystem and returns SELFTEST_PASS
 * or SELFTEST_FAIL. Run from kmain when -DSELFTEST is defined.
 */

#include <tobyos/selftest.h>
#include <tobyos/pmm.h>
#include <tobyos/vmm.h>
#include <tobyos/heap.h>
#include <tobyos/spinlock.h>
#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>

static const struct selftest g_tests[] = {
    { "pmm",      selftest_pmm },
    { "kheap",    selftest_kheap },
    { "vmm",      selftest_vmm },
    { "spinlock", selftest_spinlock },
    { "sched",    selftest_sched },
    { 0, 0 }
};

int selftest_run_all(void) {
    int passed = 0, failed = 0;
    kprintf("[selftest] running kernel self-tests...\n");

    for (int i = 0; g_tests[i].name; i++) {
        kprintf("[selftest] %s ... ", g_tests[i].name);
        int result = g_tests[i].func();
        if (result == SELFTEST_PASS) {
            kprintf("PASS\n");
            passed++;
        } else {
            kprintf("FAIL\n");
            failed++;
        }
    }

    kprintf("[selftest] results: %d passed, %d failed\n", passed, failed);
    return failed;
}

/* ---- PMM test ---- */

int selftest_pmm(void) {
    /* Allocate several pages, verify they're distinct, free them */
    uint64_t pages[16];
    for (int i = 0; i < 16; i++) {
        pages[i] = pmm_alloc_page();
        if (!pages[i]) return SELFTEST_FAIL;
        /* Verify no duplicates */
        for (int j = 0; j < i; j++) {
            if (pages[j] == pages[i]) return SELFTEST_FAIL;
        }
    }
    /* Free in reverse order */
    for (int i = 15; i >= 0; i--) {
        pmm_free_page(pages[i]);
    }
    /* Re-allocate and verify we get pages back */
    uint64_t p = pmm_alloc_page();
    if (!p) return SELFTEST_FAIL;
    pmm_free_page(p);

    return SELFTEST_PASS;
}

/* ---- Kernel heap test ---- */

int selftest_kheap(void) {
    /* Basic alloc/free cycles */
    void *ptrs[32];
    for (int i = 0; i < 32; i++) {
        size_t sz = 16 + i * 64;
        ptrs[i] = kmalloc(sz);
        if (!ptrs[i]) return SELFTEST_FAIL;
        memset(ptrs[i], 0xAA, sz);
    }

    /* Verify patterns intact */
    for (int i = 0; i < 32; i++) {
        size_t sz = 16 + i * 64;
        unsigned char *p = (unsigned char *)ptrs[i];
        for (size_t b = 0; b < sz; b++) {
            if (p[b] != 0xAA) return SELFTEST_FAIL;
        }
    }

    /* Free odd indices */
    for (int i = 1; i < 32; i += 2) {
        kfree(ptrs[i]);
        ptrs[i] = 0;
    }

    /* Reallocate into freed slots */
    for (int i = 1; i < 32; i += 2) {
        ptrs[i] = kmalloc(128);
        if (!ptrs[i]) return SELFTEST_FAIL;
        memset(ptrs[i], 0xBB, 128);
    }

    /* Verify remaining even-index patterns */
    for (int i = 0; i < 32; i += 2) {
        size_t sz = 16 + i * 64;
        unsigned char *p = (unsigned char *)ptrs[i];
        for (size_t b = 0; b < sz; b++) {
            if (p[b] != 0xAA) return SELFTEST_FAIL;
        }
    }

    /* Free all */
    for (int i = 0; i < 32; i++) {
        kfree(ptrs[i]);
    }

    return SELFTEST_PASS;
}

/* ---- VMM test ---- */

int selftest_vmm(void) {
    /* Map a page, write, verify, unmap */
    uint64_t phys = pmm_alloc_page();
    if (!phys) return SELFTEST_FAIL;

    uint64_t test_virt = 0xFFFFD20000000000ULL;
    bool rc = vmm_map(test_virt, phys, PAGE_SIZE, VMM_PRESENT | VMM_WRITE | VMM_NX);
    if (!rc) {
        pmm_free_page(phys);
        return SELFTEST_FAIL;
    }

    volatile uint64_t *p = (volatile uint64_t *)test_virt;
    *p = 0xDEADCAFEBEEF1234ULL;
    if (*p != 0xDEADCAFEBEEF1234ULL) {
        vmm_unmap(test_virt, PAGE_SIZE);
        pmm_free_page(phys);
        return SELFTEST_FAIL;
    }

    vmm_unmap(test_virt, PAGE_SIZE);
    pmm_free_page(phys);
    return SELFTEST_PASS;
}

/* ---- Spinlock test ---- */

int selftest_spinlock(void) {
    spinlock_t lock;
    spin_init(&lock);

    /* Basic lock/unlock */
    spin_lock(&lock);
    spin_unlock(&lock);

    /* Re-acquire after release */
    spin_lock(&lock);
    spin_unlock(&lock);

    return SELFTEST_PASS;
}

/* ---- Scheduler test ---- */

int selftest_sched(void) {
    /* Verify the scheduler is initialized and current process exists */
    struct proc *cur = current_proc();
    if (!cur) return SELFTEST_FAIL;
    if (cur->pid < 0) return SELFTEST_FAIL;

    return SELFTEST_PASS;
}
