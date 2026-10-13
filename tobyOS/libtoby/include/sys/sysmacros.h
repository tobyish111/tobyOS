/* sys/sysmacros.h -- a device number's major and minor, as glibc's: the
 * Linux encoding (12 bits of major over 20 of minor, split as the kernel
 * splits them), which is what st_rdev and st_dev carry. */
#ifndef LIBTOBY_SYS_SYSMACROS_H
#define LIBTOBY_SYS_SYSMACROS_H

#include <sys/types.h>

#define major(d)  ((unsigned int)((((d) >> 32) & 0xfffff000u) | (((d) >> 8) & 0xfffu)))
#define minor(d)  ((unsigned int)((((d) >> 12) & 0xffffff00u) | ((d) & 0xffu)))
#define makedev(ma, mi) ((dev_t)(((unsigned long)((ma) & 0xfffff000u) << 32) | \
                                 ((unsigned long)((ma) & 0xfffu) << 8) | \
                                 ((unsigned long)((mi) & 0xffffff00u) << 12) | \
                                 ((unsigned long)((mi) & 0xffu))))

#endif /* LIBTOBY_SYS_SYSMACROS_H */
