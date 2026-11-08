/* sys/resource.h -- the niceness calls (batch 25): getpriority and
 * setpriority, as POSIX and Linux define them (ABI_SYS_GETNICE/SETNICE);
 * and the resource limits (batch 47): getrlimit, setrlimit and prlimit over
 * ABI_SYS_PRLIMIT, each process's own table with Linux's numbering and
 * rules (include/tobyos/rlimit.h). */
#ifndef _SYS_RESOURCE_H
#define _SYS_RESOURCE_H

#define PRIO_PROCESS 0      /* WHO is a process (0: the caller) */
#define PRIO_PGRP    1      /* WHO is a process group (0: the caller's) */
#define PRIO_USER    2      /* WHO is a real uid (0: the caller's) */

/* the lowest niceness (-20..19) among WHICH/WHO's processes; -1 with errno
 * set for an error -- and -1 is also a niceness, so clear errno first */
int getpriority(int which, int who);
/* 0, or -1 with errno: ESRCH none, EPERM another user's, EACCES lower */
int setpriority(int which, int who, int prio);

typedef unsigned long long rlim_t;
#define RLIM_INFINITY  (~0ULL)
#define RLIM_SAVED_MAX RLIM_INFINITY
#define RLIM_SAVED_CUR RLIM_INFINITY

struct rlimit {
    rlim_t rlim_cur;        /* the soft limit: what is enforced */
    rlim_t rlim_max;        /* the hard limit: the soft one's ceiling */
};

#define RLIMIT_CPU        0
#define RLIMIT_FSIZE      1
#define RLIMIT_DATA       2
#define RLIMIT_STACK      3
#define RLIMIT_CORE       4
#define RLIMIT_RSS        5
#define RLIMIT_NPROC      6
#define RLIMIT_NOFILE     7
#define RLIMIT_MEMLOCK    8
#define RLIMIT_AS         9
#define RLIMIT_LOCKS      10
#define RLIMIT_SIGPENDING 11
#define RLIMIT_MSGQUEUE   12
#define RLIMIT_NICE       13
#define RLIMIT_RTPRIO     14
#define RLIMIT_RTTIME     15
#define RLIMIT_NLIMITS    16
#define RLIM_NLIMITS      RLIMIT_NLIMITS

/* 0, or -1 with errno: EINVAL a resource past 15 or a soft limit above
 * its hard one; EPERM raising a hard limit without root, or the open-file
 * one past 1024 (the descriptor table); EFAULT a bad pointer. Only
 * RLIMIT_NOFILE is enforced; the rest are kept and reported. */
int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);
/* Linux's prlimit: PID 0 the caller; NEW and OLD each may be 0; OLD is
 * written only when the call succeeds. ESRCH no such process, EPERM
 * another user's without root. */
int prlimit(int pid, int resource, const struct rlimit *new_limit,
            struct rlimit *old_limit);

#endif
