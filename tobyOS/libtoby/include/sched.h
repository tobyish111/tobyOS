/* sched.h -- the namespace half of Linux's: unshare(2) and the CLONE_NEW*
 * bits it takes (finding 241), the numbers Linux gives them. */
#ifndef LIBTOBY_SCHED_H
#define LIBTOBY_SCHED_H

#define CLONE_NEWTIME    0x00000080
#define CLONE_NEWNS      0x00020000
#define CLONE_NEWCGROUP  0x02000000
#define CLONE_NEWUTS     0x04000000
#define CLONE_NEWIPC     0x08000000
#define CLONE_NEWUSER    0x10000000
#define CLONE_NEWPID     0x20000000
#define CLONE_NEWNET     0x40000000

int unshare(int flags);

#endif
