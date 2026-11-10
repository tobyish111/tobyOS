/* cut -- select fields or characters from each line.
 *
 *     cut -b list [file ...]
 *     cut -c list [file ...]
 *     cut -f list [-d delim] [-s] [file ...]
 *
 * `list` is a comma-separated set of numbers and N-M ranges (either end
 * may be omitted). Lines with no delimiter are printed whole unless -s. */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

#define CUT_MAX 256

static int lo[CUT_MAX], hi[CUT_MAX], nrange;
static int mode;              /* 'c' characters/bytes, 'f' fields */
static char delim = '\t';
static int sflag;

static void parse_list(const char *s) {
    while (*s) {
        int a = 0, b = 0;
        int have_a = 0;
        while (*s >= '0' && *s <= '9') { a = a * 10 + (*s++ - '0'); have_a = 1; }
        if (*s == '-') {
            s++;
            int have_b = 0;
            while (*s >= '0' && *s <= '9') { b = b * 10 + (*s++ - '0'); have_b = 1; }
            if (!have_a) a = 1;
            if (!have_b) b = 1 << 30;
        } else {
            if (!have_a) eprintf("invalid list");
            b = a;
        }
        if (nrange == CUT_MAX) eprintf("list too long");
        lo[nrange] = a;
        hi[nrange] = b;
        nrange++;
        if (*s == ',') s++;
        else if (*s) eprintf("invalid list");
    }
    if (!nrange) eprintf("invalid list");
}

static int selected(int n) {
    for (int i = 0; i < nrange; i++)
        if (n >= lo[i] && n <= hi[i]) return 1;
    return 0;
}

static void cut_line(char *line) {
    if (mode == 'c') {
        for (int i = 0; line[i]; i++)
            if (selected(i + 1)) putchar(line[i]);
        putchar('\n');
        return;
    }
    if (!strchr(line, delim)) {
        if (!sflag) printf("%s\n", line);
        return;
    }
    int field = 1, first = 1;
    char *start = line;
    for (char *p = line; ; p++) {
        if (*p == delim || *p == '\0') {
            char save = *p;
            *p = '\0';
            if (selected(field)) {
                if (!first) putchar(delim);
                printf("%s", start);
                first = 0;
            }
            *p = save;
            if (!*p) break;
            start = p + 1;
            field++;
        }
    }
    putchar('\n');
}

static int cut_fd(int fd, const char *label) {
    char buf[4096];
    size_t len = 0;
    for (;;) {
        char chunk[1024];
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0) { weprintf("read %s:", label); return -1; }
        if (n == 0) break;
        for (ssize_t i = 0; i < n; i++) {
            if (chunk[i] == '\n') {
                buf[len] = '\0';
                cut_line(buf);
                len = 0;
            } else if (len + 1 < sizeof(buf)) {
                buf[len++] = chunk[i];
            }
        }
    }
    if (len) { buf[len] = '\0'; cut_line(buf); }
    return 0;
}

int main(int argc, char *argv[]) {
    util_argv0_set(argc > 0 ? argv[0] : "cut");
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        char opt = argv[i][1];
        const char *arg = argv[i] + 2;
        if (opt == 'b' || opt == 'c' || opt == 'f' || opt == 'd') {
            if (!*arg) {
                if (++i >= argc) eprintf("option -%c needs an argument", opt);
                arg = argv[i];
            }
            if (opt == 'd') delim = arg[0];
            else { mode = (opt == 'f') ? 'f' : 'c'; parse_list(arg); }
        } else if (opt == 's') {
            sflag = 1;
        } else {
            eprintf("usage: %s -b|-c|-f list [-d delim] [-s] [file ...]", argv0);
        }
    }
    if (!nrange) eprintf("usage: %s -b|-c|-f list [-d delim] [-s] [file ...]", argv0);

    int rc = 0;
    if (i >= argc) {
        if (cut_fd(0, "<stdin>") < 0) rc = 1;
    } else {
        for (; i < argc; i++) {
            int fd = strcmp(argv[i], "-") ? open(argv[i], O_RDONLY) : 0;
            if (fd < 0) { weprintf("open %s:", argv[i]); rc = 1; continue; }
            if (cut_fd(fd, argv[i]) < 0) rc = 1;
            if (fd) close(fd);
        }
    }
    return rc;
}
