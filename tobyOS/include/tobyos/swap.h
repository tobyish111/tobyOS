/* swap.h -- swap subsystem for page eviction.
 *
 * Phase 1 Depth Pass: Virtual Memory Hardening.
 *
 * Provides a simple swap partition backed by a block device. Pages
 * evicted from physical memory are written to swap slots (4KB each,
 * stored as 8 sectors). When a swapped page is faulted back in, a
 * new physical page is allocated and the data is read from disk.
 *
 * Swap slot encoding in PTE: when a page is swapped out, the PTE is
 * marked not-present with the swap slot ID stored in bits 12..31.
 */

#ifndef TOBYOS_SWAP_H
#define TOBYOS_SWAP_H

#include <tobyos/types.h>

#define SWAP_SLOT_COUNT  4096   /* 4096 slots * 4KB = 16MB swap capacity */
#define SWAP_MAGIC       0x53574150  /* "SWAP" */

#define SWAP_SECTORS_PER_PAGE  (4096 / 512)  /* 8 sectors per 4KB page */

struct blk_dev;

struct swap_entry {
    uint64_t phys_page;     /* original physical page (0 if slot free) */
    int      owner_pid;
    uint64_t virt_addr;
    int      active;
    int      compressed;    /* 1 => data lives in the zram pool, not on disk */
    int      zram_slot;     /* zram slot id when compressed */
};

/* Initialize swap with an EXPLICIT backing device (NULL = zram-only).
 * swap_partition_lba: starting LBA on that device
 * swap_size_sectors:  size of the swap area in 512-byte sectors
 *
 * Slice 122c: the old swap_init(), which picked blk_first_partition()
 * automatically, is deleted -- on a machine with a foreign disk that
 * "first partition" was the Windows EFI system partition, and disk swap
 * would have written into it. The caller must resolve the device /data
 * actually lives on and refuse the RAM fallback; see the kernel.c call
 * site. Bounds are re-checked here against dev->sector_count. */
void swap_init_dev(struct blk_dev *dev, uint64_t swap_partition_lba,
                   uint64_t swap_size_sectors);

/* Write a 4KB page to swap. Returns the slot_id on success, -1 on failure.
 * After swap_out, the physical page can be freed by the caller. */
int swap_out(uint64_t phys_page, int pid, uint64_t virt_addr);

/* Read a page from swap into a newly-allocated physical page.
 * Returns 0 on success (fills *phys_out), -1 on failure. */
int swap_in(int slot_id, uint64_t *phys_out);

/* Free a swap slot (page was discarded or process exited). */
void swap_free(int slot_id);

/* Return swap usage as percentage 0-100. */
int swap_get_usage(void);

/* Return number of active swap slots. */
int swap_get_used_slots(void);

/* Encode a swap slot into a not-present PTE value. */
static inline uint64_t swap_encode_pte(int slot_id) {
    return ((uint64_t)slot_id << 12) | (1ULL << 11); /* PTE_SWAPPED */
}

/* Decode a swap slot from a not-present PTE value. Returns -1 if
 * the PTE is not a swap entry. */
static inline int swap_decode_pte(uint64_t pte) {
    if (!(pte & (1ULL << 11))) return -1;  /* not a swap entry */
    return (int)((pte >> 12) & 0xFFFFF);   /* 20-bit slot ID */
}

#endif /* TOBYOS_SWAP_H */
