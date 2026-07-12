/* stdlib.h -- libtoby's C stdlib surface.
 *
 * Includes the heap allocator (malloc family), process exit, simple
 * conversions (atoi etc.), and getenv. The allocator is a first-fit
 * free-list implementation backed by sbrk(); see libtoby/src/stdlib.c
 * for the layout. */

#ifndef LIBTOBY_STDLIB_H
#define LIBTOBY_STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EXIT_SUCCESS  0
#define EXIT_FAILURE  1
#define RAND_MAX      0x7fffffff

void  *malloc (size_t n);
void  *calloc (size_t nmemb, size_t sz);
void  *realloc(void *p, size_t n);
void   free   (void *p);
/* POSIX aligned allocation. libtoby's malloc already returns 16-byte
 * aligned memory; larger alignments are only needed for SIMD (which the
 * codecs disable), so this is malloc-backed and the result frees with
 * free(). */
int    posix_memalign(void **memptr, size_t alignment, size_t size);

void   exit   (int code) __attribute__((noreturn));
void   abort  (void)     __attribute__((noreturn));
int    atexit (void (*f)(void));

int    atoi   (const char *s);
long   atol   (const char *s);
long   strtol (const char *s, char **endp, int base);
unsigned long strtoul(const char *s, char **endp, int base);

char  *getenv  (const char *name);
int    setenv  (const char *name, const char *value, int overwrite);
int    unsetenv(const char *name);
int    putenv  (char *string);
int    clearenv(void);

/* Synchronously runs `cmd` as if `tobysh -c <cmd>` (until a real shell
 * exists). Returns the child's exit status (0..255) on success or -1
 * on spawn/wait failure with errno set. (Milestone 25C) */
int    system  (const char *cmd);

/* Simple xorshift PRNG; deterministic per-process unless srand is called. */
int    rand  (void);
void   srand (unsigned seed);

/* Standard qsort and bsearch. */
void   qsort (void *base, size_t nmemb, size_t size,
              int (*compar)(const void *, const void *));
void  *bsearch(const void *key, const void *base, size_t nmemb,
               size_t size, int (*compar)(const void *, const void *));

/* String to floating-point. */
double strtod(const char *s, char **endp);
double atof  (const char *s);

/* Temporary files. */
int    mkstemp(char *tmpl);

/* Path resolution. If resolved is NULL, malloc's the result. */
char  *realpath(const char *path, char *resolved);

/* Absolute value. */
int    abs (int n);
long   labs(long n);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_STDLIB_H */
