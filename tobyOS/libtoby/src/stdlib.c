/* libtoby/src/stdlib.c -- the C stdlib core: exit/abort, atoi/strtol,
 * getenv, and rand.
 *
 * The allocator lives in malloc.c (finding 107 rewrote it). */

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
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

unsigned long long strtoull(const char *s, char **endp, int base) {
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (*s == '+') s++;
    else if (*s == '-') s++;      /* C allows a sign; result wraps mod 2^64 */
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2; base = 16;
    } else if (base == 0 && s[0] == '0') {
        s++; base = 8;
    } else if (base == 0) {
        base = 10;
    }
    unsigned long long v = 0;
    while (*s) {
        int d = hexval(*s);
        if (d < 0 || d >= base) break;
        v = v * (unsigned)base + (unsigned)d;
        s++;
    }
    if (endp) *endp = (char *)s;
    return v;
}

unsigned long strtoul(const char *s, char **endp, int base) {
    return (unsigned long)strtoull(s, endp, base);
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
 *  qsort -- shell sort (simple, no recursion needed)
 * ============================================================ */

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *)) {
    if (nmemb < 2 || !base || !size) return;
    char *arr = (char *)base;
    char *tmp = (char *)__builtin_alloca(size);

    for (size_t gap = nmemb / 2; gap > 0; gap /= 2) {
        for (size_t i = gap; i < nmemb; i++) {
            memcpy(tmp, arr + i * size, size);
            size_t j = i;
            while (j >= gap && compar(arr + (j - gap) * size, tmp) > 0) {
                memcpy(arr + j * size, arr + (j - gap) * size, size);
                j -= gap;
            }
            memcpy(arr + j * size, tmp, size);
        }
    }
}

/* ============================================================
 *  strtod / atof -- string to double
 * ============================================================ */

double strtod(const char *s, char **endp) {
    if (!s) { if (endp) *endp = (char *)s; return 0.0; }

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;

    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }

    double val = 0.0;
    int got_digit = 0;

    while (*s >= '0' && *s <= '9') {
        val = val * 10.0 + (*s - '0');
        s++;
        got_digit = 1;
    }

    if (*s == '.') {
        s++;
        double frac = 0.1;
        while (*s >= '0' && *s <= '9') {
            val += (*s - '0') * frac;
            frac *= 0.1;
            s++;
            got_digit = 1;
        }
    }

    if (got_digit && (*s == 'e' || *s == 'E')) {
        s++;
        int exp_sign = 1;
        if (*s == '-') { exp_sign = -1; s++; }
        else if (*s == '+') { s++; }
        int exp_val = 0;
        while (*s >= '0' && *s <= '9') {
            exp_val = exp_val * 10 + (*s - '0');
            s++;
        }
        double mult = 1.0;
        for (int i = 0; i < exp_val; i++) {
            if (exp_sign > 0) mult *= 10.0;
            else mult *= 0.1;
        }
        val *= mult;
    }

    if (endp) *endp = (char *)s;
    return sign * val;
}

double atof(const char *s) { return strtod(s, 0); }

/* ============================================================
 *  bsearch
 * ============================================================ */

void *bsearch(const void *key, const void *base, size_t nmemb,
              size_t size, int (*compar)(const void *, const void *)) {
    const char *arr = (const char *)base;
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = compar(key, arr + mid * size);
        if (cmp < 0) hi = mid;
        else if (cmp > 0) lo = mid + 1;
        else return (void *)(arr + mid * size);
    }
    return 0;
}

/* ============================================================
 *  mkstemp -- create a temporary file from template
 * ============================================================ */

int mkstemp(char *tmpl) {
    if (!tmpl) { errno = EINVAL; return -1; }
    size_t len = strlen(tmpl);
    if (len < 6) { errno = EINVAL; return -1; }
    char *suffix = tmpl + len - 6;
    for (int i = 0; i < 6; i++) {
        if (suffix[i] != 'X') { errno = EINVAL; return -1; }
    }

    static unsigned long counter = 0;
    unsigned long val = (unsigned long)toby_sc0(ABI_SYS_CLOCK_MS) ^ (++counter * 6364136223846793005ULL);

    for (int attempt = 0; attempt < 100; attempt++) {
        static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        unsigned long v = val + (unsigned long)attempt;
        for (int i = 0; i < 6; i++) {
            suffix[i] = chars[v % 36];
            v /= 36;
        }
        int fd = open(tmpl, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0) return fd;
    }
    errno = EEXIST;
    return -1;
}

/* ============================================================
 *  realpath -- resolve to absolute path (simplified)
 * ============================================================ */

char *realpath(const char *path, char *resolved) {
    if (!path || !*path) { errno = EINVAL; return 0; }

    char *buf = resolved;
    if (!buf) {
        buf = (char *)malloc(256);
        if (!buf) { errno = ENOMEM; return 0; }
    }

    if (path[0] == '/') {
        size_t len = strlen(path);
        if (len >= 256) { if (!resolved) free(buf); errno = ENAMETOOLONG; return 0; }
        memcpy(buf, path, len + 1);
    } else {
        if (!getcwd(buf, 256)) { if (!resolved) free(buf); return 0; }
        size_t cwdlen = strlen(buf);
        size_t pathlen = strlen(path);
        if (cwdlen + 1 + pathlen >= 256) {
            if (!resolved) free(buf);
            errno = ENAMETOOLONG;
            return 0;
        }
        buf[cwdlen] = '/';
        memcpy(buf + cwdlen + 1, path, pathlen + 1);
    }
    return buf;
}

/* ============================================================
 *  abs / labs
 * ============================================================ */

int abs(int n) { return n < 0 ? -n : n; }
long labs(long n) { return n < 0 ? -n : n; }
