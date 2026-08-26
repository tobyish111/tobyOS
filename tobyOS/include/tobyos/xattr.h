#ifndef TOBYOS_XATTR_H
#define TOBYOS_XATTR_H

#include <stddef.h>
#include <stdint.h>

/* Extended attributes (Phase E, 2026-08-22). A kernel-resident store keyed
 * by CANONICAL VFS PATH -- the same string space resolve_user_path() and
 * the vfs_unlink/vfs_rename hooks operate in. See src/xattr.c for the
 * honesty contract (what persists, what doesn't).
 *
 * All functions return ABI-negative errnos (not VFS codes) because their
 * only caller is the Linux syscall layer. */

int  xattr_set(const char *kpath, const char *name,
               const void *val, size_t sz, int flags);
long xattr_get(const char *kpath, const char *name, void *val, size_t sz);
long xattr_list(const char *kpath, char *buf, size_t sz);
int  xattr_remove(const char *kpath, const char *name);

/* VFS lifecycle hooks (called from vfs_unlink / vfs_rename). */
void xattr_forget_path(const char *kpath);
void xattr_rename_path(const char *oldp, const char *newp);

#endif
