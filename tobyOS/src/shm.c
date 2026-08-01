/* shm.c -- POSIX + SysV shared memory (tier 2.5 MIT-SHM needs SysV). */

#include <tobyos/shm.h>
#include <tobyos/proc.h>
#include <tobyos/vmm.h>
#include <tobyos/pmm.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/spinlock.h>
#include <tobyos/mmap.h>
#include <tobyos/page_fault.h>
#include <tobyos/uaccess.h>

#define SHM_MAX        64
#define SHM_NAME_MAX   64
#define SYSV_MAX_PAGES 512   /* 2 MiB -- enough for 800x600x4 */

#define IPC_PRIVATE    0
#define IPC_CREAT      01000
#define IPC_EXCL       02000
#define IPC_RMID       0
#define IPC_STAT       2
#define SHM_RDONLY     010000

struct shm_object {
    char     name[SHM_NAME_MAX];
    uint64_t phys_pages[64];
    size_t   n_pages;
    size_t   size;
    int      refcount;
    bool     active;
};

struct sysv_seg {
    bool     active;
    int      key;
    size_t   size;
    size_t   n_pages;
    uint64_t *pages;     /* kmalloc'd array of phys addrs */
    int      nattch;
    int      marked_rmid;
};

static struct shm_object g_shm[SHM_MAX];
static struct sysv_seg   g_sysv[SHM_MAX];
static spinlock_t g_shm_lock = SPINLOCK_INIT;

void shm_init(void) {
    memset(g_shm, 0, sizeof(g_shm));
    memset(g_sysv, 0, sizeof(g_sysv));
    kprintf("[shm] shared memory ready (POSIX + SysV, %d slots)\n", SHM_MAX);
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

long sys_shm_open(const char *name, int flags, size_t size) {
    if (!name || !name[0]) return -22;

    uint64_t f = spin_lock_irqsave(&g_shm_lock);
    struct shm_object *shm = shm_find(name);
    if (shm) {
        shm->refcount++;
        int id = (int)(shm - g_shm);
        spin_unlock_irqrestore(&g_shm_lock, f);
        return id;
    }
    if (size == 0) {
        spin_unlock_irqrestore(&g_shm_lock, f);
        return -22;
    }
    shm = shm_alloc_slot();
    if (!shm) {
        spin_unlock_irqrestore(&g_shm_lock, f);
        return -12;
    }
    size_t n_pages = (size + 4095) / 4096;
    if (n_pages > 64) {
        spin_unlock_irqrestore(&g_shm_lock, f);
        return -22;
    }
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
    return id;
}

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
    size_t n_pages = shm->n_pages;
    uint64_t pages[64];
    memcpy(pages, shm->phys_pages, n_pages * sizeof(uint64_t));
    spin_unlock_irqrestore(&g_shm_lock, f);

    size_t total_size = n_pages * 4096;
    uint64_t addr = (uint64_t)sys_mmap(hint_addr, total_size,
                             0x03, 0x01, -1, 0);
    if ((long)addr < 0) return (long)addr;

    uint64_t saved_root = vmm_set_editor_root(p->cr3);
    for (size_t i = 0; i < n_pages; i++) {
        uint64_t va = addr + i * 4096;
        vmm_unmap(va, 4096);
        vmm_map(va, pages[i], 4096, VMM_USER | VMM_WRITE | VMM_SHARED);
    }
    vmm_set_editor_root(saved_root);
    return (long)addr;
}

long sys_shm_unlink(const char *name) {
    if (!name) return -22;
    uint64_t f = spin_lock_irqsave(&g_shm_lock);
    struct shm_object *shm = shm_find(name);
    if (!shm) {
        spin_unlock_irqrestore(&g_shm_lock, f);
        return -2;
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

/* ---- SysV ---- */

static int sysv_find_key(int key) {
    if (key == IPC_PRIVATE) return -1;
    for (int i = 0; i < SHM_MAX; i++)
        if (g_sysv[i].active && g_sysv[i].key == key) return i;
    return -1;
}

static int sysv_alloc_slot(void) {
    for (int i = 0; i < SHM_MAX; i++)
        if (!g_sysv[i].active) return i;
    return -1;
}

static long sysv_create(int key, size_t size) {
    size_t n_pages = (size + 4095) / 4096;
    if (n_pages == 0 || n_pages > SYSV_MAX_PAGES) return -22;
    int slot = sysv_alloc_slot();
    if (slot < 0) return -12;
    uint64_t *pages = kmalloc(n_pages * sizeof(uint64_t));
    if (!pages) return -12;
    for (size_t i = 0; i < n_pages; i++) {
        pages[i] = pmm_alloc_page();
        if (!pages[i]) {
            for (size_t j = 0; j < i; j++) pmm_free_page(pages[j]);
            kfree(pages);
            return -12;
        }
        memset((void *)(pages[i] + vmm_hhdm_offset()), 0, 4096);
        if (page_ref_get(pages[i]) == 0) page_ref_inc(pages[i]);
    }
    g_sysv[slot].active = true;
    g_sysv[slot].key = key;
    g_sysv[slot].size = size;
    g_sysv[slot].n_pages = n_pages;
    g_sysv[slot].pages = pages;
    g_sysv[slot].nattch = 0;
    g_sysv[slot].marked_rmid = 0;
    return slot;
}

long sys_shmget(int key, size_t size, int shmflg) {
    uint64_t f = spin_lock_irqsave(&g_shm_lock);
    int existing = sysv_find_key(key);
    if (existing >= 0) {
        if ((shmflg & IPC_CREAT) && (shmflg & IPC_EXCL)) {
            spin_unlock_irqrestore(&g_shm_lock, f);
            return -17; /* EEXIST */
        }
        spin_unlock_irqrestore(&g_shm_lock, f);
        return existing;
    }
    if (!(shmflg & IPC_CREAT) && key != IPC_PRIVATE) {
        spin_unlock_irqrestore(&g_shm_lock, f);
        return -2; /* ENOENT */
    }
    long id = sysv_create(key == IPC_PRIVATE ? -1 - (int)size : key, size);
    spin_unlock_irqrestore(&g_shm_lock, f);
    if (id >= 0)
        kprintf("[shm] shmget key=0x%x size=%zu -> shmid=%ld\n",
                key, size, id);
    return id;
}

long sysv_shm_ensure(int key, size_t size) {
    uint64_t f = spin_lock_irqsave(&g_shm_lock);
    int existing = sysv_find_key(key);
    if (existing >= 0) {
        if (g_sysv[existing].size < size) {
            spin_unlock_irqrestore(&g_shm_lock, f);
            return -22;
        }
        spin_unlock_irqrestore(&g_shm_lock, f);
        return existing;
    }
    long id = sysv_create(key, size);
    spin_unlock_irqrestore(&g_shm_lock, f);
    return id;
}

long sys_shmat(int shmid, uint64_t shmaddr, int shmflg) {
    if (shmid < 0 || shmid >= SHM_MAX) return -22;
    struct proc *p = current_proc();
    if (!p) return -1;

    uint64_t f = spin_lock_irqsave(&g_shm_lock);
    struct sysv_seg *seg = &g_sysv[shmid];
    if (!seg->active) {
        spin_unlock_irqrestore(&g_shm_lock, f);
        return -22;
    }
    size_t n_pages = seg->n_pages;
    size_t total = n_pages * 4096;
    uint64_t *pages = seg->pages;
    spin_unlock_irqrestore(&g_shm_lock, f);

    uint64_t addr;
    if (shmaddr) {
        addr = shmaddr & ~0xFFFull;
    } else {
        long a = sys_mmap(0, total, 0x03, 0x01 /* ANON */, -1, 0);
        if (a < 0) return a;
        addr = (uint64_t)a;
    }

    uint32_t flags = VMM_USER | VMM_SHARED;
    if (!(shmflg & SHM_RDONLY)) flags |= VMM_WRITE;

    uint64_t saved_root = vmm_set_editor_root(p->cr3);
    for (size_t i = 0; i < n_pages; i++) {
        uint64_t va = addr + i * 4096;
        if (!shmaddr) vmm_unmap(va, 4096);
        vmm_map(va, pages[i], 4096, flags);
        page_ref_inc(pages[i]);
    }
    vmm_set_editor_root(saved_root);

    f = spin_lock_irqsave(&g_shm_lock);
    if (g_sysv[shmid].active) g_sysv[shmid].nattch++;
    spin_unlock_irqrestore(&g_shm_lock, f);
    return (long)addr;
}

long sys_shmdt(uint64_t shmaddr) {
    /* Best-effort: unmap a reasonable span; full tracking deferred. */
    if (!shmaddr) return -22;
    struct proc *p = current_proc();
    if (!p) return -1;
    /* Unmap up to SYSV_MAX_PAGES; munmap of unused is fine. */
    (void)sys_munmap(shmaddr, SYSV_MAX_PAGES * 4096);
    return 0;
}

long sys_shmctl(int shmid, int cmd, void *buf) {
    if (shmid < 0 || shmid >= SHM_MAX) return -22;
    uint64_t f = spin_lock_irqsave(&g_shm_lock);
    struct sysv_seg *seg = &g_sysv[shmid];
    if (!seg->active) {
        spin_unlock_irqrestore(&g_shm_lock, f);
        return -22;
    }
    if (cmd == IPC_RMID) {
        seg->marked_rmid = 1;
        if (seg->nattch == 0) {
            for (size_t i = 0; i < seg->n_pages; i++)
                pmm_free_page(seg->pages[i]);
            kfree(seg->pages);
            memset(seg, 0, sizeof(*seg));
        }
        spin_unlock_irqrestore(&g_shm_lock, f);
        return 0;
    }
    if (cmd == IPC_STAT && buf) {
        /* Minimal shmid_ds: size at offset that Linux x86-64 uses varies;
         * fill a small local and copy. */
        uint8_t tmp[96];
        memset(tmp, 0, sizeof tmp);
        /* shm_segsz is at offset 48 on x86-64 Linux shmid64_ds in some
         * layouts; Ozone rarely needs STAT. Put size at word 6. */
        *(uint64_t *)(tmp + 48) = seg->size;
        spin_unlock_irqrestore(&g_shm_lock, f);
        if (copy_to_user(buf, tmp, sizeof tmp) != 0) return -14;
        return 0;
    }
    spin_unlock_irqrestore(&g_shm_lock, f);
    return 0;
}

int sysv_shm_lookup(int shmid, uint64_t **pages_out, size_t *npages_out,
                    size_t *size_out) {
    if (shmid < 0 || shmid >= SHM_MAX) return -1;
    uint64_t f = spin_lock_irqsave(&g_shm_lock);
    struct sysv_seg *seg = &g_sysv[shmid];
    if (!seg->active) {
        spin_unlock_irqrestore(&g_shm_lock, f);
        return -1;
    }
    if (pages_out) *pages_out = seg->pages;
    if (npages_out) *npages_out = seg->n_pages;
    if (size_out) *size_out = seg->size;
    spin_unlock_irqrestore(&g_shm_lock, f);
    return 0;
}

void *sysv_shm_kva(int shmid) {
    uint64_t *pages = 0;
    size_t np = 0;
    if (sysv_shm_lookup(shmid, &pages, &np, 0) < 0 || !pages || !np)
        return 0;
    return (void *)(pages[0] + vmm_hhdm_offset());
}

size_t sysv_shm_size(int shmid) {
    size_t sz = 0;
    if (sysv_shm_lookup(shmid, 0, 0, &sz) < 0) return 0;
    return sz;
}
