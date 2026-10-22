/* su_cat.c -- cat, as GNU coreutils 9.7's (logs/utilgate/staging/cat.sh):
 * each FILE (stdin for none, or for "-") to standard output.
 *
 * Measured on GNU's (gnucat in WSL, strace for the order of things):
 *   - -n numbers every output line ("%6d\t"), -b only the non-empty ones
 *     (and wins over -n), -s squeezes a run of empty lines to one, -E puts
 *     '$' at each line's end -- and shows a CR just before a newline as
 *     "^M$" --, -T shows a tab as "^I", -v shows the other control bytes
 *     as ^X, DEL as ^?, a byte over 127 as M- and the same notation of it
 *     less 128; -A is -vET, -e -vE, -t -vT, -u is ignored. The state runs
 *     on from one file to the next: line numbers, a squeezed run, a line
 *     the last file left open, a CR waiting to see if a newline follows.
 *   - Standard output is looked at first (fstat): when it cannot be, that
 *     is "cat: standard output: ERR", status 1, before anything is read.
 *   - "cat: NAME: ERR" for a file that does not open, cannot be read (a
 *     directory reads as EISDIR), and the next FILE is tried; status 1.
 *   - "cat: NAME: input file is output file" when FILE is the regular file
 *     standard output writes to and copying it would run into what is
 *     being written: there is input left (its offset is short of its
 *     size) and output goes after it (O_APPEND, or an offset past the
 *     input's). `cat f >> f` is refused; `cat f 1<>f` is not.
 *   - A write that fails is "cat: write error: ERR", status 1, at once:
 *     nothing more is read. Output is written as each read is processed.
 *   - stdin, once read, is closed at the end: a failure is "cat: closing
 *     standard input: ERR".
 *
 * tobyOS: libtoby has no fcntl, so whether standard output appends comes
 * from /proc/self/fdinfo/1's "flags:" (the kernel keeps O_APPEND there);
 * a directory is opened as Linux opens one (su_open_read). The old cat
 * (programs/user_cat) had -n alone. */

#include "su_fileops.c"

#define CT_BUFSZ 65536

static struct {
    int number, nonblank, squeeze, ends, tabs, nonprint;
    int at_line_start, empties, pending_cr;
    uintmax_t line;
    char *out;
    size_t on, ocap;
    struct stat ost;
    int out_reg, out_append;
} ct;

static void ct_flush(void) {
    if (ct.on && fo_write_all(1, ct.out, ct.on) != 0) su_error(1, errno, "write error");
    ct.on = 0;
}

static void ct_put(const char *s, size_t n) {
    if (ct.on + n > ct.ocap) ct_flush();
    if (n > ct.ocap) {
        if (fo_write_all(1, s, n) != 0) su_error(1, errno, "write error");
        return;
    }
    memcpy(ct.out + ct.on, s, n);
    ct.on += n;
}

static void ct_putc(char c) {
    if (ct.on == ct.ocap) ct_flush();
    ct.out[ct.on++] = c;
}

static void ct_number(void) {
    char b[48];
    int n = snprintf(b, sizeof b, "%6ju\t", ++ct.line);
    ct_put(b, (size_t)n);
}

/* one input byte, as the options show it */
static void ct_byte(unsigned char c) {
    if (c == '\n') {
        if (ct.at_line_start) {
            if (++ct.empties > 1 && ct.squeeze) return;
            if (ct.number && !ct.nonblank) ct_number();
        }
        if (ct.pending_cr) {
            ct_put("^M", 2);
            ct.pending_cr = 0;
        }
        if (ct.ends) ct_putc('$');
        ct_putc('\n');
        ct.at_line_start = 1;
        return;
    }
    if (ct.at_line_start) {
        if (ct.number) ct_number();
        ct.at_line_start = 0;
        ct.empties = 0;
    }
    if (ct.pending_cr) {
        ct_putc('\r');
        ct.pending_cr = 0;
    }
    if (c == '\t') {
        if (ct.tabs) ct_put("^I", 2);
        else ct_putc('\t');
        return;
    }
    if (c == '\r' && ct.ends && !ct.nonprint) {
        ct.pending_cr = 1;
        return;
    }
    if (!ct.nonprint) { ct_putc((char)c); return; }
    if (c >= 128) {
        ct_put("M-", 2);
        c -= 128;
    }
    if (c < 32) {
        ct_putc('^');
        ct_putc((char)(c + 64));
    } else if (c == 127) {
        ct_put("^?", 2);
    } else {
        ct_putc((char)c);
    }
}

/* whether standard output appends, as fcntl(F_GETFL) would say */
static int ct_out_appends(void) {
#ifdef SU_HOST
    int fl = fcntl(1, F_GETFL);
    return fl >= 0 && (fl & O_APPEND);
#else
    char buf[512];
    if (slurp("/proc/self/fdinfo/1", buf, sizeof buf) < 0) return 0;
    const char *f = strstr(buf, "flags:");
    if (!f) return 0;
    return (strtoul(f + 6, 0, 8) & 02000) != 0;
#endif
}

/* 0, or 1 after saying why FILE was not copied */
static int ct_file(const char *name, int fd, int formatted, char *buf) {
    struct stat st;
    if (fstat(fd, &st) != 0) {
        su_error(0, errno, "%s", su_quotef(name));
        return 1;
    }
    if (ct.out_reg && S_ISREG(st.st_mode) &&
        st.st_dev == ct.ost.st_dev && st.st_ino == ct.ost.st_ino) {
        off_t in_pos = lseek(fd, 0, SEEK_CUR);
        off_t out_pos = ct.out_append ? st.st_size : lseek(1, 0, SEEK_CUR);
        if (in_pos >= 0 && in_pos < st.st_size && out_pos > in_pos) {
            su_error(0, 0, "%s: input file is output file", su_quotef(name));
            return 1;
        }
    }
    for (;;) {
        ssize_t n = read(fd, buf, CT_BUFSZ);
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            if (formatted) ct_flush();
            su_error(0, e, "%s", su_quotef(name));
            return 1;
        }
        if (n == 0) break;
        if (!formatted) {
            if (fo_write_all(1, buf, (size_t)n) != 0) su_error(1, errno, "write error");
            continue;
        }
        for (ssize_t i = 0; i < n; i++) ct_byte((unsigned char)buf[i]);
        ct_flush();
    }
    return 0;
}

static int do_cat(int argc, char **argv) {
    fo_stdout_guard();
    /* GNU's order: the ambiguity messages list them so */
    static const struct su_option longs[] = {
        { "number-nonblank", SU_NO_ARG, 0, 'b' },
        { "number", SU_NO_ARG, 0, 'n' },
        { "squeeze-blank", SU_NO_ARG, 0, 's' },
        { "show-nonprinting", SU_NO_ARG, 0, 'v' },
        { "show-ends", SU_NO_ARG, 0, 'E' },
        { "show-tabs", SU_NO_ARG, 0, 'T' },
        { "show-all", SU_NO_ARG, 0, 'A' },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    int c;
    while ((c = su_getopt_long(argc, argv, "benstuvAET", longs, 0)) != -1) {
        switch (c) {
        case 'b': ct.number = ct.nonblank = 1; break;
        case 'e': ct.ends = ct.nonprint = 1; break;
        case 'n': ct.number = 1; break;
        case 's': ct.squeeze = 1; break;
        case 't': ct.tabs = ct.nonprint = 1; break;
        case 'u': break;
        case 'v': ct.nonprint = 1; break;
        case 'A': ct.nonprint = ct.ends = ct.tabs = 1; break;
        case 'E': ct.ends = 1; break;
        case 'T': ct.tabs = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [FILE]...\n"
                   "Write each FILE, one after another, to standard output.\n"
                   "\n"
                   "Standard input is read when there is no FILE, and for a FILE of -.\n"
                   "\n"
                   "  -A, --show-all           the same as -vET\n"
                   "  -b, --number-nonblank    number the lines that are not empty (this\n"
                   "                             takes the place of -n)\n"
                   "  -e                       the same as -vE\n"
                   "  -E, --show-ends          end each line with a $\n"
                   "  -n, --number             number every line\n"
                   "  -s, --squeeze-blank      print a run of empty lines as one\n"
                   "  -t                       the same as -vT\n"
                   "  -T, --show-tabs          show a tab as ^I\n"
                   "  -u                       accepted, and changes nothing\n"
                   "  -v, --show-nonprinting   show control bytes as ^X and bytes above 127\n"
                   "                             as M-X (newline and tab as they are)\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "For example, `cat a - b' writes a, then standard input, then b.\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (fstat(1, &ct.ost) != 0) su_error(1, errno, "standard output");
    ct.out_reg = S_ISREG(ct.ost.st_mode);
    ct.out_append = ct.out_reg && ct_out_appends();
    ct.at_line_start = 1;
    int formatted = ct.number || ct.squeeze || ct.ends || ct.tabs || ct.nonprint;
    char *buf = (char *)malloc(CT_BUFSZ);
    ct.ocap = CT_BUFSZ * 4;
    ct.out = (char *)malloc(ct.ocap);
    if (!buf || !ct.out) su_error(1, ENOMEM, "memory exhausted");

    static char dash[] = "-";
    char *none[] = { dash };
    char **files = su_optind < argc ? argv + su_optind : none;
    int nfiles = su_optind < argc ? argc - su_optind : 1;
    int rc = 0, read_stdin = 0;
    for (int i = 0; i < nfiles; i++) {
        const char *name = files[i];
        if (!strcmp(name, "-")) {
            read_stdin = 1;
            rc |= ct_file(name, 0, formatted, buf);
            continue;
        }
        int fd = su_open_read(name, 0);
        if (fd < 0) {
            su_error(0, errno, "%s", su_quotef(name));
            rc = 1;
            continue;
        }
        rc |= ct_file(name, fd, formatted, buf);
        close(fd);
    }
    if (ct.pending_cr) {
        ct_putc('\r');
        ct.pending_cr = 0;
    }
    ct_flush();
    if (read_stdin && close(0) != 0) su_error(1, errno, "closing standard input");
    return rc;
}
