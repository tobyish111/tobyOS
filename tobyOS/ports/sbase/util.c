/* ports/sbase/util.c -- shared helpers for sbase-style ports.
 *
 * Modeled on suckless sbase's libutil. Ported to libtoby:
 *   - eprintf/weprintf/enprintf use vfprintf(stderr, ...).
 *   - estrdup composes strdup and "out of memory" eprintf.
 *   - concat_fd uses a fixed BUFSIZ buffer; libtoby's read() and
 *     write() return -1 on failure with errno set (see legacy IO
 *     handling in libtoby/src/unistd.c).
 *
 * argv0 is exposed as a const char * for read-only access; tools
 * call util_argv0_set() once from main() (mirrors sbase's
 * ARGBEGIN macro behaviour). */

#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>

const char *argv0 = "?";

void util_argv0_set(const char *p) {
    if (p && p[0]) argv0 = p;
}

static void emit(const char *fmt, va_list ap) {
    fprintf(stderr, "%s: ", argv0);
    vfprintf(stderr, fmt, ap);
    size_t n = strlen(fmt);
    if (n == 0 || fmt[n - 1] != '\n') {
        if (n && fmt[n - 1] == ':') {
            fprintf(stderr, " %s\n", strerror(errno));
        } else {
            fputc('\n', stderr);
        }
    }
}

void weprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit(fmt, ap);
    va_end(ap);
}

void eprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit(fmt, ap);
    va_end(ap);
    exit(1);
}

void enprintf(int code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit(fmt, ap);
    va_end(ap);
    exit(code);
}

void *emalloc(size_t n) {
    void *p = malloc(n);
    if (!p) eprintf("out of memory");
    return p;
}

void *erealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) eprintf("out of memory");
    return q;
}

char *estrdup(const char *s) {
    char *q = strdup(s);
    if (!q) eprintf("out of memory");
    return q;
}

int concat_fd(int fd, const char *label) {
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) return 0;
        if (n < 0) {
            weprintf("read %s:", label);
            return -1;
        }
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(1, buf + written, (size_t)(n - written));
            if (w <= 0) {
                weprintf("write:");
                return -1;
            }
            written += w;
        }
    }
}
