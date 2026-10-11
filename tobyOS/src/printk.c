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
#include <tobyos/perf.h>
#include <tobyos/proc.h>
#include <tobyos/smp.h>

/* Optional "mirror" sink: when set, every emitted byte is also handed
 * off to cb(ctx, c). See printk.h for the semantics + caveats. */
static void (*g_sink_cb)(void *ctx, char c) = 0;
static void *g_sink_ctx = 0;
static bool g_sink_suppress = false;

/* THE SINK IS ITS OWNER'S, AND IT IS FED OUTSIDE THE LOCK.
 *
 * The in-kernel shell points the sink at a builtin's stdout -- a pipe, a
 * capture's tmpfs spill file, the TCP client -- and the callback did that
 * I/O HERE, inside g_printk_lock with interrupts off. On the EliteDesk
 * (30cb3c8) `x=$(ls /dev)` in the kernel shell took the whole machine down:
 * no heartbeat, no ping, no dump. QEMU ran it 10/10. A write that allocates
 * or wakes a process can need another CPU, and every other CPU that called
 * kprintf at that moment was spinning on this lock with ITS interrupts off.
 * (Bisected on the box: `ls /dev` to the terminal survived, `x=$(ls /dev)`
 * did not -- logs/kpipebisect.py.)
 *
 * And the sink was GLOBAL: every CPU's kprintf -- the `[proc] created`
 * line of a process being spawned on another core, an IRQ handler's log --
 * went into the builtin's output, and with suppress on, off serial.
 *
 * So:
 *   - only the context that installed the sink feeds it (the same process;
 *     for pid 0, which every CPU's idle loop shares, the same CPU too).
 *     Everyone else's kprintf goes to serial and the console as if no sink
 *     were set -- never captured, never suppressed;
 *   - its bytes are STAGED here under the lock and handed to the callback
 *     after the lock is released (sink_deliver), so no sink ever runs its
 *     I/O inside g_printk_lock;
 *   - a kprintf made while delivering (the I/O path logging something)
 *     bypasses the sink rather than recursing into it. */
static struct proc *g_sink_owner;
static uint32_t     g_sink_owner_cpu;
#define SINK_STAGE_SZ 4096
static char   g_sink_stage[SINK_STAGE_SZ];
static size_t g_sink_stage_len;
static size_t g_sink_dropped;
static bool   g_sink_delivering;
/* Decided once per call, under the lock: does THIS call feed the sink? */
static bool   g_emit_to_sink;

static bool sink_is_mine(void) {
    if (!g_sink_cb || g_sink_delivering) return false;
    struct proc *p = current_proc();
    if (p != g_sink_owner) return false;
    if (p && p->pid != 0) return true;
    return smp_current_cpu_idx() == g_sink_owner_cpu;
}

static void sink_stage(char c) {
    if (g_sink_stage_len < SINK_STAGE_SZ) g_sink_stage[g_sink_stage_len++] = c;
    else g_sink_dropped++;
}

/* Called by the owner AFTER releasing g_printk_lock. Only the owner stages,
 * and a nested kprintf during delivery bypasses the sink, so the stage is
 * this context's alone. */
static void sink_deliver(void) {
    void (*cb)(void *ctx, char c) = g_sink_cb;
    void *ctx = g_sink_ctx;
    g_sink_delivering = true;
    if (cb)
        for (size_t i = 0; i < g_sink_stage_len; i++) cb(ctx, g_sink_stage[i]);
    g_sink_stage_len = 0;
    size_t dropped = g_sink_dropped;
    g_sink_dropped = 0;
    g_sink_delivering = false;
    if (dropped)
        kprintf_serial("[printk] sink: %lu byte(s) of one message dropped "
                       "(stage is %u)\n", (unsigned long)dropped,
                       (unsigned)SINK_STAGE_SZ);
}

/* When set, emit_char routes ONLY to the COM1 serial sink -- not the
 * framebuffer console, the mirror sink, or the boot log. Used by
 * kprintf_serial() so a periodic heartbeat (or any serial-only trace)
 * never clutters the on-screen console or a full-screen TUI. Guarded by
 * g_printk_lock, so it composes with normal kprintf without interleaving. */
static bool g_emit_serial_only = false;

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
    printk_set_sink_mode(cb, ctx, false);
}

void printk_set_sink_mode(void (*cb)(void *ctx, char c), void *ctx,
                          bool suppress_normal_output) {
    g_sink_cb       = cb;
    g_sink_ctx      = ctx;
    g_sink_suppress = cb ? suppress_normal_output : false;
    g_sink_owner     = cb ? current_proc() : 0;
    g_sink_owner_cpu = cb ? smp_current_cpu_idx() : 0;
}

void printk_get_sink(void (**cb)(void *ctx, char c), void **ctx,
                     bool *suppress_normal_output) {
    if (cb) *cb = g_sink_cb;
    if (ctx) *ctx = g_sink_ctx;
    if (suppress_normal_output) *suppress_normal_output = g_sink_suppress;
}

static void emit_char(char c) {
    if (g_emit_serial_only) {
        serial_putc(c);
        return;
    }
    /* Suppression is the OWNER's request, for the owner's output only. */
    bool suppress = g_emit_to_sink && g_sink_suppress;
    if (!suppress) {
        serial_putc(c);
        if (console_ready()) console_putc(c);
    }
    if (g_emit_to_sink) sink_stage(c);
    if (!suppress) bootlog_char(c);
}

static void emit_str(const char *s) {
    while (*s) emit_char(*s++);
}

static void emit_pad(char pad, int n) {
    while (n-- > 0) emit_char(pad);
}

/* Format an unsigned 64-bit value into `buf` (little-endian digits) and
 * return the number of digits written. Any base up to 16 works (8, 10, 16
 * are the ones used). */
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

/* Monotonic ms-since-boot for the log prefix.
 *
 * Early boot uses the PIT tick counter. But the PIT IRQ stops firing once
 * the kernel hands the timer tick to the LAPIC, which froze the old
 * pit-only prefix a few seconds into boot. Once the TSC is calibrated
 * (perf_now_ns != 0) we switch to it -- it never stalls. To keep the
 * timeline continuous across the switch (perf_now_ns counts from
 * calibration, not reset) we capture a one-time offset = the pit-based ms
 * at the moment of the switch, so timestamps neither jump backward nor
 * reset. Called under g_printk_lock, so the lazy offset init is safe. */
static uint64_t g_ts_offset_ms = 0;
static bool     g_ts_offset_set = false;

/* Returns ms-since-boot, or UINT64_MAX if no clock is available yet. */
uint64_t klog_ms(void);   /* public: same clock as the [N ms] log prefix */
static uint64_t now_log_ms(void) {
    uint64_t ns = perf_now_ns();
    if (ns) {
        uint64_t pms = ns / 1000000ull;
        if (!g_ts_offset_set) {
            uint32_t hz = pit_hz();
            uint64_t pit_ms = hz ? (pit_ticks() * 1000ull) / (uint64_t)hz : 0;
            g_ts_offset_ms = (pit_ms > pms) ? (pit_ms - pms) : 0;
            g_ts_offset_set = true;
        }
        return g_ts_offset_ms + pms;
    }
    uint32_t hz = pit_hz();
    if (hz == 0) return (uint64_t)-1;
    return (pit_ticks() * 1000ull) / (uint64_t)hz;
}

uint64_t klog_ms(void) { return now_log_ms(); }

static void emit_ts_prefix(void) {
    if (g_emit_to_sink && g_sink_suppress) return;
    emit_char('[');
    uint64_t ms = now_log_ms();
    if (ms == (uint64_t)-1) {
        emit_str("------");
        emit_str("] ");
        return;
    }
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
        case 'o': {
            /* File modes are octal everywhere in this kernel and there was
             * no way to print one: %o silently emitted the literal text,
             * so a mode assertion printed "mode=%o" and read as noise. */
            uint64_t v = is_long ? va_arg(ap, unsigned long)
                                 : va_arg(ap, unsigned int);
            emit_uint(v, 8, false, width, zero_pad, left_align);
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
    g_emit_to_sink = sink_is_mine();
    bool deliver = g_emit_to_sink;
    emit_ts_prefix();
    kvprintf_unlocked(fmt, ap);
    g_emit_to_sink = false;
    spin_unlock_irqrestore(&g_printk_lock, flags);
    if (deliver) sink_deliver();
}

void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

/* kprintf's line without the sink: see printk.h. */
void kprintf_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    uint64_t flags = spin_lock_irqsave(&g_printk_lock);
    g_emit_to_sink = false;
    emit_ts_prefix();
    kvprintf_unlocked(fmt, ap);
    spin_unlock_irqrestore(&g_printk_lock, flags);
    va_end(ap);
}

/* Like kprintf, but the formatted line goes ONLY to COM1 serial -- never
 * the framebuffer console, mirror sink, or boot log. Still timestamped and
 * serialised under g_printk_lock so it can't interleave with kprintf. */
void kprintf_serial(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    uint64_t flags = spin_lock_irqsave(&g_printk_lock);
    bool prev = g_emit_serial_only;
    g_emit_serial_only = true;
    emit_ts_prefix();
    kvprintf_unlocked(fmt, ap);
    g_emit_serial_only = prev;
    spin_unlock_irqrestore(&g_printk_lock, flags);
    va_end(ap);
}

void kputs(const char *s) {
    uint64_t flags = spin_lock_irqsave(&g_printk_lock);
    g_emit_to_sink = sink_is_mine();
    bool deliver = g_emit_to_sink;
    emit_ts_prefix();
    emit_str(s);
    emit_char('\n');
    g_emit_to_sink = false;
    spin_unlock_irqrestore(&g_printk_lock, flags);
    if (deliver) sink_deliver();
}

void kputc(char c) {
    uint64_t flags = spin_lock_irqsave(&g_printk_lock);
    g_emit_to_sink = sink_is_mine();
    bool deliver = g_emit_to_sink;
    emit_char(c);
    g_emit_to_sink = false;
    spin_unlock_irqrestore(&g_printk_lock, flags);
    if (deliver) sink_deliver();
}
