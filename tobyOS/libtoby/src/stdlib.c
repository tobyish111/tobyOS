/* libtoby/src/stdlib.c -- the C stdlib core: malloc family, exit/abort,
 * atoi/strtol, getenv, and rand.
 *
 * ---- Allocator design ----------------------------------------------
 *
 * Classic first-fit free-list malloc, lazy-initialised on the first
 * call to malloc(). All bookkeeping is in-band: each chunk carries a
 * 16-byte header so that the returned payload is 16-byte aligned (which
 * matches both x86-64 SysV's stack alignment and the worst-case scalar
 * alignment libtoby hands out).
 *
 *   chunk layout (16 bytes header + payload):
 *
 *      +0  size_t           size_with_flags     <- payload size in low bits,
 *                                                  IN_USE in bit 0
 *      +8  struct chunk *   next_free           <- only valid when free
 *                                                  (allocated chunks scribble
 *                                                  a magic word here so a
 *                                                  use-after-free that walks
 *                                                  the list trips an assert)
 *
 * Free list: singly-linked, head-inserted on free. When malloc walks
 * it for first-fit it does an opportunistic forward-coalesce for any
 * adjacent free chunks it encounters -- O(1) per chunk because we know
 * the next chunk's address (current chunk + sizeof(header) + size).
 *
 * Heap growth: we hold a "wilderness" pointer (the highest brk we've
 * asked the kernel for). When the free list can't satisfy a request,
 * we sbrk() max(needed_chunk_size, 16 KiB) bytes and carve a fresh
 * chunk out of the new region. We never shrink the heap (sbrk
 * negatives) -- the kernel's brk implementation supports it but it
 * complicates coalescing for very little real-world win in our small
 * sample programs.
 *
 * Thread safety: tobyOS doesn't have user threads yet, so the global
 * free list head is used unlocked. */

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include "libtoby_internal.h"

/* ---- exit + atexit ------------------------------------------------ */

#define ATEXIT_MAX 32
static void (*g_atexit_fns[ATEXIT_MAX])(void);
static int    g_atexit_n = 0;

int atexit(void (*f)(void)) {
    if (g_atexit_n >= ATEXIT_MAX) return -1;
    g_atexit_fns[g_atexit_n++] = f;
    return 0;
}

void __toby_run_atexit(void) {
    /* LIFO order, per C11. */
    while (g_atexit_n > 0) {
        void (*f)(void) = g_atexit_fns[--g_atexit_n];
        if (f) f();
    }
}

void exit(int code) {
    __toby_run_atexit();
    _exit(code);
}

void abort(void) {
    /* Skip atexit handlers (per POSIX) -- the program is in an
     * undefined state already, running them might make things worse. */
    _exit(134);
    for (;;) { /* _exit doesn't return; quiet the noreturn diagnostic */ }
}

/* ---- ASCII -> integer -------------------------------------------- */

int atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

long strtol(const char *s, char **endp, int base) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2; base = 16;
    } else if (base == 0 && s[0] == '0') {
        s++; base = 8;
    } else if (base == 0) {
        base = 10;
    }
    long v = 0;
    while (*s) {
        int d = hexval(*s);
        if (d < 0 || d >= base) break;
        v = v * base + d;
        s++;
    }
    if (endp) *endp = (char *)s;
    return sign * v;
}

unsigned long strtoul(const char *s, char **endp, int base) {
    /* For our needs strtol's body works -- callers that care about the
     * full unsigned range use strtoull (not provided). */
    long v = strtol(s, endp, base);
    return (unsigned long)v;
}

/* ---- environment ---------------------------------------------------
 *
 * `environ` is the canonical pointer that user code (and POSIX) reads.
 * crt0.S initialises it to `__toby_envp` (the stack-resident env array
 * the kernel packed for us). The first call to setenv/unsetenv/putenv
 * promotes the table to a heap-owned copy via env_take_ownership() so
 * we never have to mutate stack memory. Subsequent mutations work on
 * that heap copy.
 *
 * Memory ownership rules after promotion:
 *   - environ itself is malloc'd and grown via realloc.
 *   - Every "KEY=VALUE" string entry is malloc'd; freed on unsetenv
 *     or replace. Strings inherited from the original stack array are
 *     strdup'd into the heap during promotion so the model stays
 *     uniform (every entry is freeable).
 *   - putenv() is the deliberate exception: it stores the caller's
 *     pointer verbatim (POSIX requires it). We tag those entries in a
 *     separate bitmap so unsetenv/replace skips kfree on them. */

char **environ = 0;             /* set by crt0.S to __toby_envp */

static bool   g_env_owned = false;
static size_t g_env_cap   = 0;
static size_t g_env_count = 0;
static unsigned char *g_env_putenv_bits = 0;     /* 1 bit per slot */

static size_t envlen(char **a) {
    size_t n = 0;
    if (!a) return 0;
    while (a[n]) n++;
    return n;
}

static bool putenv_get(size_t i) {
    if (!g_env_putenv_bits || i >= g_env_cap) return false;
    return (g_env_putenv_bits[i >> 3] >> (i & 7)) & 1u;
}

static void putenv_set(size_t i, bool v) {
    if (!g_env_putenv_bits || i >= g_env_cap) return;
    unsigned char *b = &g_env_putenv_bits[i >> 3];
    unsigned mask = 1u << (i & 7);
    if (v) *b |= (unsigned char)mask;
    else   *b &= (unsigned char)~mask;
}

/* Migrate from the read-only stack-resident environ to a writable
 * heap-resident one. Returns 0 on success, -1 on OOM. Idempotent. */
static int env_take_ownership(void) {
    if (g_env_owned) return 0;

    size_t n   = envlen(environ);
    size_t cap = (n < 8 ? 16 : n + 8);
    char **arr = (char **)malloc((cap + 1) * sizeof(char *));
    if (!arr) return -1;
    size_t bits_bytes = (cap + 7) / 8;
    unsigned char *bits = (unsigned char *)malloc(bits_bytes);
    if (!bits) { free(arr); return -1; }
    memset(bits, 0, bits_bytes);

    for (size_t i = 0; i < n; i++) {
        const char *src = environ[i];
        size_t slen = strlen(src);
        char *dup = (char *)malloc(slen + 1);
        if (!dup) {
            for (size_t j = 0; j < i; j++) free(arr[j]);
            free(arr); free(bits);
            return -1;
        }
        memcpy(dup, src, slen + 1);
        arr[i] = dup;
    }
    arr[n] = 0;

    environ            = arr;
    g_env_cap          = cap;
    g_env_count        = n;
    g_env_putenv_bits  = bits;
    g_env_owned        = true;
    return 0;
}

/* Grow the heap-owned environ array so it can hold at least
 * `need` slots plus the NULL terminator. */
static int env_reserve(size_t need) {
    if (need < g_env_cap) return 0;
    size_t newcap = g_env_cap ? g_env_cap : 16;
    while (newcap < need) newcap *= 2;
    char **arr = (char **)realloc(environ, (newcap + 1) * sizeof(char *));
    if (!arr) return -1;
    size_t bits_bytes = (newcap + 7) / 8;
    unsigned char *bits = (unsigned char *)realloc(g_env_putenv_bits, bits_bytes);
    if (!bits) {
        environ = arr;       /* keep larger array around -- next try will reuse it */
        return -1;
    }
    /* Zero the freshly-grown bitmap region. */
    size_t old_bytes = (g_env_cap + 7) / 8;
    if (bits_bytes > old_bytes) {
        memset(bits + old_bytes, 0, bits_bytes - old_bytes);
    }
    environ            = arr;
    g_env_cap          = newcap;
    g_env_putenv_bits  = bits;
    return 0;
}

static size_t env_keylen(const char *kv) {
    const char *eq = kv;
    while (*eq && *eq != '=') eq++;
    return (size_t)(eq - kv);
}

static int env_find_index(const char *name, size_t nlen) {
    if (!environ) return -1;
    for (size_t i = 0; environ[i]; i++) {
        const char *kv = environ[i];
        if (strncmp(kv, name, nlen) == 0 && kv[nlen] == '=') {
            return (int)i;
        }
    }
    return -1;
}

char *getenv(const char *name) {
    if (!name || !environ) return 0;
    size_t n = strlen(name);
    int idx = env_find_index(name, n);
    if (idx < 0) return 0;
    return environ[idx] + n + 1;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || !value) { errno = EINVAL; return -1; }
    /* Reject keys containing '=' (POSIX). */
    for (const char *c = name; *c; c++) {
        if (*c == '=') { errno = EINVAL; return -1; }
    }
    if (env_take_ownership() < 0) { errno = ENOMEM; return -1; }

    size_t nlen = strlen(name);
    size_t vlen = strlen(value);
    int idx = env_find_index(name, nlen);
    if (idx >= 0 && !overwrite) return 0;

    char *kv = (char *)malloc(nlen + 1 + vlen + 1);
    if (!kv) { errno = ENOMEM; return -1; }
    memcpy(kv, name, nlen);
    kv[nlen] = '=';
    memcpy(kv + nlen + 1, value, vlen + 1);

    if (idx >= 0) {
        if (!putenv_get((size_t)idx)) free(environ[idx]);
        environ[idx] = kv;
        putenv_set((size_t)idx, false);
        return 0;
    }
    if (env_reserve(g_env_count + 1) < 0) { free(kv); errno = ENOMEM; return -1; }
    environ[g_env_count]     = kv;
    environ[g_env_count + 1] = 0;
    putenv_set(g_env_count, false);
    g_env_count++;
    return 0;
}

int unsetenv(const char *name) {
    if (!name || !*name) { errno = EINVAL; return -1; }
    for (const char *c = name; *c; c++) {
        if (*c == '=') { errno = EINVAL; return -1; }
    }
    if (env_take_ownership() < 0) { errno = ENOMEM; return -1; }
    size_t nlen = strlen(name);
    int idx = env_find_index(name, nlen);
    if (idx < 0) return 0;
    if (!putenv_get((size_t)idx)) free(environ[idx]);
    for (size_t i = (size_t)idx; i + 1 <= g_env_count; i++) {
        environ[i] = environ[i + 1];
        putenv_set(i, putenv_get(i + 1));
    }
    g_env_count--;
    return 0;
}

int putenv(char *string) {
    if (!string) { errno = EINVAL; return -1; }
    size_t klen = env_keylen(string);
    if (klen == 0 || string[klen] != '=') { errno = EINVAL; return -1; }
    if (env_take_ownership() < 0) { errno = ENOMEM; return -1; }
    int idx = env_find_index(string, klen);
    if (idx >= 0) {
        if (!putenv_get((size_t)idx)) free(environ[idx]);
        environ[idx] = string;
        putenv_set((size_t)idx, true);
        return 0;
    }
    if (env_reserve(g_env_count + 1) < 0) { errno = ENOMEM; return -1; }
    environ[g_env_count]     = string;
    environ[g_env_count + 1] = 0;
    putenv_set(g_env_count, true);
    g_env_count++;
    return 0;
}

int clearenv(void) {
    if (env_take_ownership() < 0) { errno = ENOMEM; return -1; }
    for (size_t i = 0; i < g_env_count; i++) {
        if (!putenv_get(i)) free(environ[i]);
        environ[i] = 0;
        putenv_set(i, false);
    }
    g_env_count = 0;
    if (environ) environ[0] = 0;
    return 0;
}

/* ---- xorshift64* PRNG --------------------------------------------- */

static unsigned long g_rng = 0xdeadbeefcafef00dULL;

void srand(unsigned seed) {
    g_rng = (unsigned long)seed | 0x100000000ULL;
}

int rand(void) {
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return (int)((g_rng * 0x2545F4914F6CDD1DULL) >> 33) & RAND_MAX;
}

/* ============================================================
 *  Allocator
 * ============================================================ */

#define ALIGN          16
#define HDR_SIZE       (sizeof(struct chunk))
#define MIN_USER_BYTES 16
#define MIN_TOTAL      (HDR_SIZE + MIN_USER_BYTES)
#define GROW_CHUNK     (16 * 1024)
#define IN_USE_BIT     ((size_t)1)
#define MAGIC_USED     ((void *)0xA110CA7EDF00DFACULL)

struct chunk {
    size_t        size_with_flags;     /* payload size + IN_USE bit */
    struct chunk *next_free;           /* used only when !IN_USE */
};

static struct chunk *g_free_head     = 0;
static char         *g_heap_end      = 0;   /* one-past-end of valid heap */

static inline size_t round_up(size_t n, size_t a) {
    return (n + (a - 1)) & ~(a - 1);
}

static inline size_t chunk_payload(struct chunk *c) {
    return c->size_with_flags & ~IN_USE_BIT;
}

static inline int chunk_is_used(struct chunk *c) {
    return (c->size_with_flags & IN_USE_BIT) != 0;
}

/* Brand-new heap region [base, base+bytes) -> a single big free chunk
 * stitched onto the list head. The wilderness end is bumped so the
 * coalescer doesn't walk past it. */
static void heap_admit(void *base, size_t bytes) {
    if (bytes < MIN_TOTAL) return;
    struct chunk *c = (struct chunk *)base;
    c->size_with_flags = bytes - HDR_SIZE;       /* payload = bytes - hdr */
    c->next_free = g_free_head;
    g_free_head = c;
    char *end = (char *)base + bytes;
    if (end > g_heap_end) g_heap_end = end;
}

/* sbrk() the wilderness up by `req_bytes`, then mint a chunk and add
 * it to the free list. Returns 0 on success, -1 on OOM. */
static int heap_grow(size_t req_bytes) {
    size_t want = req_bytes < GROW_CHUNK ? GROW_CHUNK : req_bytes;
    /* Round up to a page so subsequent calls keep good alignment. */
    want = round_up(want, 4096);
    void *base = sbrk((long)want);
    if (base == (void *)-1) return -1;
    heap_admit(base, want);
    return 0;
}

/* If `c` is followed in memory by another free chunk (according to the
 * size+next links walking from the free-list head we found `c` on),
 * splice them together. Doesn't try arbitrary-distance coalescing --
 * just immediate forward neighbour, which keeps the cost O(1). */
static void coalesce_forward(struct chunk *c) {
    /* Find pointer to next-in-memory candidate, but bail out if
     * walking the size header would step past the heap we own. */
    char *n_addr = (char *)c + HDR_SIZE + chunk_payload(c);
    if (n_addr + HDR_SIZE > g_heap_end) return;
    struct chunk *n = (struct chunk *)n_addr;
    /* Only coalesce if `n` is in our free list (i.e. not in use).
     * We detect that by walking the free list looking for `n`; for
     * typical workloads the free list is short so this stays cheap. */
    if (chunk_is_used(n)) return;
    /* Stitch n out of the free list. */
    struct chunk **link = &g_free_head;
    while (*link && *link != n) link = &(*link)->next_free;
    if (!*link) return;     /* not on list (must be in another arena) */
    *link = n->next_free;
    /* Merge sizes (header of n becomes payload of c). */
    c->size_with_flags = chunk_payload(c) + HDR_SIZE + chunk_payload(n);
    /* keep IN_USE bit at 0 */
}

/* Split `c` (free) so it carries exactly `payload_want` bytes; the
 * remainder (if big enough) becomes a new free chunk on the list. */
static void split(struct chunk *c, size_t payload_want) {
    size_t total = chunk_payload(c);
    if (total < payload_want + MIN_TOTAL) return;       /* too small to split */
    struct chunk *rest = (struct chunk *)((char *)c + HDR_SIZE + payload_want);
    rest->size_with_flags = total - payload_want - HDR_SIZE;
    rest->next_free = g_free_head;
    g_free_head = rest;
    c->size_with_flags = payload_want;
}

void *malloc(size_t n) {
    if (n == 0) n = 1;
    n = round_up(n, ALIGN);

    /* First-fit walk over the free list. */
    struct chunk **link = &g_free_head;
    while (*link) {
        struct chunk *c = *link;
        if (chunk_payload(c) >= n) {
            /* Remove from free list. */
            *link = c->next_free;
            split(c, n);
            c->size_with_flags = chunk_payload(c) | IN_USE_BIT;
            c->next_free = (struct chunk *)MAGIC_USED;
            return (void *)((char *)c + HDR_SIZE);
        }
        link = &c->next_free;
    }

    /* Free list empty / too fragmented: ask the kernel for more pages. */
    if (heap_grow(n + HDR_SIZE) != 0) {
        errno = ENOMEM;
        return 0;
    }
    /* Retry from the freshly admitted chunk. */
    return malloc(n);
}

void *calloc(size_t nmemb, size_t sz) {
    /* Overflow guard: refuse n*sz that would wrap. */
    if (sz != 0 && nmemb > ((size_t)-1) / sz) {
        errno = ENOMEM;
        return 0;
    }
    size_t bytes = nmemb * sz;
    void *p = malloc(bytes);
    if (p) memset(p, 0, bytes);
    return p;
}

void free(void *p) {
    if (!p) return;
    struct chunk *c = (struct chunk *)((char *)p - HDR_SIZE);
    /* Detect double free / corruption: c->next_free was set to
     * MAGIC_USED on alloc; if it's anything else we trip an assert. */
    if (c->next_free != (struct chunk *)MAGIC_USED) {
        /* Best-effort: write a diagnostic and abort. We use the raw
         * write helper to avoid going through the FILE* layer (which
         * itself uses malloc). */
        const char *m = "[libtoby] free(): double free or corruption\n";
        __toby_raw_write(2, m, strlen(m));
        abort();
    }
    c->size_with_flags = chunk_payload(c);          /* clear IN_USE */
    c->next_free = g_free_head;
    g_free_head = c;
    coalesce_forward(c);
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (n == 0) { free(p); return 0; }

    struct chunk *c = (struct chunk *)((char *)p - HDR_SIZE);
    size_t cur = chunk_payload(c);
    size_t want = round_up(n, ALIGN);

    if (want <= cur) {
        /* Shrink: split off the tail back into the free list. */
        if (cur >= want + MIN_TOTAL) {
            struct chunk *rest = (struct chunk *)((char *)c + HDR_SIZE + want);
            rest->size_with_flags = cur - want - HDR_SIZE;
            rest->next_free = g_free_head;
            g_free_head = rest;
            c->size_with_flags = want | IN_USE_BIT;
        }
        return p;
    }

    /* Try in-place grow if the next chunk in memory is free + big
     * enough. Bail out if it would lie past the wilderness. */
    char *nxt_addr = (char *)c + HDR_SIZE + chunk_payload(c);
    if (nxt_addr + HDR_SIZE > g_heap_end) goto fallback;
    struct chunk *nxt = (struct chunk *)nxt_addr;
    if (!chunk_is_used(nxt)) {
        size_t combined = cur + HDR_SIZE + chunk_payload(nxt);
        if (combined >= want) {
            /* Splice nxt out of free list. */
            struct chunk **link = &g_free_head;
            while (*link && *link != nxt) link = &(*link)->next_free;
            if (*link) {
                *link = nxt->next_free;
                c->size_with_flags = combined;
                /* Maybe split off remainder. */
                if (combined >= want + MIN_TOTAL) {
                    struct chunk *rest = (struct chunk *)((char *)c + HDR_SIZE + want);
                    rest->size_with_flags = combined - want - HDR_SIZE;
                    rest->next_free = g_free_head;
                    g_free_head = rest;
                    c->size_with_flags = want;
                }
                c->size_with_flags |= IN_USE_BIT;
                return p;
            }
        }
    }

fallback:
    /* Fall back to malloc + copy + free. */
    {
        void *q = malloc(n);
        if (!q) return 0;
        memcpy(q, p, cur);
        free(p);
        return q;
    }
}
