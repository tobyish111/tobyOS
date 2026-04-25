/* libtoby/src/errno.c -- the per-process errno global.
 *
 * Single int, zero-initialised. tobyOS does not yet have user threads,
 * so no TLS gymnastics. The wrapper layer in syscall.c is the only
 * intentional writer; user code may also assign to it freely. */

#include <errno.h>

int errno = 0;
