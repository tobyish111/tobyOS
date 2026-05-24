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

#endif /* TOBYOS_INOTIFY_H */
