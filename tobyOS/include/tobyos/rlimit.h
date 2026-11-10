/* rlimit.h -- resource limits: Linux's sixteen, soft and hard, per process.
 *
 * Each process keeps its own table (struct proc's rlim, src/proc.c), which
 * fork, clone and spawn copy to the child and exec keeps. getrlimit,
 * setrlimit and prlimit64 (the Linux personality) and ABI_SYS_PRLIMIT (the
 * native one, /bin/tsh's `ulimit`) read and set it with Linux's rules:
 *
 *   - a soft limit above its hard limit is EINVAL;
 *   - raising a hard limit takes root (Linux's CAP_SYS_RESOURCE), EPERM;
 *   - the open-file hard limit can go no higher than TOBYOS_NR_OPEN, the
 *     size of a process's descriptor table (Linux's fs.nr_open), EPERM even
 *     for root;
 *   - a resource past the sixteen is EINVAL.
 *
 * RLIMIT_NOFILE IS ENFORCED (batch 47): a descriptor is only ever given out
 * below the soft limit -- EMFILE when there is none free -- dup2/dup3 to a
 * number at or past it is EBADF, F_DUPFD from one is EINVAL, and poll over
 * more descriptors than it is EINVAL. Descriptors already open above a
 * limit lowered under them stay open and usable, as on Linux. The other
 * fifteen are kept and reported, but nothing here acts on them yet.
 *
 * Until a process's table is first set it reads as these defaults: what
 * every process was told before there were tables, when one table was
 * reported for all and nothing set was kept -- `ulimit -n 128` changed
 * nothing a later `ulimit -n` printed (handoff section 41's Not done).
 *
 * RESOURCE is Linux's numbering (RLIMIT_CPU 0 ... RLIMIT_RTTIME 15).
 */
#ifndef TOBYOS_RLIMIT_H
#define TOBYOS_RLIMIT_H

#include <stdint.h>

#define TOBYOS_RLIM_INFINITY (~0ULL)
#define TOBYOS_RLIM_NLIMITS  16

#define TOBYOS_RLIMIT_FSIZE   1
#define TOBYOS_RLIMIT_STACK   3
#define TOBYOS_RLIMIT_NOFILE  7

/* a process's descriptor table, and so the open-file hard limit's ceiling
 * (PROC_NFDS is this) */
#define TOBYOS_NR_OPEN 1024

struct tobyos_rlim {
    uint64_t cur, max;
};

static inline void tobyos_rlimit_default(int resource, uint64_t *cur,
                                         uint64_t *max) {
    *max = TOBYOS_RLIM_INFINITY;
    switch (resource) {
    case TOBYOS_RLIMIT_STACK:  *cur = 0x800000ULL; break;      /* 8 MiB */
    case TOBYOS_RLIMIT_NOFILE: *cur = TOBYOS_NR_OPEN; *max = TOBYOS_NR_OPEN; break;
    default:                   *cur = TOBYOS_RLIM_INFINITY; break;
    }
}

/* The rules for setting resource RES from OLD to NEW, as Linux's: 0, or the
 * error as a positive errno value of Linux's numbering -- 22 (EINVAL) or
 * 1 (EPERM). PRIV: the caller may raise a hard limit. One function for the
 * kernel and for the kernel shell's own table (src/shell.c). */
static inline int tobyos_rlimit_check(int resource,
                                      const struct tobyos_rlim *old,
                                      const struct tobyos_rlim *nw,
                                      int priv) {
    if (resource < 0 || resource >= TOBYOS_RLIM_NLIMITS) return 22;
    if (nw->cur > nw->max) return 22;
    if (resource == TOBYOS_RLIMIT_NOFILE && nw->max > TOBYOS_NR_OPEN) return 1;
    if (nw->max > old->max && !priv) return 1;
    return 0;
}

#endif
