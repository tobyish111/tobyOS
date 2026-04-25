/* wc -- count words, lines, characters in files.
 *
 * Modeled on sbase's wc.c (ISC license). Subset:
 *
 *     wc [-clw] [file ...]
 *
 *   -l   print line count
 *   -w   print word count
 *   -c   print byte count
 *
 * If no flags are given, all three are printed (POSIX default).
 * With multiple files, a totals line is printed at the end. */

#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static int show_l, show_w, show_c;

struct counts {
    long lines;
    long words;
    long chars;
};

static int count_fd(int fd, struct counts *c) {
    char  buf[4096];
    int   in_word = 0;
    long  lines = 0, words = 0, chars = 0;

    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) return -1;
        chars += n;
        for (ssize_t i = 0; i < n; i++) {
            unsigned char ch = (unsigned char)buf[i];
            if (ch == '\n') lines++;
            if (isspace(ch)) {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }
    c->lines = lines;
    c->words = words;
    c->chars = chars;
    return 0;
}

static void print_counts(const struct counts *c, const char *label) {
    if (show_l) printf("%8ld", c->lines);
    if (show_w) printf("%8ld", c->words);
    if (show_c) printf("%8ld", c->chars);
    if (label)  printf(" %s", label);
    putchar('\n');
}

static void usage(void) { eprintf("usage: %s [-clw] [file ...]", argv0); }

int main(int argc, char *argv[]) {
    util_argv0_set(argv[0]);

    int opt;
    while ((opt = getopt(argc, argv, "lwc")) != -1) {
        switch (opt) {
        case 'l': show_l = 1; break;
        case 'w': show_w = 1; break;
        case 'c': show_c = 1; break;
        default:  usage();
        }
    }
    if (!show_l && !show_w && !show_c) {
        show_l = show_w = show_c = 1;
    }

    struct counts total = {0, 0, 0};
    int rc = 0;
    int nfiles = argc - optind;

    if (nfiles <= 0) {
        struct counts c = {0, 0, 0};
        if (count_fd(0, &c) < 0) { weprintf("read <stdin>:"); rc = 1; }
        print_counts(&c, 0);
    } else {
        for (int i = optind; i < argc; i++) {
            int fd = open(argv[i], O_RDONLY);
            if (fd < 0) { weprintf("open %s:", argv[i]); rc = 1; continue; }
            struct counts c = {0, 0, 0};
            if (count_fd(fd, &c) < 0) {
                weprintf("read %s:", argv[i]);
                rc = 1;
            }
            close(fd);
            print_counts(&c, argv[i]);
            total.lines += c.lines;
            total.words += c.words;
            total.chars += c.chars;
        }
        if (nfiles > 1) print_counts(&total, "total");
    }
    return rc;
}
