/* tobyos_slog.h -- libtoby userland wrappers for the M28A structured
 * logging syscalls (SYS_SLOG_READ, SYS_SLOG_WRITE, SYS_SLOG_STATS).
 *
 * The kernel ABI returns negative ABI_E* on failure / non-negative on
 * success; these wrappers translate to the POSIX-shape "errno + -1"
 * convention every other libtoby call uses, and add a few small
 * helpers (level/sub formatting, table renderer) so logview and any
 * other consumer of the slog ring share one print path.
 */

#ifndef LIBTOBY_TOBYOS_SLOG_H
#define LIBTOBY_TOBYOS_SLOG_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <tobyos/abi/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Drain at most `cap` records from the kernel ring into `out`,
 * skipping any whose seq <= since_seq. Returns the number of records
 * written (>= 0) on success. On failure returns -1 and sets errno.
 *
 * The first returned record's `dropped` field carries the count of
 * records lost since the previous drain (one big bump rather than
 * per-event bookkeeping). */
int tobylog_read(struct abi_slog_record *out, size_t cap,
                 uint64_t since_seq);

/* Post a structured log record into the kernel ring. `sub` is the
 * subsystem tag (e.g. "user", "shell"); `msg` is the line body.
 * Returns 0 on success, -1 with errno set on failure. */
int tobylog_write(unsigned int level, const char *sub, const char *msg);

/* Snapshot the kernel-side counters into `out`. Returns 0 on
 * success, -1 with errno set on failure. */
int tobylog_stats(struct abi_slog_stats *out);

/* Translate a level number to its printable name (e.g. "INFO").
 * Never returns NULL; unknown levels return "?". */
const char *tobylog_level_str(unsigned int level);

/* Translate a level name back to a numeric value. Case-insensitive.
 * Returns ABI_SLOG_LEVEL_MAX on unknown. */
unsigned int tobylog_level_from_str(const char *name);

/* Stable column header + per-record formatter. Used by `logview` so
 * its output has the same shape regardless of who built the binary. */
void tobylog_print_header(FILE *fp);
void tobylog_print_record(FILE *fp, const struct abi_slog_record *r);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_TOBYOS_SLOG_H */
