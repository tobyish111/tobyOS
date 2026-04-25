/* slog.h -- Milestone 28A: structured kernel + userland log framework.
 *
 * "klog" was already taken in this tree (perf.c uses it as a category-
 * bitmask debug stream), so the new structured logger is `slog`. It
 * is intentionally orthogonal:
 *
 *   - kprintf()   -> raw bytes to serial + framebuffer console.
 *   - klog()      -> category-gated debug output (perf.c).
 *   - slog_*()    -> structured records into a kernel ring buffer,
 *                    fanned out to console via kprintf, drainable from
 *                    userland via the SYS_SLOG_READ syscall, and
 *                    flushed to /data/system.log on shutdown / on
 *                    demand. This is THE system log a sysadmin reads.
 *
 * Design properties:
 *
 *   - Single ring buffer (ABI_SLOG_RING_DEPTH = 256 entries). Each
 *     entry is 256 bytes (struct abi_slog_record). Total resident
 *     footprint ~64 KiB.
 *   - Writes are spinlock-protected with IRQs masked, so safe from
 *     IRQ context (PIT, keyboard, watchdog).
 *   - Records carry: seq (per-boot monotonic), time_ms (boot clock),
 *     level, sub tag, pid (-1 = kernel), and 192-byte message.
 *   - Console fan-out is gated by a global level threshold (default
 *     INFO). DEBUG records still hit the ring; they just don't spam
 *     the framebuffer / serial unless explicitly enabled.
 *   - Persistence: slog_persist_flush() walks the ring writing any
 *     never-flushed records out via VFS to /data/system.log, in
 *     line-oriented text. Old text is kept; the file just grows.
 *     Truncated to ABI_SLOG_PERSIST_CAP_BYTES on overflow.
 */

#ifndef TOBYOS_SLOG_H
#define TOBYOS_SLOG_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>

/* On-disk persistence cap. Beyond this we rotate by truncating the
 * head and rewriting from the latest seq. 256 KiB is plenty for a
 * stability test boot without bloating the ramfs image. */
#define SLOG_PERSIST_CAP_BYTES   (256u * 1024u)

/* Default console fan-out threshold. Records with level <= threshold
 * are echoed via kprintf. Default = INFO (matches existing behaviour
 * of a "loud" kernel). */
#define SLOG_DEFAULT_CONSOLE_LEVEL  ABI_SLOG_LEVEL_INFO

/* Initialise the ring + counters. Idempotent. Called early from
 * _start, BEFORE any subsystem might want to log. After this returns,
 * slog_emit() is safe from any context. */
void slog_init(void);

/* Have we run slog_init() yet? Used by very early code that gates
 * its own logging on slog readiness. */
bool slog_ready(void);

/* Core emission API. `sub` is a short subsystem tag (e.g. "kernel",
 * "fs", "net", "gui", "driver"). Truncated to ABI_SLOG_SUB_MAX-1
 * bytes if too long. `fmt` is kprintf-style; the formatted body is
 * truncated to ABI_SLOG_MSG_MAX-1 bytes (with a trailing "..." marker
 * on truncation). Always safe; never blocks; never allocates. */
void slog_emit(uint32_t level, const char *sub, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

void slog_vemit(uint32_t level, const char *sub, const char *fmt,
                __builtin_va_list ap);

/* Convenience macros. They expand to a single call so callers don't
 * have to spell out the level constant or pass NULL strings. */
#define SLOG_INFO(sub, ...)  slog_emit(ABI_SLOG_LEVEL_INFO,  (sub), __VA_ARGS__)
#define SLOG_WARN(sub, ...)  slog_emit(ABI_SLOG_LEVEL_WARN,  (sub), __VA_ARGS__)
#define SLOG_ERROR(sub, ...) slog_emit(ABI_SLOG_LEVEL_ERROR, (sub), __VA_ARGS__)
#define SLOG_DEBUG(sub, ...) slog_emit(ABI_SLOG_LEVEL_DEBUG, (sub), __VA_ARGS__)

/* Emit on behalf of a userland process (pid is recorded so logview
 * can show which app posted what). Used by the SYS_SLOG_WRITE
 * implementation. */
void slog_emit_pid(int32_t pid, uint32_t level, const char *sub,
                   const char *msg);

/* Drain the ring into `out`. Returns number of records written.
 * Filters by `since_seq` (only records with seq > since_seq).
 * Caller is responsible for buffer sizing. */
uint32_t slog_drain(struct abi_slog_record *out, uint32_t cap,
                    uint64_t since_seq);

/* Snapshot counters into `out`. */
void slog_stats(struct abi_slog_stats *out);

/* Console fan-out threshold accessors. Records with level <=
 * threshold are echoed via kprintf. */
uint32_t slog_console_level(void);
void     slog_set_console_level(uint32_t level);

/* Persist the ring out to /data/system.log, appending any new
 * records since the last flush. Safe to call repeatedly. Returns 0
 * on success, negative ABI_E* on failure. Must be called from a
 * sleepable context (yield-safe). */
int slog_persist_flush(void);

/* Path of the persistent log file. Single source of truth so the
 * kernel and the userland logview agree. */
#define SLOG_PERSIST_PATH  "/data/system.log"

/* Best-effort dump the entire ring to kprintf. Used by the panic
 * path so the last few seconds of context end up in the serial log
 * even if we never get to flush. */
void slog_dump_kprintf(void);

/* Look up a level's printable name (e.g. "INFO"). Returns a static
 * string; never NULL. Unknown levels return "?". */
const char *slog_level_name(uint32_t level);

/* Convert a level name back to its numeric value. Case-insensitive.
 * Returns ABI_SLOG_LEVEL_MAX on unknown. */
uint32_t slog_level_from_name(const char *name);

/* === Subsystem string constants used across the tree. === */
#define SLOG_SUB_KERNEL    "kernel"
#define SLOG_SUB_BOOT      "boot"
#define SLOG_SUB_FS        "fs"
#define SLOG_SUB_NET       "net"
#define SLOG_SUB_GUI       "gui"
#define SLOG_SUB_DRIVER    "driver"
#define SLOG_SUB_PROC      "proc"
#define SLOG_SUB_INPUT     "input"
#define SLOG_SUB_AUDIO     "audio"
#define SLOG_SUB_DISPLAY   "display"
#define SLOG_SUB_SVC       "svc"
#define SLOG_SUB_PANIC     "panic"
#define SLOG_SUB_WDOG      "wdog"
#define SLOG_SUB_SAFE      "safe"
#define SLOG_SUB_USER      "user"
#define SLOG_SUB_SLOG      "slog"
/* M29: hardware bring-up framework (hwinfo, drvmatch, boot diag,
 * regression guard, bringuptest). Use the same tag everywhere so
 * `logview --sub=hw` shows the whole story. */
#define SLOG_SUB_HW        "hw"
/* M34F audit subsystem. All security-relevant denials, package
 * lifecycle events, signature/hash failures, and login/session
 * events tag themselves with this. The userland `auditlog` tool
 * filters the slog ring on this tag. */
#define SLOG_SUB_AUDIT     "audit"
/* M34E system-file protection. Logged when a write to a protected
 * path is allowed (via the privileged scope) or denied. */
#define SLOG_SUB_SYSPROT   "sysprot"
/* M34A/B/C package security events (hash + signature verifications).
 * Distinct from the existing pkg manager kprintf output so security
 * tooling can grep just these lines. */
#define SLOG_SUB_SEC       "sec"

#endif /* TOBYOS_SLOG_H */
