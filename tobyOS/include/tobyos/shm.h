/* shm.h -- POSIX + SysV shared memory */
#ifndef TOBYOS_SHM_H
#define TOBYOS_SHM_H

#include <stddef.h>
#include <stdint.h>

void shm_init(void);

/* POSIX (native ABI) */
long sys_shm_open(const char *name, int flags, size_t size);
long sys_shm_map(int shm_id, uint64_t hint_addr);
long sys_shm_unlink(const char *name);

/* SysV (Linux personality) */
long sys_shmget(int key, size_t size, int shmflg);
long sys_shmat(int shmid, uint64_t shmaddr, int shmflg);
long sys_shmdt(uint64_t shmaddr);
long sys_shmctl(int shmid, int cmd, void *buf);

/* Kernel helpers for the fake X MIT-SHM path */
int  sysv_shm_lookup(int shmid, uint64_t **pages_out, size_t *npages_out,
                     size_t *size_out);
void     *sysv_shm_kva(int shmid);   /* HHDM pointer to first page, or NULL */
size_t    sysv_shm_size(int shmid);

/* Ensure a segment exists for key (create if absent). Returns shmid or <0. */
long sysv_shm_ensure(int key, size_t size);

/* ---- IPC namespace (Phase 3 slice 8) ---------------------------------
 * `struct ipc_ns` lives HERE, next to the tables it contains, for the same
 * reason `struct mount_ns` lives in vfs.c: the namespace is the payload plus a
 * refcount, and the payload's owner is the only file that should be able to
 * touch it. nsproxy.c drives the lifecycle through these four calls and never
 * sees the inside.
 *
 * A namespace pointer of NULL means the INITIAL namespace throughout (see
 * nsproxy.h), so get/put accept NULL as a no-op and inum(NULL) reports the
 * initial namespace's number.
 *
 * ipc_ns_create() returns an EMPTY namespace, not a copy of the caller's. That
 * asymmetry with the UTS namespace (which copies the hostname) is deliberate
 * and matches Linux: entering a new IPC namespace must make existing segments
 * INVISIBLE, or it isolates nothing. */
void    *ipc_ns_create(void);
void     ipc_ns_get(void *ns);
void     ipc_ns_put(void *ns);
uint64_t ipc_ns_inum(void *ns);

/* Number of live SysV segments in `ns` (NULL == initial). Diagnostics only. */
int      ipc_ns_nsegs(void *ns);

#endif
