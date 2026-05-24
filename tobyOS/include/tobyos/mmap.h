/* mmap.h -- memory-mapped files and anonymous mappings.
 *
 * Phase 1 Depth Pass: Virtual Memory Hardening.
 *
 * Provides POSIX-like mmap/munmap/mprotect/brk syscall implementations.
 * Mappings are lazily allocated via the page fault handler (demand paging).
 * Anonymous mappings use zero-fill-on-demand; file-backed mappings read
 * pages from disk on first access.
 */

#ifndef TOBYOS_MMAP_H
#define TOBYOS_MMAP_H

#include <tobyos/types.h>

/* mmap region layout in user address space */
#define MMAP_BASE 0x7F0000000000ULL

/* Flags for sys_mmap */
#define MAP_ANONYMOUS  (1 << 0)
#define MAP_PRIVATE    (1 << 1)
#define MAP_SHARED     (1 << 2)
#define MAP_FIXED      (1 << 3)

/* Protection flags */
#define PROT_NONE   0
#define PROT_READ   (1 << 0)
#define PROT_WRITE  (1 << 1)
#define PROT_EXEC   (1 << 2)

/* Syscall entry points */
long sys_mmap2(uint64_t addr, size_t length, int prot, int flags,
               int fd, uint64_t offset);
long sys_munmap2(uint64_t addr, size_t length);
long sys_mprotect2(uint64_t addr, size_t length, int prot);
long sys_brk2(uint64_t new_brk);

/* Process lifecycle hooks */
void mmap2_init_proc(int pid);
void mmap2_cleanup_proc(int pid);

/* COW clone for fork: duplicate parent's VMA table into child,
 * marking writable regions as COW. Returns 0 on success. */
int mmap2_cow_clone(int parent_pid, int child_pid);

/* Query: return total mapped bytes for a process. */
uint64_t mmap2_mapped_bytes(int pid);

#endif /* TOBYOS_MMAP_H */
