/* zram.c -- compressed in-RAM page store + memory-compression self-test.
 *
 * See zram.h. Each stored page is LZ4-compressed into a heap blob sized
 * exactly to the compressed length; the slot id is the handle swap.c
 * keeps. Incompressible pages are rejected so the caller falls back to
 * disk swap.
 */

#include <tobyos/zram.h>
#include <tobyos/lz4.h>
#include <tobyos/pmm.h>
#include <tobyos/blk.h>
#include <tobyos/swap.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/printk.h>

#define ZRAM_MAX_SLOTS 4096

struct zram_slot {
    uint8_t *data;     /* compressed blob (kmalloc'd, clen bytes) */
    uint16_t clen;     /* compressed length (< PAGE_SIZE, always) */
    uint8_t  active;
};

static struct zram_slot *g_slots;
static int      g_nslots;
static int     *g_ht;          /* persistent LZ4 hash table */
static uint8_t *g_scratch;     /* PAGE_SIZE compress scratch */
static bool     g_ready;

static size_t   g_active;
static uint64_t g_comp_bytes;  /* sum of clen over active slots */

bool zram_init(void) {
    if (g_ready) return true;
    g_nslots  = ZRAM_MAX_SLOTS;
    g_slots   = kcalloc((size_t)g_nslots, sizeof(*g_slots));
    g_ht      = kmalloc(LZ4_HASHTABLE_BYTES);
    g_scratch = kmalloc(PAGE_SIZE);
    if (!g_slots || !g_ht || !g_scratch) {
        if (g_slots)   { kfree(g_slots);   g_slots = NULL; }
        if (g_ht)      { kfree(g_ht);      g_ht = NULL; }
        if (g_scratch) { kfree(g_scratch); g_scratch = NULL; }
        kprintf("[zram] init failed (out of memory) -- compression disabled\n");
        return false;
    }
    g_active = 0;
    g_comp_bytes = 0;
    g_ready = true;
    kprintf("[zram] compressed page store: %d slots, LZ4 codec\n", g_nslots);
    return true;
}

int zram_store(const void *page) {
    if (!g_ready) return -1;
    int clen = lz4_compress(page, (int)PAGE_SIZE, g_scratch, (int)PAGE_SIZE, g_ht);
    if (clen <= 0) return -1;                  /* incompressible -> caller uses disk */

    int slot = -1;
    for (int i = 0; i < g_nslots; i++)
        if (!g_slots[i].active) { slot = i; break; }
    if (slot < 0) return -1;                   /* pool full */

    uint8_t *data = kmalloc((size_t)clen);
    if (!data) return -1;
    memcpy(data, g_scratch, (size_t)clen);

    g_slots[slot].data   = data;
    g_slots[slot].clen   = (uint16_t)clen;
    g_slots[slot].active = 1;
    g_active++;
    g_comp_bytes += (uint64_t)clen;
    return slot;
}

int zram_load(int id, void *page_out) {
    if (!g_ready || id < 0 || id >= g_nslots || !g_slots[id].active) return -1;
    int dl = lz4_decompress(g_slots[id].data, g_slots[id].clen,
                            page_out, (int)PAGE_SIZE);
    bool ok = (dl == (int)PAGE_SIZE);
    g_comp_bytes -= g_slots[id].clen;
    kfree(g_slots[id].data);
    g_slots[id].data   = NULL;
    g_slots[id].clen   = 0;
    g_slots[id].active = 0;
    g_active--;
    return ok ? 0 : -1;
}

void zram_free(int id) {
    if (!g_ready || id < 0 || id >= g_nslots || !g_slots[id].active) return;
    g_comp_bytes -= g_slots[id].clen;
    kfree(g_slots[id].data);
    g_slots[id].data   = NULL;
    g_slots[id].clen   = 0;
    g_slots[id].active = 0;
    g_active--;
}

size_t   zram_active_pages(void) { return g_active; }
uint64_t zram_stored_bytes(void) { return g_comp_bytes; }
uint64_t zram_orig_bytes(void)   { return (uint64_t)g_active * PAGE_SIZE; }

size_t zram_frames_saved(void) {
    /* frames the originals would occupy minus frames the pool occupies. */
    uint64_t pool_frames = (g_comp_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (g_active <= pool_frames) return 0;
    return g_active - (size_t)pool_frames;
}

uint32_t zram_ratio_x100(void) {
    if (g_comp_bytes == 0) return 0;
    return (uint32_t)(((uint64_t)g_active * PAGE_SIZE * 100) / g_comp_bytes);
}

/* ================================================================
 * Self-test (-DMEMCOMP_SELFTEST). Proves: (1) the LZ4 codec round-trips
 * and shrinks compressible data while rejecting incompressible data;
 * (2) the zram pool stores/loads pages bit-identically and reclaims
 * frames; (3) the swap layer routes compressible pages to zram and
 * incompressible pages to the disk fallback. [MEMCT] markers.
 * ================================================================ */

/* Build a representative page. kind: 0 zeros, 1 repeated 32-B pattern,
 * 2 low-entropy "text", 3 pseudo-random (incompressible). */
static void mc_fill(uint8_t *b, int kind, uint32_t seed) {
    if (kind == 0) { memset(b, 0, PAGE_SIZE); return; }
    if (kind == 1) {
        uint8_t pat[32];
        for (int i = 0; i < 32; i++) pat[i] = (uint8_t)(seed * 7u + (uint32_t)i * 13u);
        for (uint32_t i = 0; i < PAGE_SIZE; i++) b[i] = pat[i & 31];
        return;
    }
    if (kind == 2) {
        uint32_t x = seed ? seed : 1u;
        for (uint32_t i = 0; i < PAGE_SIZE; i++) {
            x = x * 1103515245u + 12345u;
            uint32_t r = (x >> 16) & 0xFF;
            b[i] = (r < 48) ? (uint8_t)' ' : (uint8_t)('a' + (r % 8));
        }
        return;
    }
    uint32_t x = seed ? seed : 0xDEADBEEFu;
    for (uint32_t i = 0; i < PAGE_SIZE; i++) {
        x = x * 1664525u + 1013904223u;
        b[i] = (uint8_t)(x >> 24);
    }
}

/* ---- ramdev for the swap disk-fallback sub-test ---- */
struct mc_ramdev { uint8_t *buf; uint64_t bytes; };
static int mc_ram_read(struct blk_dev *dev, uint64_t lba, uint32_t count, void *buf) {
    struct mc_ramdev *r = (struct mc_ramdev *)dev->priv;
    uint64_t off = lba * (uint64_t)BLK_SECTOR_SIZE, span = (uint64_t)count * BLK_SECTOR_SIZE;
    if (off + span > r->bytes) return -1;
    memcpy(buf, r->buf + off, span);
    return 0;
}
static int mc_ram_write(struct blk_dev *dev, uint64_t lba, uint32_t count, const void *buf) {
    struct mc_ramdev *r = (struct mc_ramdev *)dev->priv;
    uint64_t off = lba * (uint64_t)BLK_SECTOR_SIZE, span = (uint64_t)count * BLK_SECTOR_SIZE;
    if (off + span > r->bytes) return -1;
    memcpy(r->buf + off, buf, span);
    return 0;
}
static const struct blk_ops mc_ram_ops = { .read = mc_ram_read, .write = mc_ram_write };

/* swap_out a freshly-filled page of `kind`, then swap_in and verify it
 * comes back bit-identical. Returns 1 on PASS. `*went_zram` reports
 * whether it took the compressed path. */
static int mc_swap_roundtrip(int kind, uint32_t seed, int *went_zram) {
    uint64_t p = pmm_alloc_page();
    if (!p) return 0;
    uint8_t *pv = (uint8_t *)pmm_phys_to_virt(p);
    mc_fill(pv, kind, seed);

    size_t z_before = zram_active_pages();
    int slot = swap_out(p, 1, 0x1000);
    pmm_free_page(p);                          /* original frame released */
    if (slot < 0) { if (went_zram) *went_zram = 0; return 0; }
    if (went_zram) *went_zram = (zram_active_pages() > z_before);

    uint64_t np = 0;
    if (swap_in(slot, &np) != 0) return 0;
    uint8_t *expect = kmalloc(PAGE_SIZE);
    if (!expect) { pmm_free_page(np); return 0; }
    mc_fill(expect, kind, seed);
    int ok = memcmp(pmm_phys_to_virt(np), expect, PAGE_SIZE) == 0;
    kfree(expect);
    pmm_free_page(np);
    return ok;
}

int memcomp_self_test(void) {
    int fails = 0;
    kprintf("[MEMCT] memory-compression (LZ4 + zram) self-test\n");

    if (!zram_init()) { kprintf("[MEMCT] SKIP -- zram disabled\n"); return -1; }

    /* ---- Test 1: LZ4 codec round-trip + ratios ---- */
    {
        int *ht = kmalloc(LZ4_HASHTABLE_BYTES);
        uint8_t *orig = kmalloc(PAGE_SIZE);
        uint8_t *comp = kmalloc(PAGE_SIZE);
        uint8_t *dec  = kmalloc(PAGE_SIZE);
        const char *names[4] = { "zeros", "repeat", "text", "random" };
        if (ht && orig && comp && dec) {
            for (int t = 0; t < 4; t++) {
                mc_fill(orig, t, 0x100 + t);
                int clen = lz4_compress(orig, (int)PAGE_SIZE, comp, (int)PAGE_SIZE, ht);
                bool ok;
                if (t == 3) {
                    /* random: must be rejected, OR if it shrank it must round-trip. */
                    ok = (clen <= 0);
                    if (clen > 0) {
                        int dl = lz4_decompress(comp, clen, dec, (int)PAGE_SIZE);
                        ok = (dl == (int)PAGE_SIZE) && memcmp(orig, dec, PAGE_SIZE) == 0;
                    }
                    kprintf("[MEMCT] t1 %-6s: %s (%s)\n", names[t],
                            ok ? "PASS" : "FAIL",
                            clen <= 0 ? "incompressible, rejected" : "shrank+round-trip");
                } else {
                    ok = clen > 0;
                    if (ok) {
                        int dl = lz4_decompress(comp, clen, dec, (int)PAGE_SIZE);
                        ok = (dl == (int)PAGE_SIZE) && memcmp(orig, dec, PAGE_SIZE) == 0;
                    }
                    uint32_t r = (clen > 0) ? (uint32_t)((PAGE_SIZE * 100) / clen) : 0;
                    kprintf("[MEMCT] t1 %-6s: %s clen=%d ratio=%u.%02ux\n", names[t],
                            ok ? "PASS" : "FAIL", clen, r / 100, r % 100);
                }
                if (!ok) fails++;
            }
        } else { fails++; kprintf("[MEMCT] t1 SKIP -- alloc\n"); }
        if (ht) kfree(ht);
        if (orig) kfree(orig);
        if (comp) kfree(comp);
        if (dec) kfree(dec);
    }

    /* ---- Test 2: zram pool store/load round-trip on a batch ---- */
    {
        const int N = 96;
        uint8_t *page = kmalloc(PAGE_SIZE);
        uint8_t *back = kmalloc(PAGE_SIZE);
        int *slots = kmalloc((size_t)N * sizeof(int));
        if (page && back && slots) {
            int stored = 0;
            for (int i = 0; i < N; i++) {
                mc_fill(page, i % 4, (uint32_t)(i + 1));
                slots[i] = zram_store(page);
                if (slots[i] >= 0) stored++;
            }
            uint32_t peak_ratio = zram_ratio_x100();
            size_t   peak_saved = zram_frames_saved();
            size_t   peak_active = zram_active_pages();

            int ok = (stored > 0);
            for (int i = 0; i < N && ok; i++) {
                if (slots[i] < 0) continue;        /* random pages weren't stored */
                mc_fill(page, i % 4, (uint32_t)(i + 1));   /* expected */
                if (zram_load(slots[i], back) != 0 ||
                    memcmp(page, back, PAGE_SIZE) != 0)
                    ok = 0;
            }
            if (!ok) fails++;
            kprintf("[MEMCT] t2 zram round-trip: %s stored=%d/%d "
                    "peak: active=%lu ratio=%u.%02ux frames_saved=%lu\n",
                    ok ? "PASS" : "FAIL", stored, N,
                    (unsigned long)peak_active, peak_ratio / 100, peak_ratio % 100,
                    (unsigned long)peak_saved);
        } else { fails++; kprintf("[MEMCT] t2 SKIP -- alloc\n"); }
        if (page) kfree(page);
        if (back) kfree(back);
        if (slots) kfree(slots);
    }

    /* ---- Test 3a: swap routes a compressible page to zram (no disk) ---- */
    {
        swap_init_dev(NULL, 0, 0);                 /* zram-only, no disk */
        int wz = 0;
        int ok = mc_swap_roundtrip(1 /*repeat*/, 0xAB, &wz) && wz;
        if (!ok) fails++;
        kprintf("[MEMCT] t3a swap->zram (no disk): %s (zram=%d)\n",
                ok ? "PASS" : "FAIL", wz);

        /* incompressible page with no disk must be rejected, not faked. */
        uint64_t q = pmm_alloc_page();
        int rej_ok = 0;
        if (q) {
            mc_fill(pmm_phys_to_virt(q), 3 /*random*/, 0xCD);
            rej_ok = (swap_out(q, 1, 0x2000) < 0);
            pmm_free_page(q);
        }
        if (!rej_ok) fails++;
        kprintf("[MEMCT] t3b incompressible, no disk -> rejected: %s\n",
                rej_ok ? "PASS" : "FAIL");
    }

    /* ---- Test 3c: incompressible page falls through to the disk swap ---- */
    {
        uint64_t bytes = (uint64_t)SWAP_SLOT_COUNT * SWAP_SECTORS_PER_PAGE *
                         BLK_SECTOR_SIZE;          /* exactly the swap area */
        uint8_t *img = kmalloc((size_t)bytes);
        if (img) {
            memset(img, 0, (size_t)bytes);
            struct mc_ramdev r = { .buf = img, .bytes = bytes };
            struct blk_dev dev; memset(&dev, 0, sizeof(dev));
            dev.name = "swap-ramdev"; dev.ops = &mc_ram_ops;
            dev.sector_count = bytes / BLK_SECTOR_SIZE; dev.priv = &r;
            dev.class = BLK_CLASS_DISK;
            swap_init_dev(&dev, 0, bytes / BLK_SECTOR_SIZE);

            size_t z0 = zram_active_pages();
            int wz = 0;
            int ok_disk = mc_swap_roundtrip(3 /*random*/, 0x99, &wz) &&
                          !wz && zram_active_pages() == z0;   /* went to DISK */
            if (!ok_disk) fails++;
            kprintf("[MEMCT] t3c incompressible -> disk fallback: %s (zram=%d)\n",
                    ok_disk ? "PASS" : "FAIL", wz);

            int wz2 = 0;
            int ok_zram = mc_swap_roundtrip(0 /*zeros*/, 0x11, &wz2) && wz2;
            if (!ok_zram) fails++;
            kprintf("[MEMCT] t3d compressible w/ disk present -> zram: %s (zram=%d)\n",
                    ok_zram ? "PASS" : "FAIL", wz2);
            kfree(img);
        } else { kprintf("[MEMCT] t3c SKIP -- alloc\n"); }
        /* Leave swap in a clean, device-less state; boot's real swap_init
         * (which runs after this self-test) reconfigures it properly. */
        swap_init_dev(NULL, 0, 0);
    }

    kprintf("[MEMCT] %s (%d failure(s))\n", fails == 0 ? "PASS" : "FAIL", fails);
    return fails == 0 ? 0 : -1;
}
