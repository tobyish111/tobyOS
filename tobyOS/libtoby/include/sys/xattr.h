/* sys/xattr.h -- extended attributes, Linux's calls (finding 225).
 *
 * The kernel's (ABI_SYS_XATTR, the Linux personality's twelve calls): the
 * plain forms follow a final symbolic link, the l* forms take the link
 * itself, the f* forms an open descriptor. Names are "namespace.name" --
 * user.*, trusted.*, security.* -- up to 255 bytes; a value is up to
 * 64 KiB. A get or list with size 0 says the size it needs. There were
 * none: getfattr, setfattr and chcon had no way to reach an attribute. */
#ifndef LIBTOBY_SYS_XATTR_H
#define LIBTOBY_SYS_XATTR_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XATTR_CREATE   1          /* fail if it exists */
#define XATTR_REPLACE  2          /* fail if it does not */

int     setxattr(const char *path, const char *name, const void *value,
                 size_t size, int flags);
int     lsetxattr(const char *path, const char *name, const void *value,
                  size_t size, int flags);
int     fsetxattr(int fd, const char *name, const void *value,
                  size_t size, int flags);
ssize_t getxattr(const char *path, const char *name, void *value, size_t size);
ssize_t lgetxattr(const char *path, const char *name, void *value, size_t size);
ssize_t fgetxattr(int fd, const char *name, void *value, size_t size);
ssize_t listxattr(const char *path, char *list, size_t size);
ssize_t llistxattr(const char *path, char *list, size_t size);
ssize_t flistxattr(int fd, char *list, size_t size);
int     removexattr(const char *path, const char *name);
int     lremovexattr(const char *path, const char *name);
int     fremovexattr(int fd, const char *name);

#ifdef __cplusplus
}
#endif

#endif
