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
 * Supported conversions: %c %s %d %i %u %x %X %o %p %ld %li %lu %lx %zu
 *                         %f %F %e %E %g %G %%
 * Flags:    -  +  (space)  #  0
 * Width:    nnn or *
 * Precision: .nnn or .*  (max chars for %s; min digits for %d/%u/%x/...;
 *                         fractional/significant digits for floats)
 *
 * Floating-point (%f/%e/%g) needs SSE -- both for double arithmetic and to
 * read the double vararg from the XMM save area on x86-64 -- so stdio.c is
 * compiled -msse (see the Makefile). It is not shortest-round-trip exact,
 * but correct for the magnitudes/precisions real apps use.
 *
 * ---- FILE * streams ----------------------------------------------
 *
 * Each FILE * is buffered -- see "FILE * layer" below for what that
 * changes (the per-character syscalls) and what it does not (the order in
 * which a program's output and its other effects are seen). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/wait.h>
#include "libtoby_internal.h"

/* Forward decl -- defined in process.c */
pid_t toby_spawn(const char *path, char *const argv[], char *const envp[],
                 int fd0, int fd1, int fd2);

/* ============================================================
 *  Raw write helper (used by abort path, printf, etc.)
 * ============================================================ */

/* __toby_raw_write lives with the FILE layer: it flushes pending output
 * before writing, so an abort message lands after what was printed. */

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

/* Emit a pre-rendered numeric body (no sign) with the given sign char,
 * honoring width / '-' (left) / '0' (zero) flags. Shared by conv_float. */
static int emit_num_padded(sink_fn sink, void *ctx, int *total,
                           struct fmtflags *f, char sign,
                           const char *num, int nlen) {
    int total_len = (sign ? 1 : 0) + nlen;
    int width_pad = f->width > total_len ? f->width - total_len : 0;
    bool zero_pad = f->zero && !f->left;
    if (!f->left && !zero_pad && pad(sink, ctx, total, ' ', width_pad) != 0) return -1;
    if (sign && emit_ch(sink, ctx, total, sign) != 0) return -1;
    if (zero_pad && pad(sink, ctx, total, '0', width_pad) != 0) return -1;
    if (emit(sink, ctx, total, num, (size_t)nlen) != 0) return -1;
    if (f->left && pad(sink, ctx, total, ' ', width_pad) != 0) return -1;
    return 0;
}

/* Floating-point conversions %f/%F %e/%E %g/%G.
 *
 * Not a bit-exact, shortest-round-trip implementation (no Grisu/Ryu), but
 * correct for normal magnitudes and the precision/width/flag combinations
 * apps actually use. Requires SSE (double math + reading the double vararg),
 * so stdio.c is built -msse (see the Makefile). NaN/Inf handled; very large
 * %f magnitudes fall back to %e to avoid 64-bit integer overflow. */
static int conv_float(sink_fn sink, void *ctx, int *total,
                      struct fmtflags *f, double val, char conv) {
    char num[600];
    int n = 0;
    int prec = f->has_precision ? f->precision : 6;
    if (prec < 0) prec = 6;
    if (prec > 60) prec = 60;          /* bound buffers; doubles can't show more */
    bool upper = (conv >= 'A' && conv <= 'Z');
    char lc = (char)(conv | 0x20);     /* 'f' | 'e' | 'g' */

    union { double d; unsigned long long u; } pun;
    pun.d = val;
    char sign = 0;
    if (pun.u >> 63) { sign = '-'; val = -val; }
    else if (f->plus)  sign = '+';
    else if (f->space) sign = ' ';

    unsigned long long ab = pun.u & 0x7fffffffffffffffULL;
    if ((ab >> 52) == 0x7ffULL) {                 /* Inf / NaN */
        bool nan = (ab & 0xfffffffffffffULL) != 0;
        const char *t = nan ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        while (*t) num[n++] = *t++;
        return emit_num_padded(sink, ctx, total, f, nan ? 0 : sign, num, n);
    }

    bool strip = false;                            /* %g strips trailing zeros */
    if (lc == 'g') {
        int P = (prec == 0) ? 1 : prec;
        int X = 0;
        double t = val;
        if (t != 0.0) { while (t >= 10.0) { t /= 10.0; X++; }
                        while (t <  1.0) { t *= 10.0; X--; } }
        if (X < -4 || X >= P) { lc = 'e'; prec = P - 1; }
        else                  { lc = 'f'; prec = P - 1 - X; if (prec < 0) prec = 0; }
        if (!f->hash) strip = true;
    }

    if (lc == 'f' && val >= 1.8e19) lc = 'e';      /* avoid u64 overflow */

    if (lc == 'e') {
        int X = 0;
        double m = val;
        if (m != 0.0) { while (m >= 10.0) { m /= 10.0; X++; }
                        while (m <  1.0) { m *= 10.0; X--; } }
        double scale = 1.0;
        for (int i = 0; i < prec; i++) scale *= 10.0;
        unsigned long long one = (unsigned long long)(scale + 0.5);
        unsigned long long md  = (unsigned long long)(m * scale + 0.5);
        if (md >= 10ULL * one) { md /= 10; X++; }  /* 9.99..->10.0 carry */
        char tb[80]; int tn = 0;
        unsigned long long q = md;
        for (int i = 0; i <= prec; i++) { tb[tn++] = (char)('0' + q % 10); q /= 10; }
        num[n++] = tb[tn - 1];
        if (prec > 0 || f->hash) num[n++] = '.';
        for (int i = tn - 2; i >= 0; i--) num[n++] = tb[i];
        if (strip) { while (n > 0 && num[n-1] == '0') n--;
                     if (n > 0 && num[n-1] == '.') n--; }
        num[n++] = upper ? 'E' : 'e';
        num[n++] = (X < 0) ? '-' : '+';
        int ax = X < 0 ? -X : X;
        char eb[8]; int en = 0;
        do { eb[en++] = (char)('0' + ax % 10); ax /= 10; } while (ax);
        while (en < 2) eb[en++] = '0';
        for (int i = en - 1; i >= 0; i--) num[n++] = eb[i];
    } else {                                       /* %f */
        double scale = 1.0;
        for (int i = 0; i < prec; i++) scale *= 10.0;
        unsigned long long ip  = (unsigned long long) val;
        double frac = val - (double) ip;
        unsigned long long one = (unsigned long long)(scale + 0.5);
        unsigned long long fd  = (unsigned long long)(frac * scale + 0.5);
        if (prec == 0) { if (frac >= 0.5) ip++; }
        else if (fd >= one) { fd -= one; ip++; }   /* fractional carry */
        char tb[24]; int tn = 0;
        unsigned long long q = ip;
        do { tb[tn++] = (char)('0' + q % 10); q /= 10; } while (q);
        for (int i = tn - 1; i >= 0; i--) num[n++] = tb[i];
        if (prec > 0 || f->hash) {
            num[n++] = '.';
            char fb[80]; int fn = 0;
            unsigned long long fq = fd;
            for (int i = 0; i < prec; i++) { fb[fn++] = (char)('0' + fq % 10); fq /= 10; }
            for (int i = fn - 1; i >= 0; i--) num[n++] = fb[i];
            if (strip) { while (n > 0 && num[n-1] == '0') n--;
                         if (n > 0 && num[n-1] == '.') n--; }
        }
    }
    return emit_num_padded(sink, ctx, total, f, sign, num, n);
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

        /* length modifier
         *
         * Only ONE character was consumed, so "%lld" took the first 'l' as
         * the modifier and then read the SECOND 'l' as the conversion --
         * which is not one, so printf emitted the format text verbatim.
         * Any ported program using %lld (and they are everywhere) printed
         * "%lld" where a number belonged, with no error anywhere.
         *
         * "ll" collapses to 'l' and "hh" to 'h': this is LP64, so long and
         * long long are both 64-bit and are passed identically in varargs,
         * and conv_int already works in 64-bit. On an ABI where they differ
         * this would need its own case. */
        /* j (intmax_t) and t (ptrdiff_t) are 64-bit too, so 'l' (batch
         * 25: GNU truncate's "%jd bytes" printed "%jd"). */
        if (*fmt == 'j' || *fmt == 't') {
            f.length = 'l';
            fmt++;
        } else if (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 'L') {
            f.length = *fmt;
            fmt++;
            if ((f.length == 'l' && *fmt == 'l') ||
                (f.length == 'h' && *fmt == 'h')) fmt++;
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
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double dv = va_arg(ap, double);
            if (conv_float(sink, ctx, &total, &f, dv, f.conv) != 0) return -1;
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

/* ============================================================
 *  FILE * layer -- BUFFERED (K5, 2026-09-30)
 * ============================================================
 *
 * Every FILE used to be unbuffered: fgetc() was one read(2) per byte and
 * fputc() one write(2) per byte. Measured: xargs reading `seq 100000`
 * (588,895 bytes) made 589,185 syscalls, and `seq` itself one write per
 * printf. Now each stream has a buffer (BUFSIZ):
 *
 *   input   one read(2) fills it; fgetc/fgets/fread/ungetc serve from it.
 *   output  stdout is LINE-buffered on a terminal and FULLY buffered
 *           otherwise, as C specifies; stderr is unbuffered; fopen'd
 *           streams are fully buffered. setvbuf/setbuf are real.
 *
 * WHAT IT DOES NOT CHANGE: the order in which this process's effects are
 * seen. The native userland was written against unbuffered stdio -- forty
 * programs mix printf with raw write(2) on the same descriptors, and many
 * print, then fork or spawn or sleep or read. So pending output is written
 * out BEFORE EVERY SYSCALL libtoby makes (the check in syscall.c's toby_sc*
 * trampolines is one load when nothing is pending): a raw write, a read
 * that may block, fork, exec, spawn, sleep, wait, exit and _exit all see
 * every byte printed before them already written, exactly as before. What
 * buffering removes is only the per-character syscalls BETWEEN them: runs
 * of stdio calls that make no other syscall are coalesced into one write.
 * An unbuffered stream (stderr) flushes everything pending first too, so
 * stdout-then-stderr text keeps its order in a merged 2>&1 capture.
 *
 * The one real difference: output still in a buffer when the process is
 * KILLED by a signal is lost, as on every other Unix.
 *
 * Threads: once pthread_create has run, each stream is locked around every
 * operation. The lock spins with yields and gives up after a bound rather
 * than deadlock -- a signal handler that prints to the stream its own
 * thread is inside would otherwise hang forever (stdio is not async-signal-
 * safe anywhere, but a garbled line beats a wedged program). */

#define STREAM_BUFSZ BUFSIZ

struct __libtoby_FILE {
    int  fd;
    int  pushback;       /* -1 = none: ungetc's one guaranteed byte */
    int  eof;
    int  err;
    int  is_static;      /* stdin/stdout/stderr: never freed */
    int  mode;           /* _IOFBF / _IOLBF / _IONBF, -1 = decide on first use */
    int  dir;            /* 0 idle, 1 buf holds read-ahead, 2 buf holds output */
    int  own_buf;        /* buf was malloc'd here */
    unsigned char *buf;
    size_t cap;
    size_t rpos, rlen;   /* reading: buf[rpos, rlen) not yet consumed */
    size_t wlen;         /* writing: buf[0, wlen) not yet written */
    int  lock;
    unsigned char one;   /* the read buffer of an unbuffered stream */
    struct __libtoby_FILE *next;   /* every open stream (flush-all) */
};

static unsigned char g_stdin_buf[STREAM_BUFSZ];
static unsigned char g_stdout_buf[STREAM_BUFSZ];

static struct __libtoby_FILE g_stderr = {
    .fd = 2, .pushback = -1, .is_static = 1, .mode = _IONBF,
};
static struct __libtoby_FILE g_stdout = {
    .fd = 1, .pushback = -1, .is_static = 1, .mode = -1,
    .buf = g_stdout_buf, .cap = STREAM_BUFSZ, .next = &g_stderr,
};
static struct __libtoby_FILE g_stdin = {
    .fd = 0, .pushback = -1, .is_static = 1, .mode = -1,
    .buf = g_stdin_buf, .cap = STREAM_BUFSZ, .next = &g_stdout,
};

FILE *stdin  = &g_stdin;
FILE *stdout = &g_stdout;
FILE *stderr = &g_stderr;

static struct __libtoby_FILE *g_streams = &g_stdin;
static int g_streams_lock;

/* Set when any stream may hold unwritten output; the syscall trampolines
 * test it (see syscall.c). Cleared by a complete flush. */
volatile int __toby_stdio_pending;
/* 1: every syscall writes out pending output first (K5), for the native
 * userland written against unbuffered stdio. 0: glibc's rules, for a
 * program that asks (__toby_stdio_like_glibc) -- output waits in its
 * buffer until it fills, a line ends on a terminal, fflush, or exit; a
 * read from a terminal-buffered input stream writes out the line-buffered
 * ones. GNU diff -l's own lines come after pr's pages that way, and the
 * eager rule put them first (finding 225). */
int __toby_stdio_eager = 1;

void __toby_stdio_like_glibc(void) { __toby_stdio_eager = 0; }
/* Set by pthread_create: from then on streams are locked. */
volatile int __toby_stdio_threaded;

static void spin_lock_bounded(int *l) {
    for (long i = 0; __atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE); i++) {
        if (i > 200000) return;             /* see the header: never deadlock */
        toby_sc_raw0(ABI_SYS_YIELD);
    }
}
static void spin_unlock(int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

static void flock_(FILE *f) {
    if (__toby_stdio_threaded) spin_lock_bounded(&f->lock);
}
static void funlock_(FILE *f) {
    if (__toby_stdio_threaded) spin_unlock(&f->lock);
}
static int ftrylock_(FILE *f) {
    if (!__toby_stdio_threaded) return 1;
    return !__atomic_exchange_n(&f->lock, 1, __ATOMIC_ACQUIRE);
}

/* write(2) until done, WITHOUT the pending-output hook (this IS the flush).
 * A failure leaves errno saying why, as fflush's must: it said nothing, so
 * `uniq f > /dev/full` reported "write error" where GNU's says "write
 * error: No space left on device" (batch 30, logs/utilgate/uniq.sh). */
static long raw_write_all(int fd, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    size_t left = n;
    while (left > 0) {
        long r = toby_sc_raw3(ABI_SYS_WRITE, fd, (long)(uintptr_t)p, (long)left);
        if (r < 0) { errno = (int)-r; return -1; }
        if (r == 0) { errno = EIO; return -1; }
        p += r;
        left -= (size_t)r;
    }
    return (long)n;
}

static int stream_flush_out(FILE *f) {
    if (f->dir != 2) return 0;
    size_t n = f->wlen;
    f->wlen = 0;
    f->dir = 0;
    if (n && raw_write_all(f->fd, f->buf, n) < 0) { f->err = 1; return EOF; }
    return 0;
}

/* Write out every stream's pending output. Streams another thread holds
 * are skipped: that thread is inside an operation and will finish it. */
void __toby_stdio_flush_all(void) {
    if (!__toby_stdio_pending) return;
    __toby_stdio_pending = 0;
    if (__toby_stdio_threaded) spin_lock_bounded(&g_streams_lock);
    for (FILE *f = g_streams; f; f = f->next) {
        if (f->dir != 2) continue;
        if (!ftrylock_(f)) { __toby_stdio_pending = 1; continue; }
        stream_flush_out(f);
        funlock_(f);
    }
    if (__toby_stdio_threaded) spin_unlock(&g_streams_lock);
}

/* glibc's flush before a read from a line-buffered or unbuffered input
 * stream: every line-buffered output stream's pending bytes */
static void flush_linebuffered(void) {
    if (__toby_stdio_threaded) spin_lock_bounded(&g_streams_lock);
    for (FILE *f = g_streams; f; f = f->next) {
        if (f->dir != 2 || f->mode != _IOLBF) continue;
        if (!ftrylock_(f)) continue;
        stream_flush_out(f);
        funlock_(f);
    }
    if (__toby_stdio_threaded) spin_unlock(&g_streams_lock);
}

/* The public raw helper (abort/assert paths): what is pending goes first. */
long __toby_raw_write(int fd, const void *buf, size_t n) {
    __toby_stdio_flush_all();
    return raw_write_all(fd, buf, n);
}

static void stream_decide_mode(FILE *f) {
    if (f->mode >= 0) return;
    char tio[64];
    f->mode = toby_sc_raw3(ABI_SYS_IOCTL, f->fd, 0x5401 /* TCGETS */,
                           (long)(uintptr_t)tio) >= 0 ? _IOLBF : _IOFBF;
}

static void stream_get_buf(FILE *f) {
    if (f->buf || f->mode == _IONBF) return;
    f->buf = (unsigned char *)malloc(STREAM_BUFSZ);
    if (!f->buf) { f->mode = _IONBF; return; }
    f->cap = STREAM_BUFSZ;
    f->own_buf = 1;
}

/* Leave read mode: give back to the descriptor what was read ahead but not
 * consumed, where the descriptor can seek (a file). A pipe or terminal
 * cannot take it back; as with every buffered stdio, it is gone. */
static void stream_drop_read(FILE *f) {
    if (f->dir != 1 && f->pushback < 0) return;
    long ahead = (f->dir == 1) ? (long)(f->rlen - f->rpos) : 0;
    if (f->pushback >= 0) ahead++;
    if (ahead > 0)
        (void)toby_sc_raw3(ABI_SYS_LSEEK, f->fd, -ahead, SEEK_CUR);
    f->rpos = f->rlen = 0;
    f->pushback = -1;
    if (f->dir == 1) f->dir = 0;
}

static int stream_write(FILE *f, const char *s, size_t n) {
    if (n == 0) return 0;
    if (f->dir == 1 || f->pushback >= 0) stream_drop_read(f);
    stream_decide_mode(f);
    stream_get_buf(f);
    if (f->mode == _IONBF || !f->buf) {
        if (__toby_stdio_eager)
            __toby_stdio_flush_all();       /* keep program order across fds */
        if (raw_write_all(f->fd, s, n) < 0) { f->err = 1; return EOF; }
        return 0;
    }
    if (f->wlen + n > f->cap) {
        if (stream_flush_out(f) < 0) return EOF;
        if (n >= f->cap) {
            if (raw_write_all(f->fd, s, n) < 0) { f->err = 1; return EOF; }
            return 0;
        }
    }
    memcpy(f->buf + f->wlen, s, n);
    f->wlen += n;
    f->dir = 2;
    __toby_stdio_pending = 1;
    if (f->mode == _IOLBF && memchr(s, '\n', n)) return stream_flush_out(f);
    return 0;
}

/* Refill: > 0 bytes available, 0 at end of file, -1 on error. */
static long stream_fill(FILE *f) {
    if (f->dir == 2 && stream_flush_out(f) < 0) return -1;
    stream_decide_mode(f);
    stream_get_buf(f);
    unsigned char *dst = f->buf;
    size_t want = f->cap;
    if (f->mode == _IONBF || !f->buf) { dst = &f->one; want = 1; }
    /* toby_sc3, not the raw form: output pending on ANY stream goes out
     * before this process can block for input (a prompt, a coprocess) --
     * under glibc's rules the line-buffered streams, for a read of a
     * line-buffered or unbuffered stream (a terminal) */
    if (!__toby_stdio_eager && (f->mode == _IOLBF || f->mode == _IONBF))
        flush_linebuffered();
    long r = toby_sc3(ABI_SYS_READ, f->fd, (long)(uintptr_t)dst, (long)want);
    if (r <= 0) {
        /* a failed read leaves errno saying why, as the write side does
         * (finding 185): `uniq dir` said "error reading 'dir'" and no
         * more once a directory opened as Linux's does (batch 31) */
        if (r < 0) { f->err = 1; errno = (int)-r; }
        else       f->eof = 1;
        return r < 0 ? -1 : 0;
    }
    if (dst == &f->one) {
        /* unbuffered: hand the byte back through pushback */
        f->pushback = f->one;
        return 1;
    }
    f->rpos = 0;
    f->rlen = (size_t)r;
    f->dir  = 1;
    return r;
}

static int stream_getc(FILE *f) {
    if (f->pushback >= 0) { int c = f->pushback; f->pushback = -1; return c; }
    if (f->dir == 1 && f->rpos < f->rlen) return f->buf[f->rpos++];
    long r = stream_fill(f);
    if (r <= 0) return EOF;
    if (f->pushback >= 0) { int c = f->pushback; f->pushback = -1; return c; }
    return f->buf[f->rpos++];
}

static FILE *stream_new(int fd, int mode) {
    FILE *f = (FILE *)malloc(sizeof(*f));
    if (!f) { errno = ENOMEM; return 0; }
    memset(f, 0, sizeof(*f));
    f->fd = fd;
    f->pushback = -1;
    f->mode = mode;
    if (__toby_stdio_threaded) spin_lock_bounded(&g_streams_lock);
    f->next = g_streams;
    g_streams = f;
    if (__toby_stdio_threaded) spin_unlock(&g_streams_lock);
    return f;
}

static void stream_unlink(FILE *f) {
    if (__toby_stdio_threaded) spin_lock_bounded(&g_streams_lock);
    for (FILE **pp = &g_streams; *pp; pp = &(*pp)->next)
        if (*pp == f) { *pp = f->next; break; }
    if (__toby_stdio_threaded) spin_unlock(&g_streams_lock);
}

/* ---- formatted output into a stream ---------------------------------- *
 * The engine emits small pieces; collect them and hand the stream whole
 * chunks, so fprintf(stderr, ...) -- unbuffered -- is one write for any
 * message that fits, not one per conversion. */
struct stream_sink_ctx {
    FILE  *f;
    size_t n;
    int    bad;
    char   chunk[512];
};

static int stream_sink(void *ctx, const char *s, size_t n) {
    struct stream_sink_ctx *c = (struct stream_sink_ctx *)ctx;
    while (n > 0) {
        size_t room = sizeof c->chunk - c->n;
        if (room == 0) {
            if (stream_write(c->f, c->chunk, c->n) < 0) c->bad = 1;
            c->n = 0;
            room = sizeof c->chunk;
        }
        size_t take = n < room ? n : room;
        memcpy(c->chunk + c->n, s, take);
        c->n += take;
        s += take;
        n -= take;
    }
    return 0;
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    if (!f) return -1;
    struct stream_sink_ctx c;
    c.f = f; c.n = 0; c.bad = 0;
    flock_(f);
    int n = do_format(stream_sink, &c, fmt, ap);
    if (c.n && stream_write(f, c.chunk, c.n) < 0) c.bad = 1;
    funlock_(f);
    if (n < 0 || c.bad) { f->err = 1; return -1; }
    return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int fputs(const char *s, FILE *f) {
    if (!f) return EOF;
    size_t n = strlen(s);
    flock_(f);
    int r = stream_write(f, s, n);
    funlock_(f);
    return r < 0 ? EOF : (int)n;
}

int puts(const char *s) {
    size_t n = strlen(s);
    flock_(stdout);
    int r = stream_write(stdout, s, n);
    if (r >= 0) r = stream_write(stdout, "\n", 1);
    funlock_(stdout);
    return r < 0 ? EOF : (int)n + 1;
}

int fputc(int c, FILE *f) {
    if (!f) return EOF;
    char ch = (char)c;
    flock_(f);
    int r = stream_write(f, &ch, 1);
    funlock_(f);
    return r < 0 ? EOF : (unsigned char)ch;
}

int putc   (int c, FILE *f) { return fputc(c, f); }
int putchar(int c)          { return fputc(c, stdout); }

int fgetc(FILE *f) {
    if (!f) return EOF;
    flock_(f);
    int c = stream_getc(f);
    funlock_(f);
    return c;
}

int getc(FILE *f)   { return fgetc(f); }
int getchar(void)   { return fgetc(stdin); }

/* POSIX getdelim (batch 25: GNU cksum -c and others read lines of any
 * length). */
ssize_t getdelim(char **linep, size_t *capp, int delim, FILE *f) {
    if (!linep || !capp || !f) { errno = EINVAL; return -1; }
    size_t n = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (!*linep || n + 2 > *capp) {
            size_t cap = *capp ? *capp * 2 : 128;
            while (cap < n + 2) cap *= 2;
            char *nb = (char *)realloc(*linep, cap);
            if (!nb) { errno = ENOMEM; return -1; }
            *linep = nb;
            *capp = cap;
        }
        (*linep)[n++] = (char)c;
        if (c == delim) break;
    }
    if (n == 0) return -1;
    (*linep)[n] = 0;
    return (ssize_t)n;
}

ssize_t getline(char **linep, size_t *capp, FILE *f) {
    return getdelim(linep, capp, '\n', f);
}

char *fgets(char *s, int cap, FILE *f) {
    if (!s || cap <= 0 || !f) return 0;
    flock_(f);
    int i = 0;
    while (i < cap - 1) {
        if (f->pushback >= 0 || !(f->dir == 1 && f->rpos < f->rlen)) {
            int c = stream_getc(f);             /* pushback, or a refill */
            if (c == EOF) break;
            s[i++] = (char)c;
            if (c == '\n') break;
            continue;
        }
        size_t avail = f->rlen - f->rpos, room = (size_t)(cap - 1 - i);
        size_t take = avail < room ? avail : room;
        unsigned char *src = f->buf + f->rpos;
        unsigned char *nl = (unsigned char *)memchr(src, '\n', take);
        if (nl) take = (size_t)(nl - src) + 1;
        memcpy(s + i, src, take);
        f->rpos += take;
        i += (int)take;
        if (nl) break;
    }
    funlock_(f);
    if (i == 0) return 0;
    s[i] = '\0';
    return s;
}

size_t fread(void *buf, size_t sz, size_t n, FILE *f) {
    if (!f || sz == 0 || n == 0) return 0;
    size_t total = sz * n;
    char *out = (char *)buf;
    size_t got = 0;
    flock_(f);
    if (f->dir == 2 && stream_flush_out(f) < 0) { funlock_(f); return 0; }
    if (got < total && f->pushback >= 0) {
        out[got++] = (char)f->pushback;
        f->pushback = -1;
    }
    if (f->dir == 1 && f->rpos < f->rlen) {
        size_t avail = f->rlen - f->rpos, take = total - got;
        if (take > avail) take = avail;
        memcpy(out + got, f->buf + f->rpos, take);
        f->rpos += take;
        got += take;
    }
    while (got < total) {
        size_t left = total - got;
        stream_decide_mode(f);
        stream_get_buf(f);
        if (left >= f->cap || !f->buf || f->mode == _IONBF) {
            /* big (or unbuffered) read: straight into the caller's memory */
            long r = toby_sc3(ABI_SYS_READ, f->fd,
                              (long)(uintptr_t)(out + got), (long)left);
            if (r < 0) { f->err = 1; errno = (int)-r; break; }
            if (r == 0) { f->eof = 1; break; }
            got += (size_t)r;
            continue;
        }
        long r = stream_fill(f);
        if (r <= 0) break;
        size_t avail = f->rlen - f->rpos, take = left < avail ? left : avail;
        memcpy(out + got, f->buf + f->rpos, take);
        f->rpos += take;
        got += take;
    }
    funlock_(f);
    return got / sz;
}

size_t fwrite(const void *buf, size_t sz, size_t n, FILE *f) {
    if (!f || sz == 0 || n == 0) return 0;
    flock_(f);
    int r = stream_write(f, (const char *)buf, sz * n);
    funlock_(f);
    return r < 0 ? 0 : n;
}

int fflush(FILE *f) {
    if (!f) {                                   /* every output stream */
        __toby_stdio_pending = 1;
        __toby_stdio_flush_all();
        return 0;
    }
    flock_(f);
    int r = 0;
    if (f->dir == 2) r = stream_flush_out(f);
    else             stream_drop_read(f);      /* POSIX: re-sync the fd */
    funlock_(f);
    return r;
}

int fseek(FILE *f, long off, int whence) {
    if (!f) return -1;
    flock_(f);
    if (f->dir == 2 && stream_flush_out(f) < 0) { funlock_(f); return -1; }
    if (whence == SEEK_CUR) {
        /* relative to the STREAM's position, which trails the fd's by the
         * read-ahead still in the buffer */
        if (f->dir == 1) off -= (long)(f->rlen - f->rpos);
        if (f->pushback >= 0) off -= 1;
    }
    f->rpos = f->rlen = 0;
    f->pushback = -1;
    if (f->dir == 1) f->dir = 0;
    long r = toby_sc3(ABI_SYS_LSEEK, f->fd, off, whence);
    funlock_(f);
    if (r < 0) { f->err = 1; errno = (int)(-r); return -1; }
    f->eof = 0;
    return 0;
}

long ftell(FILE *f) {
    if (!f) return -1;
    flock_(f);
    long r = toby_sc3(ABI_SYS_LSEEK, f->fd, 0, SEEK_CUR);
    if (r >= 0) {
        if (f->dir == 1) r -= (long)(f->rlen - f->rpos);
        if (f->pushback >= 0) r -= 1;
        if (f->dir == 2) r += (long)f->wlen;
    }
    funlock_(f);
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
    int fd = open(path, flags, 0666);     /* C's fopen: 0666 less the umask */
    if (fd < 0) return 0;
    FILE *f = stream_new(fd, -1);
    if (!f) { close(fd); return 0; }
    return f;
}

int fclose(FILE *f) {
    if (!f) return EOF;
    flock_(f);
    int r = 0;
    if (f->dir == 2) r = stream_flush_out(f);
    else             stream_drop_read(f);   /* a seekable input's fd lands
                                             * at the stream's position */
    funlock_(f);
    int rc = close(f->fd);
    if (!f->is_static) {
        stream_unlink(f);
        if (f->own_buf) free(f->buf);
        free(f);
    }
    return (r < 0 || rc < 0) ? EOF : 0;
}

void perror(const char *prefix) {
    const char *m = strerror(errno);          /* before any call can move it */
    if (prefix && *prefix) fprintf(stderr, "%s: %s\n", prefix, m);
    else                   fprintf(stderr, "%s\n", m);
}

/* ============================================================
 *  ungetc
 * ============================================================ */

int ungetc(int c, FILE *f) {
    if (!f || c == EOF) return EOF;
    flock_(f);
    if (f->dir == 2) stream_flush_out(f);
    f->pushback = (unsigned char)c;
    f->eof = 0;
    funlock_(f);
    return (unsigned char)c;
}

/* ============================================================
 *  setvbuf / setbuf -- buffering control
 * ============================================================ */

int setvbuf(FILE *f, char *buf, int mode, size_t size) {
    if (!f || (mode != _IOFBF && mode != _IOLBF && mode != _IONBF)) return -1;
    flock_(f);
    if (f->dir == 2) stream_flush_out(f);
    if (f->own_buf) free(f->buf);
    f->own_buf = 0;
    f->buf = 0;
    f->cap = 0;
    f->rpos = f->rlen = 0;
    if (f->dir == 1) f->dir = 0;
    f->mode = mode;
    if (mode != _IONBF) {
        if (buf && size > 0) {
            f->buf = (unsigned char *)buf;
            f->cap = size;
        } else if (size > 0) {
            f->buf = (unsigned char *)malloc(size);
            if (f->buf) { f->cap = size; f->own_buf = 1; }
        }                          /* else: BUFSIZ, allocated on first use */
    }
    funlock_(f);
    return 0;
}

void setbuf(FILE *f, char *buf) {
    (void)setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

/* ============================================================
 *  tmpfile -- open anonymous temporary file
 * ============================================================ */

FILE *tmpfile(void) {
    static int counter = 0;
    char name[64];
    snprintf(name, sizeof(name), "/tmp/_tmpfile_%d_%d", (int)toby_sc0(ABI_SYS_GETPID), counter++);
    FILE *f = fopen(name, "w+");
    if (f) unlink(name);
    return f;
}

/* ============================================================
 *  popen / pclose -- pipe to/from a command
 * ============================================================ */

struct popen_entry {
    FILE *fp;
    pid_t pid;
};

#define POPEN_MAX 8
static struct popen_entry g_popens[POPEN_MAX];

FILE *popen(const char *command, const char *type) {
    if (!command || !type) { errno = EINVAL; return 0; }
    int reading = (type[0] == 'r');

    int fds[2];
    if (pipe(fds) < 0) return 0;

    char *argv[4];
    argv[0] = "/bin/sh";
    argv[1] = "-c";
    argv[2] = (char *)command;
    argv[3] = 0;

    pid_t pid;
    if (reading) {
        pid = toby_spawn("/bin/sh", argv, environ, 0, fds[1], 2);
    } else {
        pid = toby_spawn("/bin/sh", argv, environ, fds[0], 1, 2);
    }

    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return 0;
    }

    int our_fd, their_fd;
    if (reading) { our_fd = fds[0]; their_fd = fds[1]; }
    else         { our_fd = fds[1]; their_fd = fds[0]; }
    close(their_fd);

    FILE *fp = stream_new(our_fd, -1);
    if (!fp) { close(our_fd); return 0; }

    for (int i = 0; i < POPEN_MAX; i++) {
        if (!g_popens[i].fp) {
            g_popens[i].fp = fp;
            g_popens[i].pid = pid;
            break;
        }
    }
    return fp;
}

int pclose(FILE *fp) {
    if (!fp) return -1;
    pid_t pid = -1;
    for (int i = 0; i < POPEN_MAX; i++) {
        if (g_popens[i].fp == fp) {
            pid = g_popens[i].pid;
            g_popens[i].fp = 0;
            break;
        }
    }
    fclose(fp);
    if (pid < 0) { errno = ECHILD; return -1; }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return status;
}

/* ============================================================
 *  sscanf -- minimal scanf for common patterns
 * ============================================================ */

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int count = 0;
    const char *s = str;

    while (*fmt && *s) {
        if (*fmt == ' ') {
            while (*s == ' ' || *s == '\t' || *s == '\n') s++;
            fmt++;
            continue;
        }
        if (*fmt != '%') {
            if (*s != *fmt) break;
            s++; fmt++;
            continue;
        }
        fmt++; /* skip % */
        if (*fmt == '%') { if (*s != '%') break; s++; fmt++; continue; }

        /* %*: match and discard (2026-09-04; sudo's passwd parser needed it) */
        int suppress = 0;
        if (*fmt == '*') { suppress = 1; fmt++; }

        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }

        int lng = 0;
        if (*fmt == 'l') { lng = 1; fmt++; }

        switch (*fmt) {
        case 'd': case 'i': {
            char *end;
            long v = strtol(s, &end, 10);
            if (end == s) goto done;
            if (!suppress) {
                if (lng) { long *p = va_arg(ap, long *); *p = v; }
                else     { int  *p = va_arg(ap, int *);  *p = (int)v; }
                count++;
            }
            s = end;
            break;
        }
        case 'u': {
            char *end;
            unsigned long v = strtoul(s, &end, 10);
            if (end == s) goto done;
            if (!suppress) {
                if (lng) { unsigned long *p = va_arg(ap, unsigned long *); *p = v; }
                else     { unsigned int  *p = va_arg(ap, unsigned int *);  *p = (unsigned int)v; }
                count++;
            }
            s = end;
            break;
        }
        case 'x': case 'X': {
            char *end;
            unsigned long v = strtoul(s, &end, 16);
            if (end == s) goto done;
            if (!suppress) {
                if (lng) { unsigned long *p = va_arg(ap, unsigned long *); *p = v; }
                else     { unsigned int  *p = va_arg(ap, unsigned int *);  *p = (unsigned int)v; }
                count++;
            }
            s = end;
            break;
        }
        case '[': {
            /* scanset: %[abc], %[^abc], %[]abc], %[a-z]; width caps it */
            fmt++;
            int negate = 0;
            if (*fmt == '^') { negate = 1; fmt++; }
            unsigned char set[32] = { 0 };
            if (*fmt == ']') { set[']' >> 3] |= (unsigned char)(1u << (']' & 7)); fmt++; }
            while (*fmt && *fmt != ']') {
                unsigned char lo = (unsigned char)*fmt;
                if (fmt[1] == '-' && fmt[2] && fmt[2] != ']') {
                    unsigned char hi = (unsigned char)fmt[2];
                    for (unsigned c = lo; c <= hi; c++) set[c >> 3] |= (unsigned char)(1u << (c & 7));
                    fmt += 3;
                } else { set[lo >> 3] |= (unsigned char)(1u << (lo & 7)); fmt++; }
            }
            if (*fmt != ']') goto done;
            char *p = suppress ? NULL : va_arg(ap, char *);
            int w = 0;
            while (*s) {
                unsigned char c = (unsigned char)*s;
                int in = (set[c >> 3] >> (c & 7)) & 1;
                if (in == negate) break;
                if (width > 0 && w >= width) break;
                if (p) *p++ = *s;
                s++; w++;
            }
            if (w == 0) goto done;
            if (p) { *p = '\0'; count++; }
            break;
        }
        case 's': {
            char *p = suppress ? NULL : va_arg(ap, char *);
            while (*s == ' ' || *s == '\t') s++;
            int w = 0;
            while (*s && *s != ' ' && *s != '\t' && *s != '\n') {
                if (width > 0 && w >= width) break;
                if (p) *p++ = *s;
                s++;
                w++;
            }
            if (p) { *p = '\0'; count++; }
            break;
        }
        case 'c': {
            char *p = suppress ? NULL : va_arg(ap, char *);
            if (p) { *p = *s; count++; }
            s++;
            break;
        }
        case 'n': {
            int *p = va_arg(ap, int *);
            *p = (int)(s - str);
            break;
        }
        default:
            goto done;
        }
        fmt++;
    }
done:
    va_end(ap);
    return count;
}
