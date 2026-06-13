/* swap.c -- swap subsystem for page eviction.
 *
 * Phase 1 Depth Pass: Virtual Memory Hardening.
 *
 * Pages that are evicted from physical memory are written to a swap
 * partition on disk. Each swap slot holds one 4KB page (8 sectors).
 * The slot table is a simple flat array; slot allocation is O(N) scan
 * which is acceptable for the 4096-slot budget.
 */

#include <tobyos/swap.h>
#include <tobyos/pmm.h>
#include <tobyos/blk.h>
#include <tobyos/zram.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

static struct swap_entry g_swap_slots[SWAP_SLOT_COUNT];
static uint64_t g_swap_lba_base;
static uint64_t g_swap_size_sectors;
static struct blk_dev *g_swap_dev;
static bool g_swap_ready;
static int g_swap_used;

void swap_init_dev(struct blk_dev *dev, uint64_t swap_partition_lba,
                   uint64_t swap_size_sectors) {
    g_swap_lba_base = swap_partition_lba;
    g_swap_size_sectors = swap_size_sectors;
    g_swap_used = 0;
    g_swap_ready = false;

    memset(g_swap_slots, 0, sizeof(g_swap_slots));

    /* Bring up the compressed in-RAM page store. Even if there's no swap
     * disk, zram lets compressible pages be evicted (kept compressed in
     * RAM) -- so memory compression works independently of disk swap. */
    zram_init();

    g_swap_dev = dev;
    if (g_swap_dev && swap_size_sectors >= SWAP_SLOT_COUNT * SWAP_SECTORS_PER_PAGE) {
        g_swap_ready = true;
        kprintf("swap: initialized %d slots (%d MB) on %s at LBA %lu (+zram)\n",
                SWAP_SLOT_COUNT,
                (SWAP_SLOT_COUNT * 4096) / (1024 * 1024),
                g_swap_dev->name ? g_swap_dev->name : "?",
                (unsigned long)g_swap_lba_base);
    } else {
        kprintf("swap: no disk device -- zram-only eviction\n");
    }
}

void swap_init(uint64_t swap_partition_lba, uint64_t swap_size_sectors) {
    struct blk_dev *dev = blk_first_partition();
    if (!dev) dev = blk_first_disk();
    swap_init_dev(dev, swap_partition_lba, swap_size_sectors);
}

static int find_free_slot(void) {
    for (int i = 0; i < SWAP_SLOT_COUNT; i++) {
        if (!g_swap_slots[i].active)
            return i;
    }
    return -1;
}

int swap_out(uint64_t phys_page, int pid, uint64_t virt_addr) {
    /* We need a slot regardless of where the bytes end up (the slot id is
     * the handle the PTE keeps). Both zram and disk eviction use it. */
    int slot = find_free_slot();
    if (slot < 0) return -1;

    void *page_virt = pmm_phys_to_virt(phys_page);

    /* Prefer the compressed in-RAM store: a page that compresses well
     * never touches the disk (faster fault-in, frames reclaimed). This
     * works even without a swap disk. */
    int zslot = zram_store(page_virt);
    if (zslot >= 0) {
        g_swap_slots[slot].phys_page  = phys_page;
        g_swap_slots[slot].owner_pid  = pid;
        g_swap_slots[slot].virt_addr  = virt_addr;
        g_swap_slots[slot].active     = 1;
        g_swap_slots[slot].compressed = 1;
        g_swap_slots[slot].zram_slot  = zslot;
        g_swap_used++;
        return slot;
    }

    /* Incompressible: fall back to the disk swap area, if any. */
    if (!g_swap_ready) return -1;

    uint64_t slot_lba = g_swap_lba_base + (uint64_t)slot * SWAP_SECTORS_PER_PAGE;
    int rc = blk_write(g_swap_dev, slot_lba, SWAP_SECTORS_PER_PAGE, page_virt);
    if (rc != 0) {
        kprintf("swap: write failed for slot %d (LBA %lu)\n",
                slot, (unsigned long)slot_lba);
        return -1;
    }

    g_swap_slots[slot].phys_page  = phys_page;
    g_swap_slots[slot].owner_pid  = pid;
    g_swap_slots[slot].virt_addr  = virt_addr;
    g_swap_slots[slot].active     = 1;
    g_swap_slots[slot].compressed = 0;
    g_swap_used++;

    return slot;
}

int swap_in(int slot_id, uint64_t *phys_out) {
    if (slot_id < 0 || slot_id >= SWAP_SLOT_COUNT) return -1;
    if (!g_swap_slots[slot_id].active) return -1;
    bool compressed = g_swap_slots[slot_id].compressed;
    if (!compressed && !g_swap_ready) return -1;

    /* Allocate a fresh physical page */
    uint64_t new_phys = pmm_alloc_page();
    if (!new_phys) return -1;
    void *page_virt = pmm_phys_to_virt(new_phys);

    if (compressed) {
        if (zram_load(g_swap_slots[slot_id].zram_slot, page_virt) != 0) {
            kprintf("swap: zram decode failed for slot %d\n", slot_id);
            pmm_free_page(new_phys);
            return -1;
        }
    } else {
        uint64_t slot_lba = g_swap_lba_base +
                            (uint64_t)slot_id * SWAP_SECTORS_PER_PAGE;
        int rc = blk_read(g_swap_dev, slot_lba, SWAP_SECTORS_PER_PAGE, page_virt);
        if (rc != 0) {
            kprintf("swap: read failed for slot %d (LBA %lu)\n",
                    slot_id, (unsigned long)slot_lba);
            pmm_free_page(new_phys);
            return -1;
        }
    }

    /* Free the swap slot */
    g_swap_slots[slot_id].active = 0;
    g_swap_slots[slot_id].phys_page = 0;
    g_swap_slots[slot_id].compressed = 0;
    g_swap_used--;

    *phys_out = new_phys;
    return 0;
}

void swap_free(int slot_id) {
    if (slot_id < 0 || slot_id >= SWAP_SLOT_COUNT) return;
    if (!g_swap_slots[slot_id].active) return;

    if (g_swap_slots[slot_id].compressed)
        zram_free(g_swap_slots[slot_id].zram_slot);

    g_swap_slots[slot_id].active = 0;
    g_swap_slots[slot_id].phys_page = 0;
    g_swap_slots[slot_id].owner_pid = 0;
    g_swap_slots[slot_id].virt_addr = 0;
    g_swap_slots[slot_id].compressed = 0;
    g_swap_used--;
}

int swap_get_usage(void) {
    if (SWAP_SLOT_COUNT == 0) return 0;
    return (g_swap_used * 100) / SWAP_SLOT_COUNT;
}

int swap_get_used_slots(void) {
    return g_swap_used;
}
