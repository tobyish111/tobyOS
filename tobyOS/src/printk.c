/* printk.c -- minimal kernel printf.
 *
 * Output is fanned to serial (COM1 + debugcon) and, once available, the
 * framebuffer console. We deliberately format directly into the sinks
 * one chunk at a time -- no intermediate heap buffer, since the heap
 * doesn't exist yet.
 *
 * Supported: %s %c %d %u %x %p %lu %ld %lx, with optional '-' (left
 * align), width, and '0' padding (e.g. %-8s, %08x). Anything
 * unrecognised is printed verbatim so we can spot format-string bugs.
 */

#include <tobyos/printk.h>
#include <tobyos/serial.h>
#include <tobyos/console.h>
#include <tobyos/bootlog.h>
#include <tobyos/klibc.h>
#include <tobyos/spinlock.h>
#include <tobyos/pit.h>

/* Optional "mirror" sink: when set, every emitted byte is also handed
 * off to cb(ctx, c). See printk.h for the semantics + caveats. */
static void (*g_sink_cb)(void *ctx, char c) = 0;
static void *g_sink_ctx = 0;

/* Milestone 22 step 5: serialise the entire kvprintf body so two
 * CPUs printing concurrently never interleave individual characters
 * (which made the smp_logf wrapper necessary in the first place).
 * IRQ-save guards against an IRQ on the same CPU re-entering
 * kvprintf -- if it happens, the CPU spins on its own lock and we
 * deadlock, so the irq-save is what keeps that path correct.
 *
 * Note: this lock does NOT chase concurrent serial_putc / console_putc
 * sinks themselves, but those are MMIO-only and idempotent at the
 * byte level (each serial_putc spins on the THR-empty bit, console
 * tracks its own cursor). The interleaving we worried about was
 * "CPU A prints 'AB' while CPU B prints 'CD'" landing as 'ACBD' in
 * the log -- locking the whole format/emit pass eliminates that. */
static spinlock_t g_printk_lock = SPINLOCK_INIT;

void printk_set_sink(void (*cb)(void *ctx, char c), void *ctx) {
    g_sink_cb  = cb;
    g_sink_ctx = ctx;
}

static void emit_char(char c) {
    serial_putc(c);
    if (console_ready()) console_putc(c);
    if (g_sink_cb) g_sink_cb(g_sink_ctx, c);
    bootlog_char(c);
}

static void emit_str(const char *s) {
    while (*s) emit_char(*s++);
}

static void emit_pad(char pad, int n) {
    while (n-- > 0) emit_char(pad);
}

/* Format an unsigned 64-bit value into `buf` (little-endian digits) and
 * return the number of digits written. base must be 10 or 16. */
static int u64_to_buf(uint64_t v, unsigned base, bool upper, char *buf) {
    static const char digits_lo[] = "0123456789abcdef";
    static const char digits_up[] = "0123456789ABCDEF";
    const char *digits = upper ? digits_up : digits_lo;

    if (v == 0) { buf[0] = '0'; return 1; }
    int n = 0;
    while (v) {
        buf[n++] = digits[v % base];
        v /= base;
    }
    return n;
}

static void emit_uint(uint64_t v, unsigned base, bool upper,
                      int width, bool zero_pad, bool left_align) {
    char buf[32];
    int n = u64_to_buf(v, base, upper, buf);
    int pad = width - n;
    if (pad > 0 && !left_align) emit_pad(zero_pad ? '0' : ' ', pad);
    while (n-- > 0) emit_char(buf[n]);
    if (pad > 0 && left_align) emit_pad(' ', pad);  /* left-align: never zero-pad */
}

/* Monotonic ms since PIT started (IRQ0 ticks); before pit_init(), hz is 0. */
static void emit_ts_prefix(void) {
    emit_char('[');
    uint32_t hz = pit_hz();
    if (hz == 0) {
        emit_str("------");
        emit_str("] ");
        return;
    }
    uint64_t ms = (pit_ticks() * 1000ull) / (uint64_t)hz;
    emit_uint(ms, 10, false, 0, false, false);
    emit_str(" ms] ");
}

static void emit_int(int64_t v, int width, bool zero_pad, bool left_align) {
    if (v < 0) {
        /* Print sign first, then magnitude. -INT64_MIN overflows so cast
         * via unsigned to get the correct magnitude (UB-free). */
        emit_char('-');
        uint64_t mag = (uint64_t)(-(v + 1)) + 1u;
        if (width > 0) width--;
        emit_uint(mag, 10, false, width, zero_pad, left_align);
    } else {
        emit_uint((uint64_t)v, 10, false, width, zero_pad, left_align);
    }
}

static void kvprintf_unlocked(const char *fmt, va_list ap) {
    while (*fmt) {
        char c = *fmt++;
        if (c != '%') { emit_char(c); continue; }

        bool zero_pad   = false;
        bool left_align = false;
        int  width      = 0;
        bool is_long    = false;

        if (*fmt == '-') { left_align = true; fmt++; }
        if (*fmt == '0' && !left_align) { zero_pad = true; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == 'l') {
            is_long = true; fmt++;
            /* Accept (and silently coalesce) `ll` -- on x86_64 long
             * and long long are both 64-bit, so we can treat %llu
             * and %lu identically. Without this the second 'l' fell
             * through to the unknown-spec branch and the format
             * string was printed literally. */
            if (*fmt == 'l') fmt++;
        }

        char spec = *fmt++;
        switch (spec) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)strlen(s);
            int pad = width - len;
            if (pad > 0 && !left_align) emit_pad(' ', pad);
            emit_str(s);
            if (pad > 0 && left_align) emit_pad(' ', pad);
            break;
        }
        case 'c':
            emit_char((char)va_arg(ap, int));
            break;
        case 'd': {
            int64_t v = is_long ? va_arg(ap, long) : va_arg(ap, int);
            emit_int(v, width, zero_pad, left_align);
            break;
        }
        case 'u': {
            uint64_t v = is_long ? va_arg(ap, unsigned long)
                                 : va_arg(ap, unsigned int);
            emit_uint(v, 10, false, width, zero_pad, left_align);
            break;
        }
        case 'x': {
            uint64_t v = is_long ? va_arg(ap, unsigned long)
                                 : va_arg(ap, unsigned int);
            emit_uint(v, 16, false, width, zero_pad, left_align);
            break;
        }
        case 'X': {
            uint64_t v = is_long ? va_arg(ap, unsigned long)
                                 : va_arg(ap, unsigned int);
            emit_uint(v, 16, true, width, zero_pad, left_align);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            emit_str("0x");
            emit_uint((uint64_t)v, 16, false, 16, true, false);
            break;
        }
        case '%':
            emit_char('%');
            break;
        case '\0':
            return;
        default:
            /* Unknown spec: surface it so we notice. */
            emit_char('%');
            emit_char(spec);
            break;
        }
    }
}

void kvprintf(const char *fmt, va_list ap) {
    uint64_t flags = spin_lock_irqsave(&g_printk_lock);
    emit_ts_prefix();
    kvprintf_unlocked(fmt, ap);
    spin_unlock_irqrestore(&g_printk_lock, flags);
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

void kputs(const char *s) {
    uint64_t flags = spin_lock_irqsave(&g_printk_lock);
    emit_ts_prefix();
    emit_str(s);
    emit_char('\n');
    spin_unlock_irqrestore(&g_printk_lock, flags);
}

void kputc(char c) {
    uint64_t flags = spin_lock_irqsave(&g_printk_lock);
    emit_char(c);
    spin_unlock_irqrestore(&g_printk_lock, flags);
}
