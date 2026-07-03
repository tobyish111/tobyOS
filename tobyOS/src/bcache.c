/* bcache.c -- write-back block buffer cache.
 *
 * Sits between filesystem drivers and the raw blk_dev layer. Caches
 * up to BCACHE_MAX_ENTRIES 4 KiB blocks in RAM using LRU eviction.
 * Dirty blocks are flushed on eviction or explicit bcache_sync().
 */

#include <tobyos/bcache.h>
#include <tobyos/blk.h>
#include <tobyos/spinlock.h>
#include <tobyos/pit.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

struct bcache_entry {
    struct blk_dev *dev;
    uint64_t        lba;
    bool            valid;
    bool            dirty;
    uint32_t        refcount;
    uint64_t        last_access;
    uint8_t         data[BCACHE_BLOCK_SIZE];
};

static struct bcache_entry g_cache[BCACHE_MAX_ENTRIES];
static spinlock_t          g_lock = SPINLOCK_INIT;
static uint64_t            g_tick;
static uint64_t            g_hits;
static uint64_t            g_misses;
static uint64_t            g_evictions;

void bcache_init(void) {
    memset(g_cache, 0, sizeof(g_cache));
    g_tick       = 0;
    g_hits       = 0;
    g_misses     = 0;
    g_evictions  = 0;
    kprintf("[bcache] initialised: %u entries, %u KiB pool\n",
            BCACHE_MAX_ENTRIES,
            (BCACHE_MAX_ENTRIES * BCACHE_BLOCK_SIZE) / 1024);
}

static struct bcache_entry *find_entry(struct blk_dev *dev, uint64_t lba) {
    for (uint32_t i = 0; i < BCACHE_MAX_ENTRIES; i++) {
        struct bcache_entry *e = &g_cache[i];
        if (e->valid && e->dev == dev && e->lba == lba)
            return e;
    }
    return NULL;
}

static int flush_entry(struct bcache_entry *e) {
    if (!e->dirty) return 0;
    int rc = blk_write(e->dev, e->lba, BCACHE_SECTORS, e->data);
    if (rc == 0)
        e->dirty = false;
    return rc;
}

static struct bcache_entry *evict_one(void) {
    struct bcache_entry *best_clean = NULL;
    struct bcache_entry *best_dirty = NULL;

    for (uint32_t i = 0; i < BCACHE_MAX_ENTRIES; i++) {
        struct bcache_entry *e = &g_cache[i];
        if (!e->valid) return e;
        if (e->refcount != 0) continue;

        if (!e->dirty) {
            if (!best_clean || e->last_access < best_clean->last_access)
                best_clean = e;
        } else {
            if (!best_dirty || e->last_access < best_dirty->last_access)
                best_dirty = e;
        }
    }

    if (best_clean) {
        g_evictions++;
        best_clean->valid = false;
        return best_clean;
    }

    if (best_dirty) {
        flush_entry(best_dirty);
        g_evictions++;
        best_dirty->valid = false;
        return best_dirty;
    }

    return NULL;
}

void *bcache_get(struct blk_dev *dev, uint64_t block_lba) {
    uint64_t flags = spin_lock_irqsave(&g_lock);
    uint64_t tick = ++g_tick;

    struct bcache_entry *e = find_entry(dev, block_lba);
    if (e) {
        e->last_access = tick;
        e->refcount++;
        g_hits++;
        spin_unlock_irqrestore(&g_lock, flags);
        return e->data;
    }

    g_misses++;
    e = evict_one();
    if (!e) {
        spin_unlock_irqrestore(&g_lock, flags);
        kprintf("[bcache] all entries pinned -- cannot evict\n");
        return NULL;
    }

    e->dev         = dev;
    e->lba         = block_lba;
    e->dirty       = false;
    e->refcount    = 1;
    e->last_access = tick;
    e->valid       = true;

    /* Drop the lock for the actual I/O -- the entry is pinned (refcount=1)
     * so no other path will evict it. */
    spin_unlock_irqrestore(&g_lock, flags);

    int rc = blk_read(dev, block_lba, BCACHE_SECTORS, e->data);
    if (rc != 0) {
        flags = spin_lock_irqsave(&g_lock);
        e->valid    = false;
        e->refcount = 0;
        spin_unlock_irqrestore(&g_lock, flags);
        return NULL;
    }
    return e->data;
}

void bcache_mark_dirty(struct blk_dev *dev, uint64_t block_lba) {
    uint64_t flags = spin_lock_irqsave(&g_lock);
    struct bcache_entry *e = find_entry(dev, block_lba);
    if (e) e->dirty = true;
    spin_unlock_irqrestore(&g_lock, flags);
}

void bcache_release(struct blk_dev *dev, uint64_t block_lba) {
    uint64_t flags = spin_lock_irqsave(&g_lock);
    struct bcache_entry *e = find_entry(dev, block_lba);
    if (e && e->refcount > 0) e->refcount--;
    spin_unlock_irqrestore(&g_lock, flags);
}

/* Devices whose dirty blocks were written back during one sync /
 * invalidate pass. Collected under the lock, flushed after release --
 * blk_flush may take milliseconds on spinning media and can yield in
 * blk_io_wait, neither of which belongs inside a spinlock. */
struct bcache_flush_set {
    struct blk_dev *devs[8];
    int             n;
};

static void flush_set_add(struct bcache_flush_set *s, struct blk_dev *d) {
    for (int i = 0; i < s->n; i++)
        if (s->devs[i] == d) return;
    if (s->n < (int)(sizeof(s->devs) / sizeof(s->devs[0])))
        s->devs[s->n++] = d;
}

static void flush_set_run(struct bcache_flush_set *s) {
    for (int i = 0; i < s->n; i++)
        (void)blk_flush(s->devs[i]);
}

void bcache_sync(struct blk_dev *dev) {
    struct bcache_flush_set fs = { .n = 0 };
    uint64_t flags = spin_lock_irqsave(&g_lock);
    for (uint32_t i = 0; i < BCACHE_MAX_ENTRIES; i++) {
        struct bcache_entry *e = &g_cache[i];
        if (!e->valid || !e->dirty) continue;
        if (dev && e->dev != dev) continue;
        if (flush_entry(e) == 0) flush_set_add(&fs, e->dev);
    }
    spin_unlock_irqrestore(&g_lock, flags);
    /* Durability barrier: the writes above only reached the DRIVE's
     * volatile cache. Callers (tobyfs journal checkpoint) rely on
     * bcache_sync meaning "on stable media", so issue the device
     * flush too. Always include the named dev -- its journal writes
     * go direct via blk_write and never show up as dirty entries. */
    if (dev) flush_set_add(&fs, dev);
    flush_set_run(&fs);
}

void bcache_invalidate(struct blk_dev *dev) {
    struct bcache_flush_set fs = { .n = 0 };
    uint64_t flags = spin_lock_irqsave(&g_lock);
    for (uint32_t i = 0; i < BCACHE_MAX_ENTRIES; i++) {
        struct bcache_entry *e = &g_cache[i];
        if (!e->valid) continue;
        if (dev && e->dev != dev) continue;
        if (e->dirty && flush_entry(e) == 0) flush_set_add(&fs, e->dev);
        e->valid    = false;
        e->refcount = 0;
    }
    spin_unlock_irqrestore(&g_lock, flags);
    flush_set_run(&fs);
}

void bcache_discard(struct blk_dev *dev) {
    uint64_t flags = spin_lock_irqsave(&g_lock);
    for (uint32_t i = 0; i < BCACHE_MAX_ENTRIES; i++) {
        struct bcache_entry *e = &g_cache[i];
        if (!e->valid) continue;
        if (dev && e->dev != dev) continue;
        e->dirty    = false;          /* drop without writing back */
        e->valid    = false;
        e->refcount = 0;
    }
    spin_unlock_irqrestore(&g_lock, flags);
}

void bcache_stats(uint64_t *hits, uint64_t *misses, uint64_t *evictions) {
    uint64_t flags = spin_lock_irqsave(&g_lock);
    if (hits)      *hits      = g_hits;
    if (misses)    *misses    = g_misses;
    if (evictions) *evictions = g_evictions;
    spin_unlock_irqrestore(&g_lock, flags);
}
