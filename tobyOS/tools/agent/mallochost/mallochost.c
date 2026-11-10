/* mallochost.c -- libtoby/src/malloc.c on the HOST (finding 107). The
 * allocator is compiled under other names against a stub sbrk that hands
 * out regions, some contiguous and some with gaps, and is driven by a long
 * random workload: every block is filled with its own pattern and checked
 * on free and realloc, every payload's alignment is checked, and the heap
 * is walked (tb_malloc_check: sizes, flags, footers, merges, bins) every
 * 10000 operations. Then the case behind the finding, timed: 40000 small
 * blocks freed, then malloc/free churn.
 *
 *   gcc -O2 -g -Wall tools/agent/mallochost/mallochost.c -o /c/t/mallochost
 *
 * Prints "mallochost: PASS" and exits 0, or names the first problem. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#define LIBTOBY_MALLOC_HOST 1
#define malloc         tb_malloc
#define free           tb_free
#define realloc        tb_realloc
#define calloc         tb_calloc
#define posix_memalign tb_posix_memalign
#define sbrk           tb_sbrk
static void *tb_sbrk(long inc);
#include "../../../libtoby/src/malloc.c"
#undef malloc
#undef free
#undef realloc
#undef calloc
#undef posix_memalign
#undef sbrk

/* ---- the stub break: a big arena, a gap now and then ---- */
#define ARENA (512u << 20)
static char *g_arena, *g_brk;
static char *g_starts[4096];
static int   g_nstarts;
static uint64_t g_rng = 88172645463325252ULL;
static uint64_t rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}
static bool g_gaps = true;
static void *tb_sbrk(long inc) {
    if (!g_arena) {
        g_arena = (char *)malloc(ARENA + 4096);
        g_brk = (char *)(((uintptr_t)g_arena + 4095) & ~(uintptr_t)4095);
    }
    if (g_gaps && rnd() % 4 == 0) g_brk += 4096 * (1 + rnd() % 3);  /* a gap */
    if (g_brk + inc > g_arena + ARENA) { errno = ENOMEM; return (void *)-1; }
    char *old = g_brk;
    bool continues = g_last_fence && old == (char *)g_last_fence + 16;
    if (!continues && g_nstarts < 4096) g_starts[g_nstarts++] = old;
    g_brk += inc;
    return old;
}

static int fail(const char *what, long i) {
    printf("mallochost: FAIL %s (op %ld)\n", what, i);
    return 1;
}

struct blk { unsigned char *p; size_t n; unsigned char tag; };
#define NB 4000

static bool intact(struct blk *b) {
    for (size_t k = 0; k < b->n; k++)
        if (b->p[k] != (unsigned char)(b->tag + k)) return false;
    return true;
}
static void fill(struct blk *b) {
    for (size_t k = 0; k < b->n; k++) b->p[k] = (unsigned char)(b->tag + k);
}
static size_t rsize(void) {
    uint64_t r = rnd() % 100;
    if (r < 70) return 1 + rnd() % 64;            /* most are small */
    if (r < 90) return 1 + rnd() % 1024;
    if (r < 98) return 1 + rnd() % 16384;
    return 1 + rnd() % 300000;                    /* a few large */
}

int main(void) {
    static struct blk b[NB];
    long ops = 400000;
    for (long i = 0; i < ops; i++) {
        struct blk *x = &b[rnd() % NB];
        uint64_t what = rnd() % 10;
        if (!x->p) {
            x->n = rsize();
            x->tag = (unsigned char)rnd();
            x->p = (what == 0) ? (unsigned char *)tb_calloc(1, x->n)
                               : (unsigned char *)tb_malloc(x->n);
            if (!x->p) return fail("malloc returned NULL", i);
            if ((uintptr_t)x->p & 15) return fail("misaligned payload", i);
            if (what == 0)
                for (size_t k = 0; k < x->n; k++)
                    if (x->p[k]) return fail("calloc not zero", i);
            fill(x);
        } else if (what < 6) {
            if (!intact(x)) return fail("block overwritten", i);
            tb_free(x->p);
            x->p = 0;
        } else {
            if (!intact(x)) return fail("block overwritten before realloc", i);
            size_t nn = (what == 9) ? 1 + rnd() % 8 : rsize();
            unsigned char *q = (unsigned char *)tb_realloc(x->p, nn);
            if (!q) return fail("realloc returned NULL", i);
            if ((uintptr_t)q & 15) return fail("misaligned realloc", i);
            size_t keep = nn < x->n ? nn : x->n;
            for (size_t k = 0; k < keep; k++)
                if (q[k] != (unsigned char)(x->tag + k))
                    return fail("realloc lost bytes", i);
            x->p = q;
            x->n = nn;
            fill(x);
        }
        if (i % 10000 == 0) {
            int bad = tb_malloc_check(g_starts, g_nstarts);
            if (bad) { printf("  %d heap problems\n", bad); return fail("heap check", i); }
        }
    }
    for (int k = 0; k < NB; k++) {
        if (b[k].p && !intact(&b[k])) return fail("block overwritten at end", k);
        tb_free(b[k].p);
        b[k].p = 0;
    }
    int bad = tb_malloc_check(g_starts, g_nstarts);
    if (bad) { printf("  %d heap problems\n", bad); return fail("final heap check", -1); }
    /* everything freed: each region is one free chunk again */
    int regions_free = 0;
    for (int r = 0; r < g_nstarts; r++) {
        struct chunk *c = (struct chunk *)g_starts[r];
        if (!(c->head & INUSE) && (next_of(c)->head & INUSE) && csize(next_of(c)) == 0)
            regions_free++;
    }
    printf("random: %ld ops, %d regions, %d fully merged back\n", ops, g_nstarts, regions_free);
    if (regions_free != g_nstarts) return fail("a region did not merge back to one chunk", -1);

    /* the finding's case: 40000 small strings freed, then churn */
    g_gaps = false;
    static void *s[40000];
    clock_t t0 = clock();
    for (int k = 0; k < 40000; k++) { s[k] = tb_malloc(10 + k % 20); memset(s[k], 'x', 10); }
    for (int k = 0; k < 40000; k++) tb_free(s[k]);
    clock_t t1 = clock();
    void *v[64];
    for (long k = 0; k < 2000000; k++) {
        int j = (int)(k % 64);
        if (k >= 64) tb_free(v[j]);
        v[j] = tb_malloc(16 + (k % 48));
    }
    for (int j = 0; j < 64; j++) tb_free(v[j]);
    clock_t t2 = clock();
    printf("40000 small blocks + free: %.3f s; 2M malloc/free after: %.3f s\n",
           (double)(t1 - t0) / CLOCKS_PER_SEC, (double)(t2 - t1) / CLOCKS_PER_SEC);
    bad = tb_malloc_check(g_starts, g_nstarts);
    if (bad) return fail("heap check after churn", -1);

    /* MAPPED chunks (finding 215): 64 MiB and up are a map of their own --
     * written at both ends, grown by realloc with the bytes kept, zero
     * from calloc without being touched, and gone on free, the heap's
     * regions untouched by any of it */
    {
        size_t big = (size_t)100 << 20;
        unsigned char *m = tb_malloc(big);
        if (!m || ((uintptr_t)m & 15)) return fail("mapped malloc", -1);
        m[0] = 0x5a; m[big - 1] = 0xa5;
        unsigned char *m2 = tb_realloc(m, big * 2);
        if (!m2 || m2[0] != 0x5a || m2[big - 1] != 0xa5) return fail("mapped realloc", -1);
        m2[big * 2 - 1] = 1;
        tb_free(m2);
        unsigned char *z = tb_calloc(1, (size_t)80 << 20);
        if (!z || z[0] || z[(40 << 20)] || z[(80 << 20) - 1]) return fail("mapped calloc", -1);
        tb_free(z);
        unsigned char *s = tb_malloc(4096);              /* the heap goes on */
        unsigned char *g = tb_realloc(s, (size_t)70 << 20);
        if (!g) return fail("heap chunk realloc'd to a map", -1);
        tb_free(g);
        if (tb_malloc_check(g_starts, g_nstarts)) return fail("heap check after maps", -1);
        puts("mapped: 100 MiB, realloc to 200 MiB, calloc 80 MiB, heap to map: ok");
    }
    puts("mallochost: PASS");
    return 0;
}
