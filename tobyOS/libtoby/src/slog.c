/* libtoby/src/slog.c -- Milestone 28A userland wrappers around the
 * SLOG_* syscalls. Layout mirrors libtoby/src/devtest.c -- thin
 * syscall wrappers + a single shared table renderer that every tool
 * (logview, stabilitytest, ...) calls so the column shape stays
 * stable across the OS.
 */

#include <tobyos_slog.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "libtoby_internal.h"

int tobylog_read(struct abi_slog_record *out, size_t cap,
                 uint64_t since_seq) {
    if (cap == 0) return 0;
    if (!out) { errno = EFAULT; return -1; }
    if (cap > ABI_SLOG_RING_DEPTH) cap = ABI_SLOG_RING_DEPTH;
    long rv = toby_sc3(ABI_SYS_SLOG_READ,
                       (long)(uintptr_t)out,
                       (long)cap,
                       (long)since_seq);
    return (int)__toby_check(rv);
}

int tobylog_write(unsigned int level, const char *sub, const char *msg) {
    if (!sub || !msg) { errno = EFAULT; return -1; }
    if (level >= ABI_SLOG_LEVEL_MAX) { errno = EINVAL; return -1; }
    long rv = toby_sc3(ABI_SYS_SLOG_WRITE,
                       (long)level,
                       (long)(uintptr_t)sub,
                       (long)(uintptr_t)msg);
    return (int)__toby_check(rv);
}

int tobylog_stats(struct abi_slog_stats *out) {
    if (!out) { errno = EFAULT; return -1; }
    long rv = toby_sc1(ABI_SYS_SLOG_STATS,
                       (long)(uintptr_t)out);
    return (int)__toby_check(rv);
}

const char *tobylog_level_str(unsigned int level) {
    switch (level) {
    case ABI_SLOG_LEVEL_ERROR: return "ERROR";
    case ABI_SLOG_LEVEL_WARN:  return "WARN";
    case ABI_SLOG_LEVEL_INFO:  return "INFO";
    case ABI_SLOG_LEVEL_DEBUG: return "DEBUG";
    default:                   return "?";
    }
}

unsigned int tobylog_level_from_str(const char *name) {
    if (!name) return ABI_SLOG_LEVEL_MAX;
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

void tobylog_print_header(FILE *fp) {
    if (!fp) return;
    fprintf(fp, "%-7s %-10s %-5s %-10s %-5s %s\n",
            "SEQ", "TIME(ms)", "LEVEL", "SUB", "PID", "MESSAGE");
    fprintf(fp, "%-7s %-10s %-5s %-10s %-5s %s\n",
            "-----", "--------", "-----", "---", "---", "-------");
}

void tobylog_print_record(FILE *fp, const struct abi_slog_record *r) {
    if (!fp || !r) return;
    char pidbuf[8];
    if (r->pid < 0) {
        pidbuf[0] = '-'; pidbuf[1] = '\0';
    } else {
        snprintf(pidbuf, sizeof(pidbuf), "%d", (int)r->pid);
    }
    /* libtoby printf supports %lu (long) but not %llu, so cap to long.
     * On x86_64 this is identical to 64-bit. */
    fprintf(fp, "%-7lu %-10lu %-5s %-10s %-5s %s\n",
            (unsigned long)r->seq,
            (unsigned long)r->time_ms,
            tobylog_level_str(r->level),
            r->sub[0] ? r->sub : "kernel",
            pidbuf,
            r->msg);
}
