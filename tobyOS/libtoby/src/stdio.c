/* libtoby/src/stdio.c -- printf engine + FILE * streams.
 *
 * ---- printf engine ------------------------------------------------
 *
 * One central function -- do_format() -- walks the format string and
 * pushes bytes through a `sink` callback. Each consumer of the format
 * engine plugs in its own sink:
 *
 *   snprintf()  -> bounded-buffer sink (truncates on overflow)
 *   sprintf()   -> unbounded-buffer sink
 *   printf()    -> fd sink that calls write(1, ...)
 *   fprintf()   -> FILE * sink that ultimately calls write(fileno, ...)
 *
 * Supported conversions: %c %s %d %i %u %x %X %o %p %ld %li %lu %lx %zu %%
 * Flags:    -  +  (space)  #  0
 * Width:    nnn or *
 * Precision: .nnn or .*  (max chars for %s; min digits for %d/%u/%x/...)
 *
 * No floating-point conversions yet -- our sample programs and the
 * planned M25E ports (cat/ls/echo/wc/sh) don't use them.
 *
 * ---- FILE * streams ----------------------------------------------
 *
 * Each FILE * is a tiny struct:
 *
 *     fd          underlying kernel descriptor
 *     pushback    >=0 if a byte is queued by ungetc(), else -1
 *     eof         set when read sees EOF
 *     err         set on syscall failure
 *     is_static   1 for stdin/stdout/stderr (don't free in fclose)
 *
 * No buffering today -- writes go straight to write(), reads pull
 * one byte at a time from read(). This keeps the implementation
 * tiny and matches what our sample programs need. A future M25E
 * may add 4 KiB write buffers if profiling shows they're worth it. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include "libtoby_internal.h"

/* ============================================================
 *  Raw write helper (used by abort path, printf, etc.)
 * ============================================================ */

long __toby_raw_write(int fd, const void *buf, size_t n) {
    /* Loop in case the kernel does a short write (it currently
     * doesn't, but we don't want to bake that assumption in). */
    const char *p = (const char *)buf;
    size_t left = n;
    while (left > 0) {
        long r = toby_sc3(ABI_SYS_WRITE, fd, (long)(uintptr_t)p, (long)left);
        if (r <= 0) return -1;
        p += r;
        left -= (size_t)r;
    }
    return (long)n;
}

/* ============================================================
 *  do_format() -- the printf engine
 * ============================================================ */

typedef int (*sink_fn)(void *ctx, const char *s, size_t n);

static int emit(sink_fn sink, void *ctx, int *total, const char *s, size_t n) {
    int rc = sink(ctx, s, n);
    *total += (int)n;
    return rc;
}

static int emit_ch(sink_fn sink, void *ctx, int *total, char c) {
    return emit(sink, ctx, total, &c, 1);
}

/* fmtflags packs the parsed flag set + width/precision so the actual
 * conversion functions don't need 6 separate parameters. */
struct fmtflags {
    int  width;
    int  precision;
    bool left;
    bool plus;
    bool space;
    bool hash;
    bool zero;
    bool has_precision;
    char length;        /* 'h', 'l', 'L', 'z', or 0 */
    char conv;
};

static int parse_int(const char **fmt) {
    int n = 0;
    while (**fmt >= '0' && **fmt <= '9') { n = n * 10 + (**fmt - '0'); (*fmt)++; }
    return n;
}

/* Format unsigned `v` in `base` into `buf` (right-aligned), returning
 * the number of digits written. Buffer is filled from the END so that
 * we can prefix easily. The returned length never exceeds 32. */
static int u_to_str(unsigned long v, int base, bool upper, char *buf, int buflen) {
    static const char *lo = "0123456789abcdef";
    static const char *up = "0123456789ABCDEF";
    const char *digits = upper ? up : lo;
    int n = 0;
    if (v == 0) {
        buf[buflen - 1] = '0';
        return 1;
    }
    while (v && n < buflen) {
        buf[buflen - 1 - n] = digits[v % (unsigned)base];
        v /= (unsigned)base;
        n++;
    }
    return n;
}

/* Pad the sink with `n` copies of `c`. */
static int pad(sink_fn sink, void *ctx, int *total, char c, int n) {
    while (n-- > 0) {
        if (emit_ch(sink, ctx, total, c) != 0) return -1;
    }
    return 0;
}

static int conv_string(sink_fn sink, void *ctx, int *total,
                       struct fmtflags *f, const char *s) {
    if (!s) s = "(null)";
    int len = (int)(f->has_precision ? strnlen(s, (size_t)f->precision) : strlen(s));
    int padn = f->width > len ? f->width - len : 0;
    if (!f->left && pad(sink, ctx, total, ' ', padn) != 0) return -1;
    if (emit(sink, ctx, total, s, (size_t)len) != 0) return -1;
    if ( f->left && pad(sink, ctx, total, ' ', padn) != 0) return -1;
    return 0;
}

static int conv_char(sink_fn sink, void *ctx, int *total,
                     struct fmtflags *f, int ch) {
    int padn = f->width > 1 ? f->width - 1 : 0;
    if (!f->left && pad(sink, ctx, total, ' ', padn) != 0) return -1;
    if (emit_ch(sink, ctx, total, (char)ch) != 0) return -1;
    if ( f->left && pad(sink, ctx, total, ' ', padn) != 0) return -1;
    return 0;
}

static int conv_int(sink_fn sink, void *ctx, int *total,
                    struct fmtflags *f, long sv, unsigned long uv,
                    bool is_signed, int base, bool upper) {
    char numbuf[32];
    char prefix[3];
    int  prefix_len = 0;
    bool negative = false;

    if (is_signed && sv < 0) {
        uv = (unsigned long)(-sv);
        negative = true;
    } else if (is_signed) {
        uv = (unsigned long)sv;
    }

    int ndig = u_to_str(uv, base, upper, numbuf, (int)sizeof(numbuf));
    char *digits = numbuf + sizeof(numbuf) - ndig;

    /* Sign / space / + */
    if (negative)              prefix[prefix_len++] = '-';
    else if (is_signed && f->plus)  prefix[prefix_len++] = '+';
    else if (is_signed && f->space) prefix[prefix_len++] = ' ';

    /* # flag: 0x for hex, 0 for octal */
    if (f->hash && uv != 0) {
        if (base == 16) {
            prefix[prefix_len++] = '0';
            prefix[prefix_len++] = upper ? 'X' : 'x';
        } else if (base == 8) {
            prefix[prefix_len++] = '0';
        }
    }

    /* Apply precision (min digit count). */
    int prec_pad = 0;
    if (f->has_precision && f->precision > ndig) prec_pad = f->precision - ndig;

    int total_len = prefix_len + prec_pad + ndig;
    int width_pad = f->width > total_len ? f->width - total_len : 0;
    /* If 0-flag is set AND no precision AND not left-aligned, pad with 0. */
    bool zero_pad = f->zero && !f->left && !f->has_precision;

    if (!f->left && !zero_pad) {
        if (pad(sink, ctx, total, ' ', width_pad) != 0) return -1;
    }
    if (emit(sink, ctx, total, prefix, (size_t)prefix_len) != 0) return -1;
    if (zero_pad) {
        if (pad(sink, ctx, total, '0', width_pad) != 0) return -1;
    }
    if (pad(sink, ctx, total, '0', prec_pad) != 0) return -1;
    if (emit(sink, ctx, total, digits, (size_t)ndig) != 0) return -1;
    if (f->left) {
        if (pad(sink, ctx, total, ' ', width_pad) != 0) return -1;
    }
    return 0;
}

static int do_format(sink_fn sink, void *ctx, const char *fmt, va_list ap) {
    int total = 0;
    while (*fmt) {
        if (*fmt != '%') {
            /* Run of literal chars, batched into one sink call. */
            const char *start = fmt;
            while (*fmt && *fmt != '%') fmt++;
            if (emit(sink, ctx, &total, start, (size_t)(fmt - start)) != 0) return -1;
            continue;
        }
        fmt++;     /* skip '%' */

        struct fmtflags f = {0};
        f.precision = -1;

        /* flags */
        for (;;) {
            switch (*fmt) {
            case '-': f.left  = true; fmt++; continue;
            case '+': f.plus  = true; fmt++; continue;
            case ' ': f.space = true; fmt++; continue;
            case '#': f.hash  = true; fmt++; continue;
            case '0': f.zero  = true; fmt++; continue;
            }
            break;
        }

        /* width */
        if (*fmt == '*') { f.width = va_arg(ap, int); fmt++; }
        else if (*fmt >= '0' && *fmt <= '9') f.width = parse_int(&fmt);

        /* precision */
        if (*fmt == '.') {
            fmt++;
            f.has_precision = true;
            if (*fmt == '*') { f.precision = va_arg(ap, int); fmt++; }
            else f.precision = parse_int(&fmt);
        }

        /* length modifier */
        if (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 'L') {
            f.length = *fmt;
            fmt++;
        }

        f.conv = *fmt ? *fmt++ : 0;
        if (!f.conv) break;

        switch (f.conv) {
        case '%': if (emit_ch(sink, ctx, &total, '%') != 0) return -1; break;
        case 'c': if (conv_char(sink, ctx, &total, &f, va_arg(ap, int)) != 0) return -1; break;
        case 's': if (conv_string(sink, ctx, &total, &f, va_arg(ap, const char *)) != 0) return -1; break;

        case 'd': case 'i': {
            long sv = (f.length == 'l') ? va_arg(ap, long)
                    : (f.length == 'z') ? (long)va_arg(ap, size_t)
                                        : (long)va_arg(ap, int);
            if (conv_int(sink, ctx, &total, &f, sv, 0, true, 10, false) != 0) return -1;
            break;
        }
        case 'u': {
            unsigned long uv = (f.length == 'l') ? va_arg(ap, unsigned long)
                             : (f.length == 'z') ? (unsigned long)va_arg(ap, size_t)
                                                 : (unsigned long)va_arg(ap, unsigned int);
            if (conv_int(sink, ctx, &total, &f, 0, uv, false, 10, false) != 0) return -1;
            break;
        }
        case 'x': case 'X': {
            unsigned long uv = (f.length == 'l') ? va_arg(ap, unsigned long)
                             : (f.length == 'z') ? (unsigned long)va_arg(ap, size_t)
                                                 : (unsigned long)va_arg(ap, unsigned int);
            if (conv_int(sink, ctx, &total, &f, 0, uv, false, 16, f.conv == 'X') != 0) return -1;
            break;
        }
        case 'o': {
            unsigned long uv = (f.length == 'l') ? va_arg(ap, unsigned long)
                             : (f.length == 'z') ? (unsigned long)va_arg(ap, size_t)
                                                 : (unsigned long)va_arg(ap, unsigned int);
            if (conv_int(sink, ctx, &total, &f, 0, uv, false, 8, false) != 0) return -1;
            break;
        }
        case 'p': {
            unsigned long uv = (unsigned long)(uintptr_t)va_arg(ap, void *);
            f.hash = true;       /* always print 0x */
            if (conv_int(sink, ctx, &total, &f, 0, uv, false, 16, false) != 0) return -1;
            break;
        }
        default:
            /* Unknown conversion: emit literally. */
            if (emit_ch(sink, ctx, &total, '%') != 0) return -1;
            if (emit_ch(sink, ctx, &total, f.conv) != 0) return -1;
            break;
        }
    }
    return total;
}

/* ============================================================
 *  Sinks
 * ============================================================ */

struct buf_sink_ctx {
    char  *out;
    size_t cap;
    size_t pos;
};

static int buf_sink(void *ctx, const char *s, size_t n) {
    struct buf_sink_ctx *bs = (struct buf_sink_ctx *)ctx;
    /* Always advance pos so the engine reports the would-have-been
     * length correctly; just don't write past cap. */
    if (bs->pos < bs->cap) {
        size_t avail = bs->cap - bs->pos;
        size_t copy  = n < avail ? n : avail;
        memcpy(bs->out + bs->pos, s, copy);
    }
    bs->pos += n;
    return 0;
}

struct fd_sink_ctx { int fd; };

static int fd_sink(void *ctx, const char *s, size_t n) {
    struct fd_sink_ctx *fs = (struct fd_sink_ctx *)ctx;
    if (__toby_raw_write(fs->fd, s, n) < 0) return -1;
    return 0;
}

/* ============================================================
 *  Public formatted-output API
 * ============================================================ */

int vsnprintf(char *out, size_t cap, const char *fmt, va_list ap) {
    struct buf_sink_ctx bs = { .out = out, .cap = cap, .pos = 0 };
    int n = do_format(buf_sink, &bs, fmt, ap);
    if (cap > 0) {
        size_t terminator_at = bs.pos < cap ? bs.pos : cap - 1;
        out[terminator_at] = '\0';
    }
    return n;
}

int snprintf(char *out, size_t cap, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}

int vsprintf(char *out, const char *fmt, va_list ap) {
    return vsnprintf(out, (size_t)-1 / 2, fmt, ap);
}

int sprintf(char *out, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vsprintf(out, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *fmt, va_list ap) {
    struct fd_sink_ctx fs = { .fd = 1 };
    return do_format(fd_sink, &fs, fmt, ap);
}

int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

/* ============================================================
 *  FILE * layer
 * ============================================================ */

struct __libtoby_FILE {
    int  fd;
    int  pushback;       /* -1 = none */
    int  eof;
    int  err;
    int  is_static;      /* don't free in fclose() if 1 */
};

static struct __libtoby_FILE g_stdin  = { 0, -1, 0, 0, 1 };
static struct __libtoby_FILE g_stdout = { 1, -1, 0, 0, 1 };
static struct __libtoby_FILE g_stderr = { 2, -1, 0, 0, 1 };

FILE *stdin  = &g_stdin;
FILE *stdout = &g_stdout;
FILE *stderr = &g_stderr;

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    if (!f) return -1;
    struct fd_sink_ctx fs = { .fd = f->fd };
    int n = do_format(fd_sink, &fs, fmt, ap);
    if (n < 0) f->err = 1;
    return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int fputs(const char *s, FILE *f) {
    if (!f) return EOF;
    size_t n = strlen(s);
    if (__toby_raw_write(f->fd, s, n) < 0) { f->err = 1; return EOF; }
    return (int)n;
}

int puts(const char *s) {
    int r = fputs(s, stdout);
    if (r < 0) return EOF;
    char nl = '\n';
    if (__toby_raw_write(stdout->fd, &nl, 1) < 0) { stdout->err = 1; return EOF; }
    return r + 1;
}

int fputc(int c, FILE *f) {
    if (!f) return EOF;
    char ch = (char)c;
    if (__toby_raw_write(f->fd, &ch, 1) < 0) { f->err = 1; return EOF; }
    return (unsigned char)ch;
}

int putc   (int c, FILE *f) { return fputc(c, f); }
int putchar(int c)          { return fputc(c, stdout); }

int fgetc(FILE *f) {
    if (!f) return EOF;
    if (f->pushback >= 0) { int c = f->pushback; f->pushback = -1; return c; }
    unsigned char c;
    long r = toby_sc3(ABI_SYS_READ, f->fd, (long)(uintptr_t)&c, 1);
    if (r <= 0) {
        if (r < 0) f->err = 1;
        f->eof = 1;
        return EOF;
    }
    return c;
}

int getc(FILE *f)   { return fgetc(f); }
int getchar(void)   { return fgetc(stdin); }

char *fgets(char *s, int cap, FILE *f) {
    if (!s || cap <= 0 || !f) return 0;
    int i = 0;
    while (i < cap - 1) {
        int c = fgetc(f);
        if (c == EOF) {
            if (i == 0) return 0;
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

size_t fread(void *buf, size_t sz, size_t n, FILE *f) {
    if (!f || sz == 0 || n == 0) return 0;
    size_t total = sz * n;
    char *out = (char *)buf;
    size_t got = 0;
    while (got < total) {
        long r = toby_sc3(ABI_SYS_READ, f->fd,
                          (long)(uintptr_t)(out + got),
                          (long)(total - got));
        if (r < 0) { f->err = 1; break; }
        if (r == 0) { f->eof = 1; break; }
        got += (size_t)r;
    }
    return got / sz;
}

size_t fwrite(const void *buf, size_t sz, size_t n, FILE *f) {
    if (!f || sz == 0 || n == 0) return 0;
    size_t total = sz * n;
    if (__toby_raw_write(f->fd, buf, total) < 0) { f->err = 1; return 0; }
    return n;
}

int fflush(FILE *f) { (void)f; return 0; }   /* unbuffered today */

int fseek(FILE *f, long off, int whence) {
    if (!f) return -1;
    long r = toby_sc3(ABI_SYS_LSEEK, f->fd, off, whence);
    if (r < 0) { f->err = 1; errno = (int)(-r); return -1; }
    f->eof = 0;
    return 0;
}

long ftell(FILE *f) {
    if (!f) return -1;
    long r = toby_sc3(ABI_SYS_LSEEK, f->fd, 0, SEEK_CUR);
    if (r < 0) { f->err = 1; errno = (int)(-r); return -1; }
    return r;
}

void rewind(FILE *f) { fseek(f, 0, SEEK_SET); if (f) { f->eof = 0; f->err = 0; } }

int  feof    (FILE *f) { return f ? f->eof : 0; }
int  ferror  (FILE *f) { return f ? f->err : 0; }
void clearerr(FILE *f) { if (f) { f->eof = 0; f->err = 0; } }
int  fileno  (FILE *f) { return f ? f->fd : -1; }

/* fopen mode parser. Matches the C standard subset we care about: r,
 * w, a, rb, wb, ab, r+, w+, a+ (+/- "b"). Anything else -> EINVAL. */
static int fopen_mode_to_flags(const char *mode) {
    if (!mode || !*mode) return -1;
    int flags = -1;
    int update = 0;
    char m = mode[0];
    /* skip 'b' anywhere -- we treat text and binary identically */
    for (int i = 1; mode[i]; i++) {
        if (mode[i] == '+') update = 1;
        else if (mode[i] != 'b') return -1;
    }
    switch (m) {
    case 'r': flags = update ? O_RDWR : O_RDONLY; break;
    case 'w': flags = (update ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
    case 'a': flags = (update ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
    default:  return -1;
    }
    return flags;
}

FILE *fopen(const char *path, const char *mode) {
    int flags = fopen_mode_to_flags(mode);
    if (flags < 0) { errno = EINVAL; return 0; }
    int fd = open(path, flags, 0644);
    if (fd < 0) return 0;
    FILE *f = (FILE *)malloc(sizeof(*f));
    if (!f) { close(fd); errno = ENOMEM; return 0; }
    f->fd = fd;
    f->pushback = -1;
    f->eof = 0;
    f->err = 0;
    f->is_static = 0;
    return f;
}

int fclose(FILE *f) {
    if (!f) return EOF;
    int rc = close(f->fd);
    if (!f->is_static) free(f);
    return rc;
}

void perror(const char *prefix) {
    if (prefix && *prefix) {
        fputs(prefix, stderr);
        fputs(": ", stderr);
    }
    fputs(strerror(errno), stderr);
    fputc('\n', stderr);
}
