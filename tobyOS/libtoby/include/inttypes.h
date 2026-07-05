/* libtoby/include/inttypes.h -- printf/scanf format macros for the
 * fixed-width types (C99 subset; LP64: int64_t is long). Added for the
 * QuickJS port (M-JS); anything printf-family in libtoby understands
 * the standard l/ll/z length modifiers. */

#ifndef _INTTYPES_H
#define _INTTYPES_H

#include <stdint.h>

#define PRId8   "d"
#define PRId16  "d"
#define PRId32  "d"
#define PRId64  "ld"
#define PRIi64  "ld"
#define PRIu8   "u"
#define PRIu16  "u"
#define PRIu32  "u"
#define PRIu64  "lu"
#define PRIx8   "x"
#define PRIx16  "x"
#define PRIx32  "x"
#define PRIx64  "lx"
#define PRIX64  "lX"
#define PRIo64  "lo"
#define PRIdPTR "ld"
#define PRIuPTR "lu"
#define PRIxPTR "lx"

#define SCNd32  "d"
#define SCNd64  "ld"
#define SCNu32  "u"
#define SCNu64  "lu"
#define SCNx32  "x"
#define SCNx64  "lx"

#endif /* _INTTYPES_H */
