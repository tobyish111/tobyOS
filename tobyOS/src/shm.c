/* shm.c -- POSIX shared memory (Phase 1 M1.4).
 *
 * Implements shm_open/shm_unlink semantics: named shared memory objects
 * that multiple processes can mmap into their address spaces. Backed by
 * physical pages allocated on first creation.
 *
 * Kernel-side: an shm object is a named allocation of physical pages.
 * User-side: processes obtain an fd via SYS_SHM_OPEN, then mmap it.
 */

#include <tobyos/proc.h>
#include <tobyos/vmm.h>
#include <tobyos/pmm.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/spinlock.h>

#define SHM_MAX        32
#define SHM_NAME_MAX   64

struct shm_object {
    char     name[SHM_NAME_MAX];
    uint64_t phys_pages[64];  /* physical page addresses */
    size_t   n_pages;
    size_t   size;
    int      refcount;
    bool     active;
};

static struct shm_object g_shm[SHM_MAX];
static spinlock_t g_shm_lock = SPINLOCK_INIT;

void shm_init(void) {
    memset(g_shm, 0, sizeof(g_shm));
    kprintf("[shm] shared memory subsystem ready (%d slots)\n", SHM_MAX);
}

static struct shm_object *shm_find(const char *name) {
    for (int i = 0; i < SHM_MAX; i++) {
        if (g_shm[i].active && strcmp(g_shm[i].name, name) == 0)
            return &g_shm[i];
    }
    return 0;
}

static struct shm_object *shm_alloc_slot(void) {
    for (int i = 0; i < SHM_MAX; i++) {
        if (!g_shm[i].active) return &g_shm[i];
    }
    return 0;
}

/* Create or open a shared memory object. Returns an "shm id" (index)
 * or negative error. */
long sys_shm_open(const char *name, int flags, size_t size) {
    if (!name || !name[0]) return -22; /* EINVAL */

    uint64_t f = spin_lock_irqsave(&g_shm_lock);

    struct shm_object *shm = shm_find(name);
    if (shm) {
        shm->refcount++;
        int id = (int)(shm - g_shm);
        spin_unlock_irqrestore(&g_shm_lock, f);
        return id;
    }

    /* Create new */
    if (size == 0) {
        spin_unlock_irqrestore(&g_shm_lock, f);
        return -22;
    }

    shm = shm_alloc_slot();
    if (!shm) {
        spin_unlock_irqrestore(&g_shm_lock, f);
        return -12; /* ENOMEM */
    }

    size_t n_pages = (size + 4095) / 4096;
    if (n_pages > 64) {
        spin_unlock_irqrestore(&g_shm_lock, f);
        return -22; /* too large */
    }

    /* Allocate physical pages */
    for (size_t i = 0; i < n_pages; i++) {
        shm->phys_pages[i] = pmm_alloc_page();
        if (!shm->phys_pages[i]) {
            for (size_t j = 0; j < i; j++)
                pmm_free_page(shm->phys_pages[j]);
            spin_unlock_irqrestore(&g_shm_lock, f);
            return -12;
        }
        memset((void *)(shm->phys_pages[i] + vmm_hhdm_offset()), 0, 4096);
    }

    size_t nlen = strlen(name);
    if (nlen >= SHM_NAME_MAX) nlen = SHM_NAME_MAX - 1;
    memcpy(shm->name, name, nlen);
    shm->name[nlen] = '\0';
    shm->n_pages  = n_pages;
    shm->size     = size;
    shm->refcount = 1;
    shm->active   = true;

    int id = (int)(shm - g_shm);
    spin_unlock_irqrestore(&g_shm_lock, f);

    (void)flags;
    kprintf("[shm] created '%s' size=%zu pages=%zu id=%d\n",
            name, size, n_pages, id);
    return id;
}

/* Map shared memory into the calling process's address space. */
long sys_shm_map(int shm_id, uint64_t hint_addr) {
    if (shm_id < 0 || shm_id >= SHM_MAX) return -22;

    struct proc *p = current_proc();
    if (!p) return -1;

    uint64_t f = spin_lock_irqsave(&g_shm_lock);
    struct shm_object *shm = &g_shm[shm_id];
    if (!shm->active) {
        spin_unlock_irqrestore(&g_shm_lock, f);
        return -22;
    }

    size_t total_size = shm->n_pages * 4096;
    spin_unlock_irqrestore(&g_shm_lock, f);

    /* Use mmap to find a free region and map the shared pages */
    uint64_t addr = sys_mmap(hint_addr, total_size,
                             0x03 /* PROT_READ|PROT_WRITE */,
                             0x01 /* MAP_ANON */, -1, 0);
    if ((long)addr < 0) return addr;

    /* Remap with the shared physical pages instead of the anonymous ones */
    uint64_t saved_root = vmm_set_editor_root(p->cr3);
    for (size_t i = 0; i < shm->n_pages; i++) {
        uint64_t va = addr + i * 4096;
        vmm_unmap(va, 4096);
        vmm_map(va, shm->phys_pages[i], 4096, VMM_USER | VMM_WRITE);
    }
    vmm_set_editor_root(saved_root);

    return (long)addr;
}

/* Unlink (destroy when last ref closes) */
long sys_shm_unlink(const char *name) {
    if (!name) return -22;

    uint64_t f = spin_lock_irqsave(&g_shm_lock);
    struct shm_object *shm = shm_find(name);
    if (!shm) {
        spin_unlock_irqrestore(&g_shm_lock, f);
        return -2; /* ENOENT */
    }

    shm->refcount--;
    if (shm->refcount <= 0) {
        for (size_t i = 0; i < shm->n_pages; i++)
            pmm_free_page(shm->phys_pages[i]);
        shm->active = false;
    }
    spin_unlock_irqrestore(&g_shm_lock, f);
    return 0;
}
