/* libtoby/src/malloc.c -- the allocator: malloc, free, realloc, calloc,
 * posix_memalign.
 *
 * ---- Why it was rewritten (finding 107) ------------------------------
 *
 * It was best-fit over ONE singly-linked free list: every malloc walked the
 * whole list, and every free walked it again to find the neighbour it might
 * merge with. A program that freed many small blocks paid for all of them
 * on every later allocation -- after a script unset a 20000-element array
 * (about 40000 strings), 200 `$( )` took /bin/tsh 0.67 s where they had
 * taken 0.45 s (bash: 0.46 both; logs/b17 f41frag). It is every native
 * program's allocator.
 *
 * ---- The design --------------------------------------------------------
 *
 * Segregated free lists with boundary tags -- the textbook shape:
 *
 *   chunk: a 16-byte header, then the payload (a multiple of 16, at least
 *   16), so a payload is 16-byte aligned as it always was.
 *
 *      +0  size_t head     payload size | PREV_INUSE (bit 1) | INUSE (bit 0)
 *      +8  void  *link     in use: MAGIC_USED (free() checks it: a double
 *                          free or a stray pointer aborts, as before);
 *                          free: the next chunk in its bin
 *      payload, when FREE:  +0 the previous chunk in its bin
 *                           last 8 bytes: the payload size (a footer)
 *
 *   PREV_INUSE and the footer let free() find the chunk before it and merge
 *   in O(1); the size finds the one after. Free chunks never touch: they
 *   merge as they are freed.
 *
 *   bins: payload 16..1024 in exact 16-byte classes (64 bins, so a small
 *   request is one list pop); above that a bin per power of two, searched
 *   best-fit within the bin. A bitmap of non-empty bins finds the next
 *   larger one in a couple of instructions.
 *
 *   regions: each sbrk() region ends in a FENCE -- a zero-size chunk marked
 *   in use -- so no merge walks off it. A region that continues the last
 *   one exactly takes the old fence as its first header and merges back.
 *
 *   MAPPED chunks (finding 215): a request of TC_MAP_MIN or more, or one
 *   the heap cannot grow for, is an anonymous mmap of its own, as glibc's
 *   are -- pages the kernel gives only when they are touched, so a 4 GiB
 *   buffer that is never filled costs nothing (stdbuf -o 4G: malloc said
 *   no where Linux's says yes). Such a chunk is marked MAPPED, belongs to
 *   no region and no bin, and is unmapped by free.
 *
 * Thread safety: unlocked, as it was. */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#ifndef LIBTOBY_MALLOC_HOST
#include "libtoby_internal.h"
#endif

#define ALIGN        16u
#define HDR          ((size_t)16)
#define MIN_PAYLOAD  ((size_t)16)
#define GROW_CHUNK   ((size_t)64 * 1024)
#define INUSE        ((size_t)1)
#define PREV_INUSE   ((size_t)2)
#define MAPPED       ((size_t)4)       /* a chunk of its own mmap (215) */
#define FLAGS        ((size_t)15)
#define TC_MAP_MIN   ((size_t)64 << 20) /* 64 MiB and up: mmap first */
#define MAGIC_USED   ((void *)0xA110CA7EDF00DFACULL)

#define NSMALL       64u            /* payload 16..1024 */
#define NBINS        128u

struct chunk {
    size_t        head;
    struct chunk *link;
};

static struct chunk *g_bin[NBINS];
static uint64_t      g_binmap[NBINS / 64];
static struct chunk *g_last_fence;  /* the fence of the newest region */

static inline size_t round_up(size_t n, size_t a) {
    return (n + (a - 1)) & ~(a - 1);
}
static inline size_t csize(const struct chunk *c) { return c->head & ~FLAGS; }
static inline char *payload_of(struct chunk *c) { return (char *)c + HDR; }
static inline struct chunk *next_of(struct chunk *c) {
    return (struct chunk *)((char *)c + HDR + csize(c));
}
/* A free chunk's previous-in-bin pointer and footer live in its payload. */
static inline struct chunk **prevp(struct chunk *c) {
    return (struct chunk **)payload_of(c);
}
static inline void set_footer(struct chunk *c) {
    *(size_t *)((char *)c + HDR + csize(c) - sizeof(size_t)) = csize(c);
}

static unsigned bin_index(size_t payload) {
    if (payload <= 1024) return (unsigned)(payload / 16u) - 1u;
    unsigned lg = 63u - (unsigned)__builtin_clzll((unsigned long long)payload);
    unsigned i = NSMALL + (lg - 10u);
    return i < NBINS ? i : NBINS - 1u;
}

static void bin_insert(struct chunk *c) {
    unsigned i = bin_index(csize(c));
    c->link = g_bin[i];
    *prevp(c) = 0;
    if (g_bin[i]) *prevp(g_bin[i]) = c;
    g_bin[i] = c;
    g_binmap[i / 64] |= (uint64_t)1 << (i % 64);
}

static void bin_unlink(struct chunk *c) {
    unsigned i = bin_index(csize(c));
    struct chunk *pv = *prevp(c), *nx = c->link;
    if (pv) pv->link = nx; else g_bin[i] = nx;
    if (nx) *prevp(nx) = pv;
    if (!g_bin[i]) g_binmap[i / 64] &= ~((uint64_t)1 << (i % 64));
}

/* The first non-empty bin at or above I, or NBINS. */
static unsigned bin_next(unsigned i) {
    while (i < NBINS) {
        uint64_t m = g_binmap[i / 64] >> (i % 64);
        if (m) return i + (unsigned)__builtin_ctzll(m);
        i = (i / 64 + 1) * 64;
    }
    return NBINS;
}

/* A free chunk C, its head already set (size, PREV_INUSE, INUSE clear):
 * merged with a free neighbour on either side, footed, binned. */
static void release(struct chunk *c) {
    struct chunk *n = next_of(c);
    if (!(n->head & INUSE)) {                     /* the one after */
        bin_unlink(n);
        c->head = (csize(c) + HDR + csize(n)) | (c->head & PREV_INUSE);
    }
    if (!(c->head & PREV_INUSE)) {                /* the one before */
        size_t psz = *(size_t *)((char *)c - sizeof(size_t));
        struct chunk *p = (struct chunk *)((char *)c - psz - HDR);
        bin_unlink(p);
        p->head = (psz + HDR + csize(c)) | (p->head & PREV_INUSE);
        c = p;
    }
    set_footer(c);
    next_of(c)->head &= ~PREV_INUSE;
    bin_insert(c);
}

/* A new region [BASE, BASE+BYTES) from sbrk: one free chunk and a fence. */
static void heap_add(char *base, size_t bytes) {
    char *end = base + bytes;
    char *start = (char *)round_up((uintptr_t)base, ALIGN);
    struct chunk *c;
    size_t prev_bit;
    if (g_last_fence && start == (char *)g_last_fence + HDR) {
        c = g_last_fence;                      /* continue the last region */
        prev_bit = c->head & PREV_INUSE;
    } else {
        if (end < start + 2 * HDR + MIN_PAYLOAD) return;
        c = (struct chunk *)start;
        prev_bit = PREV_INUSE;                 /* nothing before it */
    }
    struct chunk *fence = (struct chunk *)((uintptr_t)(end - HDR) & ~(uintptr_t)(ALIGN - 1));
    size_t payload = (size_t)((char *)fence - payload_of(c));
    fence->head = 0 | INUSE;                   /* PREV_INUSE: c is free */
    fence->link = MAGIC_USED;
    g_last_fence = fence;
    c->head = payload | prev_bit;
    release(c);
}

static int heap_grow(size_t need) {
    size_t want = need + 3 * HDR;
    if (want < GROW_CHUNK) want = GROW_CHUNK;
    want = round_up(want, 4096);
    void *base = sbrk((long)want);
    if (base == (void *)-1) return -1;
    heap_add((char *)base, want);
    return 0;
}

/* A free chunk of at least NEED bytes, out of its bin -- or 0. */
static struct chunk *bin_take(size_t need) {
    unsigned i = bin_index(need);
    if (i >= NSMALL) {                            /* best fit in its bin */
        struct chunk *best = 0;
        for (struct chunk *c = g_bin[i]; c; c = c->link) {
            size_t s = csize(c);
            if (s >= need && (!best || s < csize(best))) {
                best = c;
                if (s == need) break;
            }
        }
        if (best) { bin_unlink(best); return best; }
        i++;
    }
    i = bin_next(i);
    if (i >= NBINS) return 0;
    struct chunk *c = g_bin[i];                   /* every one here fits */
    bin_unlink(c);
    return c;
}

/* Hand out the free chunk C for NEED bytes: the rest, if a chunk's worth,
 * goes back free. */
static void *use(struct chunk *c, size_t need) {
    size_t s = csize(c);
    if (s >= need + HDR + MIN_PAYLOAD) {
        struct chunk *rest = (struct chunk *)(payload_of(c) + need);
        rest->head = (s - need - HDR) | PREV_INUSE;
        c->head = need | INUSE | (c->head & PREV_INUSE);
        release(rest);
    } else {
        c->head |= INUSE;
        next_of(c)->head |= PREV_INUSE;
    }
    c->link = MAGIC_USED;
    return payload_of(c);
}

/* A MAPPED chunk of NEED bytes, or 0 */
static void *map_chunk(size_t need) {
    size_t len = round_up(need + HDR, 4096);
    void *m = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return 0;
    struct chunk *c = (struct chunk *)m;
    c->head = (len - HDR) | INUSE | MAPPED;
    c->link = MAGIC_USED;
    return payload_of(c);
}

void *malloc(size_t n) {
    if (n > ((size_t)-1 >> 2)) { errno = ENOMEM; return 0; }
    size_t need = round_up(n ? n : 1, ALIGN);
    if (need < MIN_PAYLOAD) need = MIN_PAYLOAD;
    if (need >= TC_MAP_MIN) {
        void *m = map_chunk(need);
        if (m) return m;
    }
    struct chunk *c = bin_take(need);
    if (!c) {
        if (heap_grow(need) != 0) {
            void *m = map_chunk(need);         /* the heap is out: a map */
            if (m) return m;
            errno = ENOMEM;
            return 0;
        }
        c = bin_take(need);
        if (!c) { errno = ENOMEM; return 0; }
    }
    return use(c, need);
}

static void bad_pointer(const char *who) {
    const char *m = who;
#ifndef LIBTOBY_MALLOC_HOST
    __toby_raw_write(2, m, strlen(m));
#else
    (void)!write(2, m, strlen(m));
#endif
    abort();
}

void free(void *p) {
    if (!p) return;
    struct chunk *c = (struct chunk *)((char *)p - HDR);
    /* A double free or a pointer malloc never gave: its link is not the
     * mark an allocated chunk carries. */
    if (c->link != MAGIC_USED || !(c->head & INUSE))
        bad_pointer("[libtoby] free(): double free or corruption\n");
    c->link = 0;
    if (c->head & MAPPED) {
        munmap(c, csize(c) + HDR);
        return;
    }
    c->head &= ~INUSE;
    release(c);
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (n == 0) { free(p); return 0; }
    if (n > ((size_t)-1 >> 2)) { errno = ENOMEM; return 0; }
    struct chunk *c = (struct chunk *)((char *)p - HDR);
    if (c->link != MAGIC_USED || !(c->head & INUSE))
        bad_pointer("[libtoby] realloc(): invalid pointer\n");
    size_t cur = csize(c);
    size_t want = round_up(n, ALIGN);
    if (want < MIN_PAYLOAD) want = MIN_PAYLOAD;

    if (c->head & MAPPED) {                       /* its own map: kept, or moved */
        if (want <= cur) return p;
        void *q = malloc(n);
        if (!q) return 0;
        memcpy(q, p, cur);
        free(p);
        return q;
    }
    if (want <= cur) {                            /* shrink in place */
        if (cur >= want + HDR + MIN_PAYLOAD) {
            struct chunk *rest = (struct chunk *)(payload_of(c) + want);
            rest->head = (cur - want - HDR) | PREV_INUSE;
            c->head = want | INUSE | (c->head & PREV_INUSE);
            release(rest);
        }
        return p;
    }
    struct chunk *nx = next_of(c);                /* grow into a free next */
    if (!(nx->head & INUSE) && cur + HDR + csize(nx) >= want) {
        bin_unlink(nx);
        size_t total = cur + HDR + csize(nx);
        c->head = total | INUSE | (c->head & PREV_INUSE);
        next_of(c)->head |= PREV_INUSE;
        if (total >= want + HDR + MIN_PAYLOAD) {
            struct chunk *rest = (struct chunk *)(payload_of(c) + want);
            rest->head = (total - want - HDR) | PREV_INUSE;
            c->head = want | INUSE | (c->head & PREV_INUSE);
            release(rest);
        }
        return p;
    }
    void *q = malloc(n);
    if (!q) return 0;
    memcpy(q, p, cur);
    free(p);
    return q;
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
    (void)alignment;
    if (!memptr) return EINVAL;
    void *p = malloc(size);
    if (!p) return ENOMEM;
    *memptr = p;
    return 0;
}

void *calloc(size_t nmemb, size_t sz) {
    if (sz != 0 && nmemb > ((size_t)-1) / sz) {
        errno = ENOMEM;
        return 0;
    }
    size_t bytes = nmemb * sz;
    void *p = malloc(bytes);
    /* a fresh map is zero already: touching it all would make it real */
    if (p && !(((struct chunk *)((char *)p - HDR))->head & MAPPED))
        memset(p, 0, bytes);
    return p;
}

#ifdef LIBTOBY_MALLOC_HOST
/* For the host harness: walk every region from its first chunk to its
 * fence and check the invariants -- sizes, flags, footers, no two free
 * chunks adjacent, every free chunk in the bin its size says. Returns the
 * number of problems. */
int tb_malloc_check(char *const *starts, int nstarts) {
    int bad = 0;
    for (int r = 0; r < nstarts; r++) {
        struct chunk *c = (struct chunk *)starts[r];
        bool prev_free = false;
        if (!(c->head & PREV_INUSE)) bad++;
        for (int guard = 0; guard < 10000000; guard++) {
            size_t s = csize(c);
            bool used = (c->head & INUSE) != 0;
            if (used && s == 0) break;                         /* fence */
            if (s < MIN_PAYLOAD || (s & 15)) { bad++; break; }
            if (((c->head & PREV_INUSE) != 0) == prev_free) bad++;
            if (!used) {
                if (prev_free) bad++;                          /* unmerged */
                if (*(size_t *)((char *)c + HDR + s - 8) != s) bad++;
                unsigned i = bin_index(s);
                bool found = false;
                for (struct chunk *b = g_bin[i]; b; b = b->link)
                    if (b == c) { found = true; break; }
                if (!found) bad++;
            } else if (c->link != MAGIC_USED) {
                bad++;
            }
            prev_free = !used;
            c = next_of(c);
        }
    }
    return bad;
}
#endif
