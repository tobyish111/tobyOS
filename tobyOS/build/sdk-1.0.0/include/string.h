/* string.h -- libtoby's mem... / str... surface.
 *
 * Implementations live in libtoby/src/string.c. They are deliberately
 * straightforward (byte-at-a-time, no SSE/MMX) since we build user
 * programs with -mno-sse / -mno-mmx anyway. Performance is not the
 * goal at the libc layer; correctness and zero kernel coupling is.
 *
 * Functions follow C11 semantics. UB cases (overlap on memcpy, NULL
 * source pointers, etc.) are not specially handled -- callers are
 * expected to honour the contract. */

#ifndef LIBTOBY_STRING_H
#define LIBTOBY_STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void   *memcpy (void *dst, const void *src, size_t n);
void   *memmove(void *dst, const void *src, size_t n);
void   *memset (void *dst, int c, size_t n);
int     memcmp (const void *a, const void *b, size_t n);
void   *memchr (const void *s, int c, size_t n);

size_t  strlen (const char *s);
size_t  strnlen(const char *s, size_t cap);
int     strcmp (const char *a, const char *b);
int     strncmp(const char *a, const char *b, size_t n);
char   *strcpy (char *dst, const char *src);
char   *strncpy(char *dst, const char *src, size_t n);
char   *strcat (char *dst, const char *src);
char   *strncat(char *dst, const char *src, size_t n);
char   *strchr (const char *s, int c);
char   *strrchr(const char *s, int c);
char   *strstr (const char *hay, const char *needle);

/* Allocates via malloc; returns NULL on OOM. */
char   *strdup (const char *s);
char   *strndup(const char *s, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_STRING_H */
