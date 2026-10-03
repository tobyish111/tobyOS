/* rlimit.h -- the resource limits tobyOS reports.
 *
 * The kernel enforces none of them: a set is accepted and changes nothing,
 * and every read answers from here. The Linux personality's prlimit64 and
 * getrlimit (src/syscall.c) and /bin/tsh's `ulimit` (src/shell.c) all use
 * this one table, so a Linux bash and tsh on the same box print the same
 * `ulimit -a` -- they had two tables, and the legacy getrlimit even
 * disagreed with prlimit64 about open files (4096 against PROC_NFDS).
 *
 * RESOURCE is Linux's numbering (RLIMIT_CPU 0 ... RLIMIT_RTTIME 15).
 */
#ifndef TOBYOS_RLIMIT_H
#define TOBYOS_RLIMIT_H

#include <stdint.h>

#define TOBYOS_RLIM_INFINITY (~0ULL)

#define TOBYOS_RLIMIT_STACK   3
#define TOBYOS_RLIMIT_NOFILE  7

static inline void tobyos_rlimit(int resource, uint64_t nofile,
                                 uint64_t *cur, uint64_t *max) {
    *max = TOBYOS_RLIM_INFINITY;
    switch (resource) {
    case TOBYOS_RLIMIT_STACK:  *cur = 0x800000ULL; break;      /* 8 MiB */
    case TOBYOS_RLIMIT_NOFILE: *cur = nofile; *max = nofile; break;
    default:                   *cur = TOBYOS_RLIM_INFINITY; break;
    }
}

#endif
