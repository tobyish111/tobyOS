/* libtoby/src/getopt.c -- POSIX-style getopt().
 *
 * Algorithm matches XSH 4.4.4. We do NOT permute argv (no GNU
 * extension), so once a non-option element is seen, getopt stops.
 *
 * State is process-global (POSIX requires it that way): optind,
 * optarg, optopt, opterr, plus a private cursor that walks the
 * current argv element across consecutive bundled flags ("-abc").
 *
 * The implementation is tiny on purpose: every sbase port we care
 * about uses the same option grammar, and we do not need anything
 * fancier.
 */

#include <unistd.h>
#include <stdio.h>
#include <string.h>

char *optarg = 0;
int   optind = 1;
int   optopt = 0;
int   opterr = 1;

static int  scan_pos = 1;          /* next char inside argv[optind] */
static int  scan_argi = 0;         /* which argv[] scan_pos refers to */

static void diag(const char *prog, const char *msg, int c) {
    if (!opterr) return;
    fprintf(stderr, "%s: %s -- %c\n", prog ? prog : "?", msg, c);
}

int getopt(int argc, char *const argv[], const char *optstring) {
    if (optind >= argc) return -1;

    const char *cur = argv[optind];

    /* Reset internal cursor when optind advances. */
    if (scan_argi != optind) {
        scan_argi = optind;
        scan_pos  = 1;
    }

    if (cur == 0 || cur[0] != '-' || cur[1] == '\0') return -1;
    if (cur[0] == '-' && cur[1] == '-' && cur[2] == '\0') {
        optind++;
        return -1;
    }

    int c = (unsigned char)cur[scan_pos];
    if (c == '\0') {
        optind++;
        scan_pos = 1;
        scan_argi = optind;
        return getopt(argc, argv, optstring);
    }

    int colon_lead = (optstring[0] == ':');
    const char *match = strchr(optstring + (colon_lead ? 1 : 0), c);
    if (!match || c == ':') {
        optopt = c;
        diag(argv[0], "unknown option", c);
        scan_pos++;
        if (cur[scan_pos] == '\0') {
            optind++;
            scan_pos = 1;
            scan_argi = optind;
        }
        return '?';
    }

    if (match[1] == ':') {
        /* Option requires an argument. */
        optopt = c;
        if (cur[scan_pos + 1] != '\0') {
            optarg = (char *)&cur[scan_pos + 1];
            optind++;
            scan_pos = 1;
            scan_argi = optind;
            return c;
        }
        if (optind + 1 < argc) {
            optarg = argv[optind + 1];
            optind += 2;
            scan_pos = 1;
            scan_argi = optind;
            return c;
        }
        /* Missing argument. */
        optind++;
        scan_pos = 1;
        scan_argi = optind;
        diag(argv[0], "option requires an argument", c);
        return colon_lead ? ':' : '?';
    }

    /* Bare flag; advance within the bundle. */
    scan_pos++;
    if (cur[scan_pos] == '\0') {
        optind++;
        scan_pos = 1;
        scan_argi = optind;
    }
    return c;
}
