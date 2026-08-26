/* inotify.h -- basic file watching mechanism (M1.5).
 *
 * Provides a kernel-level inotify API: userspace can create an inotify
 * instance (which acts as a virtual fd), add watches on paths, and
 * read back events when watched files are created, deleted, modified,
 * or renamed.
 *
 * Syscalls:
 *   SYS_INOTIFY_INIT       -> fd (inotify instance)
 *   SYS_INOTIFY_ADD_WATCH  -> wd (watch descriptor)
 *   SYS_INOTIFY_RM_WATCH   -> 0 or error
 *
 * The kernel fires events via inotify_emit() from VFS write paths.
 * Userspace reads events by reading the inotify fd.
 */

#ifndef TOBYOS_INOTIFY_H
#define TOBYOS_INOTIFY_H

#include <tobyos/types.h>
#include <tobyos/abi/abi.h>

#define INOTIFY_MAX_INSTANCES 16
#define INOTIFY_MAX_WATCHES   64
#define INOTIFY_EVENT_QUEUE   32

void    inotify_init_subsystem(void);

long    sys_inotify_init(void);
long    sys_inotify_add_watch(int fd, const char *path, uint32_t mask);
long    sys_inotify_rm_watch(int fd, int wd);

void    inotify_emit(const char *path, uint32_t mask, const char *name);
/* Rename pairs share a cookie so watchers can correlate FROM/TO. */
void    inotify_emit_cookie(const char *path, uint32_t mask,
                            const char *name, uint32_t cookie);

/* 2026-08-22: the pieces that make an instance usable through a REAL fd
 * (FILE_KIND_INOTIFY). Before these, sys_inotify_init's return value was
 * an instance INDEX -- the first caller got 0, which userspace then used
 * as an fd and read stdin. */
long    inotify_read(int id, void *buf, size_t n);  /* Linux wire format */
bool    inotify_readable(int id);
void    inotify_ref(int id);        /* dup/fork share the instance */
void    inotify_set_nonblock(int id, bool nb);
bool    inotify_nonblock(int id);
void    inotify_release(int id);
bool    inotify_active(void);       /* any instance live? cheap emit gate */

#endif /* TOBYOS_INOTIFY_H */
