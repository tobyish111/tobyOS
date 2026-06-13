/* zram.h -- compressed in-RAM page store (Linux zram/zswap style).
 *
 * Instead of writing every evicted page out to the swap disk, we first
 * try to LZ4-compress it and keep the compressed blob in kernel RAM. A
 * page that compresses well (zeros, repeated structure -- the common case
 * for anonymous memory) then costs a fraction of a frame and faults back
 * in without touching the disk; an incompressible page is rejected so the
 * caller falls through to disk swap. swap.c is the integration point.
 *
 * Each stored page gets a slot id (the handle the swap layer keeps). The
 * compressed bytes live in a heap blob sized to the page; loading a slot
 * decompresses it back into a fresh 4 KiB page and frees the blob.
 */

#ifndef TOBYOS_ZRAM_H
#define TOBYOS_ZRAM_H

#include <tobyos/types.h>

/* Allocate the pool's slot table + codec scratch. Returns false (pool
 * disabled, zram_store always rejects) if the allocations fail. Idempotent. */
bool zram_init(void);

/* Compress and store one PAGE_SIZE page. Returns a slot id >= 0 on
 * success, or -1 if the page is incompressible, the pool is full, or zram
 * is disabled (caller should then store the page uncompressed). */
int zram_store(const void *page);

/* Decompress slot `id` into `page_out` (must be PAGE_SIZE) and free the
 * slot. Returns 0 on success, -1 on a bad slot / decode error. */
int zram_load(int id, void *page_out);

/* Discard a stored slot without decompressing (page was dropped). */
void zram_free(int id);

/* ---- stats (for the self-test + sysmon) ---- */
size_t   zram_active_pages(void);      /* pages currently held */
uint64_t zram_stored_bytes(void);      /* compressed bytes currently held */
uint64_t zram_orig_bytes(void);        /* original bytes those represent */
size_t   zram_frames_saved(void);      /* whole 4K frames net reclaimed */
uint32_t zram_ratio_x100(void);        /* orig*100/compressed (e.g. 312 = 3.12x) */

/* Self-test: LZ4 round-trip + ratios, zram store/load round-trip, and the
 * swap-layer compress/fallback path on a ramdev. Returns 0 on all-pass.
 * Emits [MEMCT] markers. Called from kernel.c under -DMEMCOMP_SELFTEST. */
int memcomp_self_test(void);

#endif /* TOBYOS_ZRAM_H */
