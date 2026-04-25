/* klibc.c -- minimal freestanding libc.
 *
 * The compiler may insert implicit calls to memcpy/memset/memmove/memcmp
 * (e.g. for struct copies or large local zero-init), so these MUST exist
 * with C linkage and the standard names. Implementations are deliberately
 * dumb byte-by-byte loops; we'll specialise once we have a profiler.
 */

#include <tobyos/klibc.h>

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i != 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint8_t  v = (uint8_t)c;
    for (size_t i = 0; i < n; i++) d[i] = v;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t ca = (uint8_t)a[i];
        uint8_t cb = (uint8_t)b[i];
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0)  return 0;
    }
    return 0;
}

/* ============================================================
 *  Tiny snprintf -- format strings for M26 devtest diagnostics.
 *
 *  Implements the same conversion set kprintf does:
 *      %s %c %d %u %lu %ld %x %lx %p %%
 *  Width (decimal) and zero-pad (leading 0) are supported between
 *  '%' and the conversion, e.g. "%08x".
 *
 *  We emit through an internal cursor so the output is always
 *  truncated cleanly at `cap - 1` and NUL-terminated. The return
 *  value is the number of bytes that WOULD have been written
 *  (matching ISO C snprintf semantics so callers can detect
 *  truncation via `rv >= cap`).
 * ============================================================ */

struct ksn_state {
    char  *dst;
    size_t cap;
    size_t pos;       /* total bytes that would be written  */
};

static void ksn_putc(struct ksn_state *s, char c) {
    if (s->dst && s->pos + 1 < s->cap) {
        s->dst[s->pos] = c;
    }
    s->pos++;
}

static void ksn_puts(struct ksn_state *s, const char *p) {
    if (!p) p = "(null)";
    while (*p) ksn_putc(s, *p++);
}

static void ksn_emit_uint(struct ksn_state *s, unsigned long v,
                          unsigned base, bool upper,
                          int width, bool zero_pad, bool left_align) {
    char buf[32];
    int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v) {
        unsigned d = (unsigned)(v % base);
        char c;
        if (d < 10) c = (char)('0' + d);
        else        c = (char)((upper ? 'A' : 'a') + d - 10);
        buf[n++] = c;
        v /= base;
    }
    if (!left_align) {
        while (n < width) buf[n++] = zero_pad ? '0' : ' ';
        while (n--) ksn_putc(s, buf[n]);
    } else {
        /* Emit digits first, then pad with spaces on the right. */
        int digits = n;
        while (n--) ksn_putc(s, buf[n]);
        for (int k = digits; k < width; k++) ksn_putc(s, ' ');
    }
}

static void ksn_emit_str(struct ksn_state *s, const char *p,
                         int width, bool left_align) {
    if (!p) p = "(null)";
    /* O(strlen) but kprintf's emit_str does the same -- bounded by
     * the caller's format-arg sizes, not by attacker input. */
    int len = (int)strlen(p);
    if (!left_align && len < width) {
        for (int k = 0; k < width - len; k++) ksn_putc(s, ' ');
    }
    while (*p) ksn_putc(s, *p++);
    if (left_align && len < width) {
        for (int k = 0; k < width - len; k++) ksn_putc(s, ' ');
    }
}

int kvsnprintf(char *dst, size_t cap, const char *fmt, va_list ap) {
    struct ksn_state s = { .dst = dst, .cap = cap, .pos = 0 };

    /* Format grammar (extended for M28 to match kvprintf):
     *   %[-][0][WIDTH][l|ll][specifier]
     * where specifier in { s c d i u x X p % }.
     *
     * On x86_64 sizeof(long)==sizeof(long long)==8, so %lu and %llu
     * fetch the same va_arg type. We coalesce both onto the same
     * "is_long" path. Without that the parser dropped through the
     * default branch on the second 'l' and the format string was
     * emitted literally -- which then mis-consumed va_args and could
     * page-fault on the next %s. */
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { ksn_putc(&s, *p); continue; }
        p++;

        bool zero_pad   = false;
        bool left_align = false;
        int  width      = 0;
        bool is_long    = false;

        if (*p == '-') { left_align = true; p++; }
        if (*p == '0' && !left_align) { zero_pad = true; p++; }
        while (*p >= '0' && *p <= '9') {
            width = width * 10 + (*p - '0');
            p++;
        }
        if (*p == 'l') {
            is_long = true; p++;
            /* Accept (and silently coalesce) `ll`. */
            if (*p == 'l') p++;
        }

        switch (*p) {
        case 's': ksn_emit_str(&s, va_arg(ap, const char *),
                               width, left_align); break;
        case 'c': ksn_putc(&s, (char)va_arg(ap, int));    break;
        case 'd': case 'i': {
            long v = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
            unsigned long u;
            if (v < 0) { ksn_putc(&s, '-'); u = (unsigned long)(-v); }
            else u = (unsigned long)v;
            ksn_emit_uint(&s, u, 10, false, width, zero_pad, left_align);
            break;
        }
        case 'u': {
            unsigned long u = is_long ? va_arg(ap, unsigned long)
                                      : (unsigned long)va_arg(ap, unsigned);
            ksn_emit_uint(&s, u, 10, false, width, zero_pad, left_align);
            break;
        }
        case 'x': {
            unsigned long u = is_long ? va_arg(ap, unsigned long)
                                      : (unsigned long)va_arg(ap, unsigned);
            ksn_emit_uint(&s, u, 16, false, width, zero_pad, left_align);
            break;
        }
        case 'X': {
            unsigned long u = is_long ? va_arg(ap, unsigned long)
                                      : (unsigned long)va_arg(ap, unsigned);
            ksn_emit_uint(&s, u, 16, true, width, zero_pad, left_align);
            break;
        }
        case 'p': {
            unsigned long u = (unsigned long)(uintptr_t)va_arg(ap, void *);
            ksn_putc(&s, '0'); ksn_putc(&s, 'x');
            ksn_emit_uint(&s, u, 16, false, 16, true, false);
            break;
        }
        case '%': ksn_putc(&s, '%'); break;
        default:  ksn_putc(&s, '%'); if (*p) ksn_putc(&s, *p); break;
        }
    }

    if (cap > 0) {
        size_t term = (s.pos < cap) ? s.pos : (cap - 1);
        if (dst) dst[term] = '\0';
    }
    return (int)s.pos;
}

int ksnprintf(char *dst, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    return n;
}
