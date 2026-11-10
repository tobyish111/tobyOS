#ifndef TOBYOS_SYSVIPC_H
#define TOBYOS_SYSVIPC_H

#include <stddef.h>
#include <stdint.h>

/* System V semaphores + message queues (Phase H, 2026-08-22). See
 * src/sysvipc.c. All return ABI-negative errnos; the only callers are
 * the Linux syscall arms plus the exit hook. */

long sysv_semget(int key, int nsems, int flags);
long sysv_semop (int id, uint64_t usops, int nops, uint64_t deadline_ns);
long sysv_semctl(int id, int semnum, int cmd, uint64_t arg);

long sysv_msgget(int key, int flags);
long sysv_msgsnd(int id, uint64_t umsgp, size_t msgsz, int flags);
long sysv_msgrcv(int id, uint64_t umsgp, size_t msgsz, long msgtyp,
                 int flags);
long sysv_msgctl(int id, int cmd, uint64_t arg);

/* Exit hook: apply SEM_UNDO adjustments for a dying process (tgid). */
void sysv_release_proc(int tgid);

#endif
