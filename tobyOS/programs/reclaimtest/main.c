/* programs/reclaimtest/main.c -- proof that page reclaim round-trips data.
 *
 * The whole point of this program is that a GREEN exit code has to be
 * impossible unless every evicted page came back byte-for-byte. Reclaim's
 * failure mode is silent memory corruption, not a crash, so a test that only
 * checks "we survived" would pass over exactly the bug it exists to find.
 *
 * Shape:
 *   1. mmap a block of anonymous pages.
 *   2. Fill every page with a pattern derived from BOTH the page index and the
 *      offset within the page, so a swap slot delivered to the wrong page --
 *      or a page silently zeroed -- is detectable. A constant fill would pass
 *      even if every page were swapped with every other.
 *   3. Sleep. Sleeping is what makes this work: the process goes PROC_BLOCKED,
 *      which is the only state mem_reclaim_pages() will evict from. Under
 *      -DRECLAIM_BOOT the idle loop reclaims unconditionally every tick, so
 *      the pages are evicted (repeatedly) during this window.
 *   4. Read every byte back and compare.
 *
 * Rounds: the sleep/verify cycle runs several times, so a page is evicted,
 * faulted back, re-dirtied and evicted again -- which is where refcount and
 * TLB-shootdown mistakes actually show up, rather than on a single clean pass.
 *
 * Exit codes: 0 = all rounds verified, 1 = mmap failed, 2 = MISMATCH.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#define PAGE_SZ    4096u
#define N_PAGES    256u              /* 1 MiB -- big enough to span batches */
#define N_ROUNDS   3
#define SLEEP_US   700000u           /* 0.7 s blocked, per round */

/* Pattern byte for (page, offset). Mixes both coordinates so a whole-page
 * mix-up and a within-page shift are both caught. */
static unsigned char pat(unsigned page, unsigned off) {
    return (unsigned char)((page * 31u + off * 7u + (page >> 3)) & 0xFF);
}

int main(void) {
    unsigned char *mem = mmap(0, (size_t)N_PAGES * PAGE_SZ,
                              PROT_READ | PROT_WRITE,
                              MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED || !mem) {
        printf("[RECLAIM] mmap of %u pages FAILED\n", N_PAGES);
        return 1;
    }
    printf("[RECLAIM] mapped %u pages at %p\n", N_PAGES, (void *)mem);

    for (int round = 1; round <= N_ROUNDS; round++) {
        /* Re-fill every round: round N verifies data written AFTER round N-1's
         * eviction, so a page that came back stale (rather than corrupt) is
         * caught too. The round number is folded in via the fill below. */
        for (unsigned pg = 0; pg < N_PAGES; pg++)
            for (unsigned off = 0; off < PAGE_SZ; off++)
                mem[pg * PAGE_SZ + off] =
                    (unsigned char)(pat(pg, off) + (unsigned char)round);

        /* Block, so the kernel considers us for eviction. */
        usleep(SLEEP_US);

        unsigned long bad = 0;
        unsigned first_bad_pg = 0, first_bad_off = 0;
        unsigned char got = 0, want = 0;
        for (unsigned pg = 0; pg < N_PAGES; pg++) {
            for (unsigned off = 0; off < PAGE_SZ; off++) {
                unsigned char w = (unsigned char)(pat(pg, off) +
                                                  (unsigned char)round);
                unsigned char g = mem[pg * PAGE_SZ + off];
                if (g != w) {
                    if (!bad) {
                        first_bad_pg = pg; first_bad_off = off;
                        got = g; want = w;
                    }
                    bad++;
                }
            }
        }

        if (bad) {
            printf("[RECLAIM] round %d MISMATCH: %lu bad byte(s); "
                   "first at page %u off %u (got 0x%02x want 0x%02x)\n",
                   round, bad, first_bad_pg, first_bad_off, got, want);
            printf("[RECLAIM] VERDICT: FAIL\n");
            return 2;
        }
        printf("[RECLAIM] round %d verified %u pages OK\n", round, N_PAGES);
    }

    printf("[RECLAIM] VERDICT: PASS rounds=%d pages=%u\n", N_ROUNDS, N_PAGES);
    return 0;
}
