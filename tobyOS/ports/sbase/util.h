/* ports/sbase/util.h -- shared helpers for sbase-style ports.
 *
 * Modelled on suckless sbase's util.h (ISC license, upstream
 * https://git.suckless.org/sbase). The set is trimmed to what the
 * tools we actually port need; we deliberately do NOT bring in
 * util/strlcpy, util/regex, util/recurse, etc. because we do not
 * have ports that use them yet.
 *
 * Calling convention follows sbase verbatim:
 *
 *   - argv0 is set by the ARGBEGIN-style loop in each tool's main()
 *     (or, in our case, by util_argv0_set()) so eprintf can prefix
 *     diagnostics with the program name.
 *   - eprintf(fmt, ...) writes to stderr and exits 1.
 *   - weprintf(fmt, ...) writes to stderr and returns.
 *   - estrdup / emalloc / erealloc are checked allocators that
 *     eprintf("out of memory") on failure rather than returning NULL.
 *
 * If you add a tool here, prefer to model it on the upstream sbase
 * version line-for-line, then adapt only what libtoby is missing. */

#ifndef PORTS_SBASE_UTIL_H
#define PORTS_SBASE_UTIL_H

#include <stddef.h>

extern const char *argv0;

void  util_argv0_set(const char *p);

void  eprintf (const char *fmt, ...) __attribute__((noreturn,
                                                    format(printf, 1, 2)));
void  weprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void  enprintf(int code, const char *fmt, ...)
      __attribute__((noreturn, format(printf, 2, 3)));

void *emalloc (size_t n);
void *erealloc(void *p, size_t n);
char *estrdup (const char *s);

/* Read bytes from an open fd, write to stdout. Used by cat, head,
 * etc. so the read-loop logic isn't copy-pasted into every tool.
 * Returns 0 on success, -1 on read or write error (errno set). */
int   concat_fd(int fd, const char *label);

#endif
