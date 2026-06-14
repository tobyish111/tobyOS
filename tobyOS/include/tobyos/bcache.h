#ifndef TOBYOS_BCACHE_H
#define TOBYOS_BCACHE_H

#include <tobyos/types.h>

struct blk_dev;

#define BCACHE_BLOCK_SIZE  4096
#define BCACHE_SECTORS     (BCACHE_BLOCK_SIZE / 512)
#define BCACHE_MAX_ENTRIES 256

void bcache_init(void);

/* Read a 4KiB block. Returns pointer to cached data (valid until bcache_release).
 * The block is read from disk on cache miss. Returns NULL on I/O error. */
void *bcache_get(struct blk_dev *dev, uint64_t block_lba);

/* Mark the currently-held block as dirty (will be written back on eviction or sync). */
void bcache_mark_dirty(struct blk_dev *dev, uint64_t block_lba);

/* Release a block obtained via bcache_get (decrements refcount). */
void bcache_release(struct blk_dev *dev, uint64_t block_lba);

/* Flush all dirty blocks for a device (or all devices if dev is NULL). */
void bcache_sync(struct blk_dev *dev);

/* Invalidate all cache entries for a device (flushes dirty blocks first). */
void bcache_invalidate(struct blk_dev *dev);

/* Drop all cache entries for a device WITHOUT flushing dirty blocks --
 * i.e. discard not-yet-written-back data. Models a power loss (the
 * write-back cache is volatile). Used by the FS crash-consistency tests. */
void bcache_discard(struct blk_dev *dev);

/* Statistics for diagnostics. */
void bcache_stats(uint64_t *hits, uint64_t *misses, uint64_t *evictions);

#endif
