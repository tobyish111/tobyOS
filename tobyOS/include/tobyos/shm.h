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

#endif
