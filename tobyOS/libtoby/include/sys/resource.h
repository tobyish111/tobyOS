/* sys/resource.h -- the niceness calls (batch 25): getpriority and
 * setpriority, as POSIX and Linux define them (ABI_SYS_GETNICE/SETNICE).
 * No rlimits here: the native ABI has none. */
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

#endif
