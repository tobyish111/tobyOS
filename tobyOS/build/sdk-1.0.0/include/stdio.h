/* stdio.h -- libtoby's <stdio.h>.
 *
 * Minimal POSIX-shape stream I/O. Each FILE* is unbuffered (writes go
 * straight to a write() syscall, reads pull from a 1-char pushback
 * slot or trap into read()). This keeps the implementation tiny --
 * full stdio buffering can come back as a 25E optimization once the
 * port targets need it. printf-family supports:
 *
 *   conversions: %c %s %d %i %u %x %X %o %p %ld %li %lu %lx %zu %%
 *   flags      : - + ' ' # 0
 *   width      : digits or *
 *   precision  : .digits or .*  (for %s = max chars; for %d = min digits)
 *
 * No floating-point conversions yet -- our sample programs and the
 * M25E ports we plan to add (cat, ls, echo, wc, sh) don't use them. */

#ifndef LIBTOBY_STDIO_H
#define LIBTOBY_STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EOF             (-1)
#define BUFSIZ          4096
#define FILENAME_MAX    256
#define FOPEN_MAX       16

typedef struct __libtoby_FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* ---- formatted output (no FILE *) -------------------------------- */
int    printf  (const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int    vprintf (const char *fmt, va_list ap);
int    sprintf (char *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int    snprintf(char *out, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int    vsnprintf(char *out, size_t cap, const char *fmt, va_list ap);
int    vsprintf (char *out, const char *fmt, va_list ap);

/* ---- formatted output (FILE *) ----------------------------------- */
int    fprintf (FILE *f, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int    vfprintf(FILE *f, const char *fmt, va_list ap);

/* ---- byte / line I/O --------------------------------------------- */
int    puts    (const char *s);
int    fputs   (const char *s, FILE *f);
int    putchar (int c);
int    putc    (int c, FILE *f);
int    fputc   (int c, FILE *f);
int    getchar (void);
int    getc    (FILE *f);
int    fgetc   (FILE *f);
char  *fgets   (char *s, int cap, FILE *f);

/* ---- file streams ------------------------------------------------- */
FILE  *fopen   (const char *path, const char *mode);
int    fclose  (FILE *f);
size_t fread   (void *buf, size_t sz, size_t n, FILE *f);
size_t fwrite  (const void *buf, size_t sz, size_t n, FILE *f);
int    fflush  (FILE *f);
int    fseek   (FILE *f, long off, int whence);
long   ftell   (FILE *f);
void   rewind  (FILE *f);
int    feof    (FILE *f);
int    ferror  (FILE *f);
void   clearerr(FILE *f);
int    fileno  (FILE *f);

/* ---- diagnostics -------------------------------------------------- */
void   perror  (const char *prefix);

#ifdef __cplusplus
}
#endif

#endif /* LIBTOBY_STDIO_H */
