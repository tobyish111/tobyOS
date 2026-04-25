/* slog.c -- Milestone 28A: structured kernel + userland log framework.
 *
 * Implementation contract (mirror of slog.h):
 *
 *   - Records live in a fixed-depth ring (ABI_SLOG_RING_DEPTH). When
 *     the writer wraps it overwrites the oldest slot and bumps a
 *     `g_dropped` counter. The next drain reports the dropped count
 *     on its first returned record so userland (logview) can warn.
 *
 *   - All writes funnel through slog_record_locked(); IRQs are masked
 *     for the duration of the formatted body copy so an IRQ that also
 *     wants to log can't deadlock the spinlock. Formatting is done
 *     into a stack scratch buffer first, then copied into the slot,
 *     so we never call the formatter with the lock held.
 *
 *   - Console fan-out re-uses kprintf() so the existing serial +
 *     framebuffer plumbing is unchanged. Threshold defaults to INFO.
 *
 *   - slog_persist_flush() rewrites SLOG_PERSIST_PATH with the entire
 *     ring contents in line-oriented text. The file gets truncated
 *     to SLOG_PERSIST_CAP_BYTES if it would exceed that, by dropping
 *     the oldest lines. Callers must be in a sleepable context (the
 *     boot path is fine; the panic path uses slog_dump_kprintf()
 *     instead, which only touches kprintf, not VFS).
 */

#include <tobyos/slog.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/spinlock.h>
#include <tobyos/pit.h>
#include <tobyos/proc.h>
#include <tobyos/vfs.h>

/* ---- ring buffer ------------------------------------------------- */

static struct abi_slog_record g_ring[ABI_SLOG_RING_DEPTH];
static spinlock_t              g_ring_lock = SPINLOCK_INIT;
static uint64_t                g_seq         = 0;     /* next-to-assign */
static uint64_t                g_dropped     = 0;     /* lifetime drops */
static uint64_t                g_dropped_peek = 0;    /* drops since last drain */
static uint64_t                g_persist_seq = 0;     /* highest seq flushed */
static bool                    g_ready       = false;

/* ---- counters ---------------------------------------------------- */

static uint64_t g_total_emitted        = 0;
static uint64_t g_per_level[ABI_SLOG_LEVEL_MAX] = { 0 };
static uint64_t g_persist_bytes        = 0;
static uint64_t g_persist_flushes      = 0;
static uint64_t g_persist_failures     = 0;
static uint32_t g_console_level        = SLOG_DEFAULT_CONSOLE_LEVEL;

/* ---- helpers ----------------------------------------------------- */

static uint64_t slog_now_ms(void) {
    uint32_t hz = pit_hz();
    if (!hz) return 0;
    /* pit_ticks() is monotonic from boot; convert ticks->ms cleanly. */
    return (pit_ticks() * 1000ull) / hz;
}

static void copy_field(char *dst, size_t cap, const char *src) {
    if (!cap) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t i = 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

const char *slog_level_name(uint32_t level) {
    switch (level) {
    case ABI_SLOG_LEVEL_ERROR: return "ERROR";
    case ABI_SLOG_LEVEL_WARN:  return "WARN";
    case ABI_SLOG_LEVEL_INFO:  return "INFO";
    case ABI_SLOG_LEVEL_DEBUG: return "DEBUG";
    default:                   return "?";
    }
}

uint32_t slog_level_from_name(const char *name) {
    if (!name) return ABI_SLOG_LEVEL_MAX;
    /* Tiny case-insensitive compare against the four names. */
    char buf[8] = { 0 };
    size_t i = 0;
    for (; i < sizeof(buf) - 1 && name[i]; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        buf[i] = c;
    }
    buf[i] = '\0';
    if (!strcmp(buf, "ERROR") || !strcmp(buf, "ERR"))   return ABI_SLOG_LEVEL_ERROR;
    if (!strcmp(buf, "WARN")  || !strcmp(buf, "WARNING")) return ABI_SLOG_LEVEL_WARN;
    if (!strcmp(buf, "INFO"))                            return ABI_SLOG_LEVEL_INFO;
    if (!strcmp(buf, "DEBUG"))                           return ABI_SLOG_LEVEL_DEBUG;
    return ABI_SLOG_LEVEL_MAX;
}

/* Print a single record to kprintf. Callers must NOT hold g_ring_lock
 * (kprintf has its own lock and we want to avoid lock-order issues). */
static void emit_console(const struct abi_slog_record *r) {
    /* Format:  [time_ms] LEVEL  sub  pid=X: msg */
    if (r->pid >= 0) {
        kprintf("[%llu] %-5s %-8s pid=%d: %s\n",
                (unsigned long long)r->time_ms,
                slog_level_name(r->level),
                r->sub[0] ? r->sub : "kernel",
                (int)r->pid,
                r->msg);
    } else {
        kprintf("[%llu] %-5s %-8s: %s\n",
                (unsigned long long)r->time_ms,
                slog_level_name(r->level),
                r->sub[0] ? r->sub : "kernel",
                r->msg);
    }
}

/* Insert a record into the ring at the next slot. Must be called with
 * g_ring_lock held + IRQs masked. Returns the seq it assigned. */
static uint64_t ring_push_locked(uint32_t level, int32_t pid,
                                 const char *sub, const char *body) {
    uint64_t seq  = ++g_seq;
    uint64_t slot = (seq - 1) & (ABI_SLOG_RING_DEPTH - 1);
    /* If we're about to overwrite a slot that hasn't been drained yet
     * (i.e. its seq is still > 0 and we've wrapped), bump dropped. */
    if (g_ring[slot].seq != 0 && g_ring[slot].seq + ABI_SLOG_RING_DEPTH <= seq) {
        g_dropped++;
        g_dropped_peek++;
        if (g_dropped_peek > 0xFFFFu) g_dropped_peek = 0xFFFFu;
    }
    struct abi_slog_record *r = &g_ring[slot];
    r->seq      = seq;
    r->time_ms  = slog_now_ms();
    r->level    = level;
    r->pid      = pid;
    r->dropped  = 0;
    r->flags    = 0;
    r->_pad0    = 0;
    r->_reserved[0] = 0;
    copy_field(r->sub, sizeof(r->sub), sub ? sub : SLOG_SUB_KERNEL);
    copy_field(r->msg, sizeof(r->msg), body ? body : "");
    g_total_emitted++;
    if (level < ABI_SLOG_LEVEL_MAX) g_per_level[level]++;
    return seq;
}

/* ---- public API -------------------------------------------------- */

void slog_init(void) {
    if (g_ready) return;
    /* Zero the ring explicitly: the `seq == 0` sentinel is what
     * ring_push_locked uses to tell "never written" from "ancient". */
    for (uint32_t i = 0; i < ABI_SLOG_RING_DEPTH; i++) {
        g_ring[i].seq = 0;
        g_ring[i].time_ms = 0;
        g_ring[i].level = ABI_SLOG_LEVEL_INFO;
        g_ring[i].pid   = -1;
        g_ring[i].sub[0] = '\0';
        g_ring[i].msg[0] = '\0';
        g_ring[i].dropped = 0;
        g_ring[i].flags = 0;
    }
    g_seq = 0;
    g_dropped = 0;
    g_dropped_peek = 0;
    g_persist_seq = 0;
    g_total_emitted = 0;
    for (uint32_t l = 0; l < ABI_SLOG_LEVEL_MAX; l++) g_per_level[l] = 0;
    g_persist_bytes = 0;
    g_persist_flushes = 0;
    g_persist_failures = 0;
    g_console_level = SLOG_DEFAULT_CONSOLE_LEVEL;
    g_ready = true;
    /* First record is our own boot marker so logview always shows
     * something even if no other subsystem has logged yet. */
    SLOG_INFO(SLOG_SUB_SLOG, "slog ready (depth=%u, persist=%s)",
              (unsigned)ABI_SLOG_RING_DEPTH, SLOG_PERSIST_PATH);
}

bool slog_ready(void) { return g_ready; }

void slog_vemit(uint32_t level, const char *sub, const char *fmt, va_list ap) {
    if (!g_ready) {
        /* Ring not up yet -- still hit the console so very-early code
         * isn't silently dropped. */
        kprintf("[slog/early] %s %s: ", slog_level_name(level),
                sub ? sub : SLOG_SUB_KERNEL);
        kvprintf(fmt, ap);
        kprintf("\n");
        return;
    }
    if (level >= ABI_SLOG_LEVEL_MAX) level = ABI_SLOG_LEVEL_INFO;

    /* Format into a stack buffer first; never hold the ring lock
     * across the formatter (which itself takes printk_lock if it
     * tried to bounce, etc.). */
    char body[ABI_SLOG_MSG_MAX];
    int  n = kvsnprintf(body, sizeof(body), fmt, ap);
    if (n < 0) { body[0] = '?'; body[1] = '\0'; }
    if ((size_t)n >= sizeof(body)) {
        /* Truncated -- mark with "..." so readers know. */
        if (sizeof(body) >= 4) {
            body[sizeof(body) - 4] = '.';
            body[sizeof(body) - 3] = '.';
            body[sizeof(body) - 2] = '.';
            body[sizeof(body) - 1] = '\0';
        }
    }

    int32_t pid = -1;
    /* current_proc() is safe early-boot; returns NULL if no process */
    struct proc *p = current_proc();
    if (p) pid = (int32_t)p->pid;

    struct abi_slog_record copy_for_console;
    {
        uint64_t flags = spin_lock_irqsave(&g_ring_lock);
        uint64_t seq = ring_push_locked(level, pid, sub, body);
        /* Snapshot for console fan-out outside the lock. */
        uint64_t slot = (seq - 1) & (ABI_SLOG_RING_DEPTH - 1);
        copy_for_console = g_ring[slot];
        spin_unlock_irqrestore(&g_ring_lock, flags);
    }

    if (level <= g_console_level) {
        emit_console(&copy_for_console);
    }
}

void slog_emit(uint32_t level, const char *sub, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    slog_vemit(level, sub, fmt, ap);
    va_end(ap);
}

void slog_emit_pid(int32_t pid, uint32_t level, const char *sub,
                   const char *msg) {
    if (!g_ready) {
        kprintf("[slog/early-user] %s %s pid=%d: %s\n",
                slog_level_name(level),
                sub ? sub : SLOG_SUB_USER,
                (int)pid,
                msg ? msg : "");
        return;
    }
    if (level >= ABI_SLOG_LEVEL_MAX) level = ABI_SLOG_LEVEL_INFO;

    struct abi_slog_record copy_for_console;
    {
        uint64_t flags = spin_lock_irqsave(&g_ring_lock);
        uint64_t seq = ring_push_locked(level, pid,
                                        sub ? sub : SLOG_SUB_USER,
                                        msg ? msg : "");
        uint64_t slot = (seq - 1) & (ABI_SLOG_RING_DEPTH - 1);
        copy_for_console = g_ring[slot];
        spin_unlock_irqrestore(&g_ring_lock, flags);
    }
    if (level <= g_console_level) {
        emit_console(&copy_for_console);
    }
}

uint32_t slog_drain(struct abi_slog_record *out, uint32_t cap,
                    uint64_t since_seq) {
    if (!out || !cap) return 0;

    uint32_t written = 0;
    uint64_t flags = spin_lock_irqsave(&g_ring_lock);

    /* Walk the ring in seq-ascending order. We have at most
     * ABI_SLOG_RING_DEPTH live entries; the current `g_seq` tells us
     * the highest assigned. */
    uint64_t newest = g_seq;
    uint64_t oldest = (newest > ABI_SLOG_RING_DEPTH)
                          ? (newest - ABI_SLOG_RING_DEPTH + 1)
                          : 1;
    if (since_seq + 1 > oldest) oldest = since_seq + 1;

    for (uint64_t s = oldest; s <= newest && written < cap; s++) {
        uint64_t slot = (s - 1) & (ABI_SLOG_RING_DEPTH - 1);
        if (g_ring[slot].seq != s) continue;  /* slot was overwritten */
        out[written] = g_ring[slot];
        if (written == 0) {
            /* Annotate the first record with how many were dropped
             * since the previous drain. */
            out[0].dropped = (uint16_t)g_dropped_peek;
        }
        written++;
    }
    if (written) {
        /* Reset peek counter once a drain saw it. */
        g_dropped_peek = 0;
    }
    spin_unlock_irqrestore(&g_ring_lock, flags);
    return written;
}

void slog_stats(struct abi_slog_stats *out) {
    if (!out) return;
    uint64_t flags = spin_lock_irqsave(&g_ring_lock);
    out->total_emitted    = g_total_emitted;
    out->total_dropped    = g_dropped;
    for (uint32_t l = 0; l < ABI_SLOG_LEVEL_MAX; l++) {
        out->per_level[l] = g_per_level[l];
    }
    out->persist_bytes    = g_persist_bytes;
    out->persist_flushes  = g_persist_flushes;
    out->persist_failures = g_persist_failures;
    out->ring_depth       = ABI_SLOG_RING_DEPTH;
    /* in_use = number of slots whose seq is non-zero. Cheap O(N). */
    uint32_t in_use = 0;
    for (uint32_t i = 0; i < ABI_SLOG_RING_DEPTH; i++) {
        if (g_ring[i].seq != 0) in_use++;
    }
    out->ring_in_use      = in_use;
    spin_unlock_irqrestore(&g_ring_lock, flags);
}

uint32_t slog_console_level(void) { return g_console_level; }
void slog_set_console_level(uint32_t level) {
    if (level > ABI_SLOG_LEVEL_DEBUG) level = ABI_SLOG_LEVEL_DEBUG;
    g_console_level = level;
}

/* Format a single record into a textual line ending with '\n'.
 * Returns the number of bytes written (excluding NUL). */
static int format_line(char *buf, size_t cap, const struct abi_slog_record *r) {
    if (r->pid >= 0) {
        return ksnprintf(buf, cap, "[%llu] %-5s %-8s pid=%d: %s\n",
                         (unsigned long long)r->time_ms,
                         slog_level_name(r->level),
                         r->sub[0] ? r->sub : "kernel",
                         (int)r->pid,
                         r->msg);
    }
    return ksnprintf(buf, cap, "[%llu] %-5s %-8s: %s\n",
                     (unsigned long long)r->time_ms,
                     slog_level_name(r->level),
                     r->sub[0] ? r->sub : "kernel",
                     r->msg);
}

int slog_persist_flush(void) {
    if (!g_ready) return -1;

    /* Snapshot the ring under the lock, then do all the VFS work
     * outside. This is bounded: at most ABI_SLOG_RING_DEPTH records,
     * each formatted line ~= sub(8) + msg(<=192) + headers(~32) bytes. */
    static struct abi_slog_record snap[ABI_SLOG_RING_DEPTH];
    uint32_t n = 0;
    uint64_t hi_seq = 0;
    {
        uint64_t flags = spin_lock_irqsave(&g_ring_lock);
        uint64_t newest = g_seq;
        uint64_t oldest = (newest > ABI_SLOG_RING_DEPTH)
                              ? (newest - ABI_SLOG_RING_DEPTH + 1)
                              : 1;
        for (uint64_t s = oldest; s <= newest; s++) {
            uint64_t slot = (s - 1) & (ABI_SLOG_RING_DEPTH - 1);
            if (g_ring[slot].seq != s) continue;
            snap[n++] = g_ring[slot];
            hi_seq = s;
        }
        spin_unlock_irqrestore(&g_ring_lock, flags);
    }
    if (n == 0) return 0;

    /* Build the on-disk buffer. Reasonable upper bound:
     *   per-line ceiling ~ ABI_SLOG_MSG_MAX + 64 = 256 bytes
     *   total           ~ ring_depth * 256 = 64 KiB
     * SLOG_PERSIST_CAP_BYTES caps it. We allocate once on the static
     * .bss to keep this leaf-callable from boot paths. */
    static char buf[SLOG_PERSIST_CAP_BYTES];
    size_t off = 0;
    /* Header line so a sysadmin can spot a fresh boot in the log. */
    int hd = ksnprintf(buf + off, sizeof(buf) - off,
                       "==== tobyOS system log (slog) -- %u records ====\n",
                       (unsigned)n);
    if (hd > 0) off += (size_t)hd;
    for (uint32_t i = 0; i < n && off + 32 < sizeof(buf); i++) {
        int wn = format_line(buf + off, sizeof(buf) - off, &snap[i]);
        if (wn <= 0) break;
        off += (size_t)wn;
    }
    /* If we ran out of room mid-write, leave a marker. */
    if (off + 4 >= sizeof(buf)) {
        const char *trunc = "...(truncated)\n";
        size_t tl = strlen(trunc);
        if (off + tl < sizeof(buf)) {
            for (size_t k = 0; k < tl; k++) buf[off + k] = trunc[k];
            off += tl;
        }
    }
    int rc = vfs_write_all(SLOG_PERSIST_PATH, buf, off);
    if (rc != 0) {
        g_persist_failures++;
        return rc;
    }
    g_persist_bytes  += off;
    g_persist_flushes++;
    g_persist_seq    = hi_seq;
    return 0;
}

void slog_dump_kprintf(void) {
    if (!g_ready) {
        kprintf("[slog] (not initialised)\n");
        return;
    }
    /* Snapshot then dump outside the lock. */
    static struct abi_slog_record snap[ABI_SLOG_RING_DEPTH];
    uint32_t n = 0;
    {
        uint64_t flags = spin_lock_irqsave(&g_ring_lock);
        uint64_t newest = g_seq;
        uint64_t oldest = (newest > ABI_SLOG_RING_DEPTH)
                              ? (newest - ABI_SLOG_RING_DEPTH + 1)
                              : 1;
        for (uint64_t s = oldest; s <= newest; s++) {
            uint64_t slot = (s - 1) & (ABI_SLOG_RING_DEPTH - 1);
            if (g_ring[slot].seq != s) continue;
            snap[n++] = g_ring[slot];
        }
        spin_unlock_irqrestore(&g_ring_lock, flags);
    }
    kprintf("[slog] ring dump: %u records, %llu dropped lifetime, "
            "console_level=%s\n",
            (unsigned)n,
            (unsigned long long)g_dropped,
            slog_level_name(g_console_level));
    for (uint32_t i = 0; i < n; i++) emit_console(&snap[i]);
    kprintf("[slog] end ring dump\n");
}
