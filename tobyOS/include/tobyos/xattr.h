#ifndef TOBYOS_XATTR_H
#define TOBYOS_XATTR_H

#include <stddef.h>
#include <stdint.h>

/* Extended attributes (Phase E, 2026-08-22; Linux's rules, finding 225).
 *
 * Two homes. A filesystem that keeps attributes with its nodes, as Linux's
 * tmpfs does (vfs_ops->xattr), holds a struct xa_list per node: hard links
 * share it, a rename keeps it, and it dies with the node. Any other gets
 * the store here, keyed by CANONICAL VFS PATH, with the vfs_unlink and
 * vfs_rename hooks keeping it in step (src/xattr.c has its contract).
 *
 * Both answer one operation at a time and know nothing of permissions or
 * namespaces: those are the syscall layer's (src/syscall.c, xattr_policy),
 * as Linux's VFS asks them before the filesystem's handler. Every function
 * returns 0 or a length, or an ABI-negative errno. */

#define XATTR_NAME_MAX 255        /* Linux's: a longer name is ERANGE */
#define XATTR_SIZE_MAX 65536      /* Linux's: a longer value is E2BIG */
#define XATTR_LIST_MAX 65536      /* a list longer than this is E2BIG */

#define XA_CREATE  1              /* setxattr's flags, Linux's values */
#define XA_REPLACE 2

enum xa_op { XA_GET, XA_SET, XA_LIST, XA_REMOVE };

/* One node's attributes, kept in Linux's tmpfs order (names descending, as
 * its listxattr gives them). Opaque outside src/xattr.c. */
struct xa_list;

/* OP on *HEAD: GET NAME into BUF (SZ 0 asks the size), SET NAME to BUF/SZ
 * with FLAGS, LIST every name NUL-ended into BUF (SZ 0 asks the size),
 * REMOVE NAME. */
long xa_list_op(struct xa_list **head, int op, const char *name,
                void *buf, size_t sz, int flags);
void xa_list_free(struct xa_list **head);

/* The store for a filesystem with no ->xattr: OP on KPATH, as xa_list_op. */
long xattr_store(const char *kpath, int op, const char *name,
                 void *buf, size_t sz, int flags);

/* VFS lifecycle hooks for the store (called from vfs_unlink / vfs_rename). */
void xattr_forget_path(const char *kpath);
void xattr_rename_path(const char *oldp, const char *newp);

#endif
