/* sys/types.h -- libtoby's POSIX type aliases.
 *
 * Just the pieces real C programs commonly poke at. Everything is
 * defined in terms of the freestanding integer types so we don't pull
 * a heavy dependency chain (no <bits/types> nonsense). */

#ifndef LIBTOBY_SYS_TYPES_H
#define LIBTOBY_SYS_TYPES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef long           ssize_t;
typedef long           off_t;
typedef int            pid_t;
typedef unsigned int   mode_t;
typedef unsigned int   uid_t;
typedef unsigned int   gid_t;
typedef unsigned long  ino_t;
typedef long           time_t;
typedef long           clock_t;
typedef long           suseconds_t;

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_SYS_TYPES_H */
