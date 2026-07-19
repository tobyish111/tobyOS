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

/* ---- memfd (memfd_create) -- anonymous, page-backed, mmap-COHERENT shared
 * memory. Unlike file-backed mmap (which copies file bytes into private anon
 * pages), a memfd owns a list of physical pages; every mmap of the memfd maps
 * those SAME pages, so writes through one mapping (or read/write()) are visible
 * through every other mapping -- the sharing semantics chrome's compositor /
 * base::*SharedMemoryRegion needs. Backing pages are freed only when the last
 * fd (refcount) closes; munmap of a memfd VMA (VMA_FLAG_NOFREE) leaves them. */
struct memfd;
struct memfd *memfd_new(void);              /* refcount 1; NULL on OOM */
void  memfd_ref(struct memfd *mf);
void  memfd_unref(struct memfd *mf);        /* frees pages+struct at 0 */
long  memfd_ftruncate(struct memfd *mf, uint64_t size);
long  memfd_read (struct memfd *mf, uint64_t pos, void *dst, size_t n);
long  memfd_write(struct memfd *mf, uint64_t pos, const void *src, size_t n);
uint64_t memfd_size(struct memfd *mf);
/* Map [offset,offset+len) of the memfd into the current proc (growing the memfd
 * if needed). `flags` uses the internal VMA_FLAG_* set; returns the base vaddr
 * or a negative errno. */
long  memfd_map(uint64_t addr, uint64_t len, uint32_t prot, uint32_t flags,
                struct memfd *mf, uint64_t offset);
/* File sealing (fcntl F_ADD_SEALS/F_GET_SEALS). Chrome's Mojo shared-memory
 * channel (mojo/core/channel_linux.cc) seals its memfd (F_SEAL_SHRINK/GROW) and
 * then CHECKs F_GET_SEALS reports them back -- returning 0 there is a FATAL
 * "Check failed". add_seals returns 0 on success or a negative errno. */
long  memfd_add_seals(struct memfd *mf, unsigned int seals);
long  memfd_get_seals(struct memfd *mf);

#endif /* TOBYOS_MMAP_H */
