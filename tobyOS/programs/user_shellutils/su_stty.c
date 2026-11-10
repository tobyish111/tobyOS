/* su_stty.c -- stty, as GNU coreutils 9.7's (logs/utilgate/stty.sh with no
 * terminal; logs/utilgate/sttypty.sh on WSL's ptys, the Linux build).
 *
 *   stty [-F DEVICE | --file=DEVICE] [SETTING]...
 *   stty [-F DEVICE | --file=DEVICE] [-a|--all]
 *   stty [-F DEVICE | --file=DEVICE] [-g|--save]
 *
 *   - Every setting GNU's has on Linux: the control, input, output and
 *     local flags (-name to clear one), the delay classes (nl1 cr2 tab3
 *     ...), the combinations (raw cooked cbreak sane evenp oddp parity
 *     pass8 litout nl ek tabs decctlq lcase crt dec), the special
 *     characters (intr ^C, erase ^?, quit undef, kill 0x15, ...), min and
 *     time, the speeds (9600, ispeed/ospeed N, exta extb, 134.5), rows,
 *     cols/columns, size, speed, line, drain, and stty -g's own string.
 *   - Its listings: the settings unlike "sane" (no argument), all of them
 *     (-a) wrapped to the screen's width as GNU wraps them, or the
 *     restorable string (-g: the four flag words and 32 characters, hex).
 *   - GNU's order: names, missing arguments, speeds and min/time/line are
 *     checked before the terminal is asked anything; a refused change is
 *     the kernel's error, a partial one "unable to perform all requested
 *     operations".
 *
 * Flag values are Linux's, here, because the tobyOS kernel's termios is
 * (include/tobyos/tty.h) and libtoby names only some of them. Speeds live
 * in c_cflag's CBAUD and CIBAUD bits as on Linux. It knew echo, icanon,
 * raw, cooked and sane, and printed "speed 38400 baud" whatever the
 * terminal said. */

#define ST_CMSPAR   0x40000000u
#define ST_CRTSCTS  0x80000000u
#define ST_CBAUD    0x0000100Fu
#define ST_CIBAUD   0x100F0000u
#define ST_IBSHIFT  16
#define ST_IMAXBEL  0x2000u
#define ST_IUTF8    0x4000u
#define ST_IUCLC    0x0200u
#define ST_OLCUC    0x0002u
#define ST_OFILL    0x0040u
#define ST_OFDEL    0x0080u
#define ST_NLDLY    0x0100u
#define ST_CRDLY    0x0600u
#define ST_TABDLY   0x1800u
#define ST_BSDLY    0x2000u
#define ST_VTDLY    0x4000u
#define ST_FFDLY    0x8000u
#define ST_XCASE    0x0004u
#define ST_ECHOCTL  0x0200u
#define ST_ECHOPRT  0x0400u
#define ST_ECHOKE   0x0800u
#define ST_FLUSHO   0x1000u
#define ST_EXTPROC  0x10000u
#define ST_NCC      32          /* glibc's NCCS: what -g prints and reads */
#define ST_KCC      17          /* the characters the kernel keeps (VEOL2+1) */

enum { ST_CONTROL, ST_INPUT, ST_OUTPUT, ST_LOCAL, ST_COMBO };
#define ST_SANE_SET   1
#define ST_SANE_UNSET 2
#define ST_REV        4
#define ST_OMIT       8

struct st_mode { const char *name; int type, flags; unsigned bits, mask; };

static const struct st_mode st_modes[] = {
    { "parenb", ST_CONTROL, ST_REV, 0x100, 0 },
    { "parodd", ST_CONTROL, ST_REV, 0x200, 0 },
    { "cmspar", ST_CONTROL, ST_REV, ST_CMSPAR, 0 },
    { "cs5", ST_CONTROL, 0, 0x00, 0x30 },
    { "cs6", ST_CONTROL, 0, 0x10, 0x30 },
    { "cs7", ST_CONTROL, 0, 0x20, 0x30 },
    { "cs8", ST_CONTROL, 0, 0x30, 0x30 },
    { "hupcl", ST_CONTROL, ST_REV, 0x400, 0 },
    { "hup", ST_CONTROL, ST_REV | ST_OMIT, 0x400, 0 },
    { "cstopb", ST_CONTROL, ST_REV, 0x40, 0 },
    { "cread", ST_CONTROL, ST_SANE_SET | ST_REV, 0x80, 0 },
    { "clocal", ST_CONTROL, ST_REV, 0x800, 0 },
    { "crtscts", ST_CONTROL, ST_REV, ST_CRTSCTS, 0 },
    { "ignbrk", ST_INPUT, ST_SANE_UNSET | ST_REV, 0x1, 0 },
    { "brkint", ST_INPUT, ST_SANE_SET | ST_REV, 0x2, 0 },
    { "ignpar", ST_INPUT, ST_REV, 0x4, 0 },
    { "parmrk", ST_INPUT, ST_REV, 0x8, 0 },
    { "inpck", ST_INPUT, ST_REV, 0x10, 0 },
    { "istrip", ST_INPUT, ST_REV, 0x20, 0 },
    { "inlcr", ST_INPUT, ST_SANE_UNSET | ST_REV, 0x40, 0 },
    { "igncr", ST_INPUT, ST_SANE_UNSET | ST_REV, 0x80, 0 },
    { "icrnl", ST_INPUT, ST_SANE_SET | ST_REV, 0x100, 0 },
    { "ixon", ST_INPUT, ST_REV, 0x400, 0 },
    { "ixoff", ST_INPUT, ST_SANE_UNSET | ST_REV, 0x1000, 0 },
    { "tandem", ST_INPUT, ST_REV | ST_OMIT, 0x1000, 0 },
    { "iuclc", ST_INPUT, ST_SANE_UNSET | ST_REV, ST_IUCLC, 0 },
    { "ixany", ST_INPUT, ST_SANE_UNSET | ST_REV, 0x800, 0 },
    { "imaxbel", ST_INPUT, ST_SANE_SET | ST_REV, ST_IMAXBEL, 0 },
    { "iutf8", ST_INPUT, ST_SANE_UNSET | ST_REV, ST_IUTF8, 0 },
    { "opost", ST_OUTPUT, ST_SANE_SET | ST_REV, 0x1, 0 },
    { "olcuc", ST_OUTPUT, ST_SANE_UNSET | ST_REV, ST_OLCUC, 0 },
    { "ocrnl", ST_OUTPUT, ST_SANE_UNSET | ST_REV, 0x8, 0 },
    { "onlcr", ST_OUTPUT, ST_SANE_SET | ST_REV, 0x4, 0 },
    { "onocr", ST_OUTPUT, ST_SANE_UNSET | ST_REV, 0x10, 0 },
    { "onlret", ST_OUTPUT, ST_SANE_UNSET | ST_REV, 0x20, 0 },
    { "ofill", ST_OUTPUT, ST_SANE_UNSET | ST_REV, ST_OFILL, 0 },
    { "ofdel", ST_OUTPUT, ST_SANE_UNSET | ST_REV, ST_OFDEL, 0 },
    { "nl1", ST_OUTPUT, ST_SANE_UNSET, 0x100, ST_NLDLY },
    { "nl0", ST_OUTPUT, ST_SANE_SET, 0, ST_NLDLY },
    { "cr3", ST_OUTPUT, ST_SANE_UNSET, 0x600, ST_CRDLY },
    { "cr2", ST_OUTPUT, ST_SANE_UNSET, 0x400, ST_CRDLY },
    { "cr1", ST_OUTPUT, ST_SANE_UNSET, 0x200, ST_CRDLY },
    { "cr0", ST_OUTPUT, ST_SANE_SET, 0, ST_CRDLY },
    { "tab3", ST_OUTPUT, ST_SANE_UNSET, 0x1800, ST_TABDLY },
    { "tab2", ST_OUTPUT, ST_SANE_UNSET, 0x1000, ST_TABDLY },
    { "tab1", ST_OUTPUT, ST_SANE_UNSET, 0x800, ST_TABDLY },
    { "tab0", ST_OUTPUT, ST_SANE_SET, 0, ST_TABDLY },
    { "bs1", ST_OUTPUT, ST_SANE_UNSET, 0x2000, ST_BSDLY },
    { "bs0", ST_OUTPUT, ST_SANE_SET, 0, ST_BSDLY },
    { "vt1", ST_OUTPUT, ST_SANE_UNSET, 0x4000, ST_VTDLY },
    { "vt0", ST_OUTPUT, ST_SANE_SET, 0, ST_VTDLY },
    { "ff1", ST_OUTPUT, ST_SANE_UNSET, 0x8000, ST_FFDLY },
    { "ff0", ST_OUTPUT, ST_SANE_SET, 0, ST_FFDLY },
    { "isig", ST_LOCAL, ST_SANE_SET | ST_REV, 0x1, 0 },
    { "icanon", ST_LOCAL, ST_SANE_SET | ST_REV, 0x2, 0 },
    { "iexten", ST_LOCAL, ST_SANE_SET | ST_REV, 0x8000, 0 },
    { "echo", ST_LOCAL, ST_SANE_SET | ST_REV, 0x8, 0 },
    { "echoe", ST_LOCAL, ST_SANE_SET | ST_REV, 0x10, 0 },
    { "crterase", ST_LOCAL, ST_REV | ST_OMIT, 0x10, 0 },
    { "echok", ST_LOCAL, ST_SANE_SET | ST_REV, 0x20, 0 },
    { "echonl", ST_LOCAL, ST_SANE_UNSET | ST_REV, 0x40, 0 },
    { "noflsh", ST_LOCAL, ST_SANE_UNSET | ST_REV, 0x80, 0 },
    { "xcase", ST_LOCAL, ST_SANE_UNSET | ST_REV, ST_XCASE, 0 },
    { "tostop", ST_LOCAL, ST_SANE_UNSET | ST_REV, 0x100, 0 },
    { "echoprt", ST_LOCAL, ST_SANE_UNSET | ST_REV, ST_ECHOPRT, 0 },
    { "prterase", ST_LOCAL, ST_REV | ST_OMIT, ST_ECHOPRT, 0 },
    { "echoctl", ST_LOCAL, ST_SANE_SET | ST_REV, ST_ECHOCTL, 0 },
    { "ctlecho", ST_LOCAL, ST_REV | ST_OMIT, ST_ECHOCTL, 0 },
    { "echoke", ST_LOCAL, ST_SANE_SET | ST_REV, ST_ECHOKE, 0 },
    { "crtkill", ST_LOCAL, ST_REV | ST_OMIT, ST_ECHOKE, 0 },
    { "flusho", ST_LOCAL, ST_SANE_UNSET | ST_REV, ST_FLUSHO, 0 },
    { "extproc", ST_LOCAL, ST_SANE_UNSET | ST_REV, ST_EXTPROC, 0 },
    { "evenp", ST_COMBO, ST_REV | ST_OMIT, 0, 0 },
    { "parity", ST_COMBO, ST_REV | ST_OMIT, 0, 0 },
    { "oddp", ST_COMBO, ST_REV | ST_OMIT, 0, 0 },
    { "nl", ST_COMBO, ST_REV | ST_OMIT, 0, 0 },
    { "ek", ST_COMBO, ST_OMIT, 0, 0 },
    { "sane", ST_COMBO, ST_OMIT, 0, 0 },
    { "cooked", ST_COMBO, ST_REV | ST_OMIT, 0, 0 },
    { "raw", ST_COMBO, ST_REV | ST_OMIT, 0, 0 },
    { "pass8", ST_COMBO, ST_REV | ST_OMIT, 0, 0 },
    { "litout", ST_COMBO, ST_REV | ST_OMIT, 0, 0 },
    { "cbreak", ST_COMBO, ST_REV | ST_OMIT, 0, 0 },
    { "decctlq", ST_COMBO, ST_REV | ST_OMIT, 0, 0 },
    { "tabs", ST_COMBO, ST_REV | ST_OMIT, 0, 0 },
    { "lcase", ST_COMBO, ST_REV | ST_OMIT, 0, 0 },
    { "LCASE", ST_COMBO, ST_REV | ST_OMIT, 0, 0 },
    { "crt", ST_COMBO, ST_OMIT, 0, 0 },
    { "dec", ST_COMBO, ST_OMIT, 0, 0 },
    { 0, 0, 0, 0, 0 }
};

struct st_char { const char *name; unsigned char sane; int index; };

/* in GNU's order; indices are Linux's c_cc slots */
static const struct st_char st_chars[] = {
    { "intr", 3, 0 }, { "quit", 28, 1 }, { "erase", 127, 2 }, { "kill", 21, 3 },
    { "eof", 4, 4 }, { "eol", 0, 11 }, { "eol2", 0, 16 }, { "swtch", 0, 7 },
    { "start", 17, 8 }, { "stop", 19, 9 }, { "susp", 26, 10 }, { "rprnt", 18, 12 },
    { "werase", 23, 14 }, { "lnext", 22, 15 }, { "discard", 15, 13 },
    { "min", 1, 6 }, { "time", 0, 5 }, { 0, 0, 0 }
};

static const struct { const char *name; unsigned code; unsigned long value; } st_speeds[] = {
    { "0", 0, 0 }, { "50", 1, 50 }, { "75", 2, 75 }, { "110", 3, 110 }, { "134", 4, 134 },
    { "134.5", 4, 134 }, { "150", 5, 150 }, { "200", 6, 200 }, { "300", 7, 300 },
    { "600", 8, 600 }, { "1200", 9, 1200 }, { "1800", 10, 1800 }, { "2400", 11, 2400 },
    { "4800", 12, 4800 }, { "9600", 13, 9600 }, { "19200", 14, 19200 }, { "38400", 15, 38400 },
    { "exta", 14, 19200 }, { "extb", 15, 38400 }, { "57600", 0x1001, 57600 },
    { "115200", 0x1002, 115200 }, { "230400", 0x1003, 230400 }, { "460800", 0x1004, 460800 },
    { "500000", 0x1005, 500000 }, { "576000", 0x1006, 576000 }, { "921600", 0x1007, 921600 },
    { "1000000", 0x1008, 1000000 }, { "1152000", 0x1009, 1152000 },
    { "1500000", 0x100a, 1500000 }, { "2000000", 0x100b, 2000000 },
    { "2500000", 0x100c, 2500000 }, { "3000000", 0x100d, 3000000 },
    { "3500000", 0x100e, 3500000 }, { "4000000", 0x100f, 4000000 }, { 0, 0, 0 }
};

/* the terminal's state as stty sees it: the four words, the line, and the
 * 32 characters -- the kernel keeps the first 17 */
struct st_state {
    struct termios t;
    unsigned char cc[ST_NCC];
};

static const char *st_dev = "standard input";
static int st_col, st_max_col = 80;

static int st_speed_code(const char *s) {
    for (int i = 0; st_speeds[i].name; i++)
        if (!strcmp(st_speeds[i].name, s)) return (int)st_speeds[i].code;
    return -1;
}

static unsigned long st_code_value(unsigned code) {
    for (int i = 0; st_speeds[i].name; i++)
        if (st_speeds[i].code == code) return st_speeds[i].value;
    return 0;
}

#ifdef SU_HOST
/* glibc keeps its own copy of the speeds (c_ispeed/c_ospeed) and may set
 * the terminal from them, so they are given too: the platform's B macro */
static speed_t st_bmacro(unsigned code) {
    static const struct { unsigned code; speed_t b; } m[] = {
        { 0, B0 }, { 1, B50 }, { 2, B75 }, { 3, B110 }, { 4, B134 }, { 5, B150 },
        { 6, B200 }, { 7, B300 }, { 8, B600 }, { 9, B1200 }, { 10, B1800 },
        { 11, B2400 }, { 12, B4800 }, { 13, B9600 }, { 14, B19200 }, { 15, B38400 },
        { 0x1001, B57600 }, { 0x1002, B115200 }, { 0x1003, B230400 },
        { 0x1004, B460800 }, { 0x1005, B500000 }, { 0x1006, B576000 },
        { 0x1007, B921600 }, { 0x1008, B1000000 }, { 0x1009, B1152000 },
        { 0x100a, B1500000 }, { 0x100b, B2000000 }, { 0x100c, B2500000 },
        { 0x100d, B3000000 }, { 0x100e, B3500000 }, { 0x100f, B4000000 },
    };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) if (m[i].code == code) return m[i].b;
    return B0;
}
#endif

static void st_set_ospeed(struct st_state *s, unsigned code) {
    s->t.c_cflag = (s->t.c_cflag & ~ST_CBAUD) | code;
#ifdef SU_HOST
    cfsetospeed(&s->t, st_bmacro(code));
    s->t.c_cflag = (s->t.c_cflag & ~ST_CBAUD) | code;
#endif
}

static void st_set_ispeed(struct st_state *s, unsigned code) {
    s->t.c_cflag = (s->t.c_cflag & ~ST_CIBAUD) | (code << ST_IBSHIFT);
#ifdef SU_HOST
    cfsetispeed(&s->t, st_bmacro(code));
    s->t.c_cflag = (s->t.c_cflag & ~ST_CIBAUD) | (code << ST_IBSHIFT);
#endif
}

static unsigned long st_ospeed(const struct st_state *s) { return st_code_value(s->t.c_cflag & ST_CBAUD); }
static unsigned long st_ispeed(const struct st_state *s) {
    return st_code_value((s->t.c_cflag & ST_CIBAUD) >> ST_IBSHIFT);
}

static tcflag_t *st_word(struct st_state *s, int type) {
    switch (type) {
    case ST_CONTROL: return &s->t.c_cflag;
    case ST_INPUT: return &s->t.c_iflag;
    case ST_OUTPUT: return &s->t.c_oflag;
    case ST_LOCAL: return &s->t.c_lflag;
    default: return 0;
    }
}

/* GNU's wrapf: a unit, after a space or on a new line at the screen's edge */
static void st_wrapf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (st_col > 0) {
        if (st_max_col - st_col <= n) { putchar('\n'); st_col = 0; }
        else { putchar(' '); st_col++; }
    }
    fputs(buf, stdout);
    st_col += n;
}

/* a character as GNU shows one: ^C, ^?, M-x, <undef> */
static const char *st_visible(unsigned char ch) {
    static char b[8];
    char *p = b;
    if (ch == 0) return "<undef>";
    if (ch >= 32) {
        if (ch < 127) *p++ = (char)ch;
        else if (ch == 127) { *p++ = '^'; *p++ = '?'; }
        else {
            *p++ = 'M';
            *p++ = '-';
            if (ch >= 128 + 32) {
                if (ch < 128 + 127) *p++ = (char)(ch - 128);
                else { *p++ = '^'; *p++ = '?'; }
            } else { *p++ = '^'; *p++ = (char)(ch - 128 + 64); }
        }
    } else { *p++ = '^'; *p++ = (char)(ch + 64); }
    *p = 0;
    return b;
}

static void st_show_speed(const struct st_state *s, int fancy) {
    unsigned long is = st_ispeed(s), os = st_ospeed(s);
    if (is == 0 || is == os) st_wrapf(fancy ? "speed %lu baud;" : "%lu\n", os);
    else st_wrapf(fancy ? "ispeed %lu baud; ospeed %lu baud;" : "%lu %lu\n", is, os);
    if (!fancy) st_col = 0;
}

struct st_winsize { unsigned short row, col, xp, yp; };

static int st_getwin(struct st_winsize *w) { return ioctl(0, 0x5413 /* TIOCGWINSZ */, w); }

static void st_show(struct st_state *s, int all) {
    st_show_speed(s, 1);
    if (all) {
        struct st_winsize w;
        if (st_getwin(&w) == 0) st_wrapf("rows %d; columns %d;", w.row, w.col);
    }
    st_wrapf("line = %d;", s->t.c_line);
    putchar('\n');
    st_col = 0;
    int empty = 1;
    for (int i = 0; strcmp(st_chars[i].name, "min"); i++) {
        unsigned char v = s->cc[st_chars[i].index];
        if (!all && v == st_chars[i].sane) continue;
        empty = 0;
        st_wrapf("%s = %s;", st_chars[i].name, st_visible(v));
    }
    if (all) {
        st_wrapf("min = %lu; time = %lu;", (unsigned long)s->cc[6], (unsigned long)s->cc[5]);
        if (st_col) putchar('\n');
    } else if (!(s->t.c_lflag & 0x2))
        st_wrapf("min = %lu; time = %lu;\n", (unsigned long)s->cc[6], (unsigned long)s->cc[5]);
    else if (!empty)
        putchar('\n');
    st_col = 0;
    empty = 1;
    int prev = ST_CONTROL;
    for (int i = 0; st_modes[i].name; i++) {
        const struct st_mode *m = &st_modes[i];
        if (m->flags & ST_OMIT) continue;
        if (m->type != prev) {
            if (all || !empty) { putchar('\n'); st_col = 0; empty = 1; }
            prev = m->type;
        }
        tcflag_t w = *st_word(s, m->type);
        unsigned mask = m->mask ? m->mask : m->bits;
        if ((w & mask) == m->bits) {
            if (all || (m->flags & ST_SANE_UNSET)) { st_wrapf("%s", m->name); empty = 0; }
        } else if (all ? (m->flags & ST_REV) != 0
                       : (m->flags & (ST_SANE_SET | ST_REV)) == (ST_SANE_SET | ST_REV)) {
            st_wrapf("-%s", m->name);
            empty = 0;
        }
    }
    if (all || !empty) putchar('\n');
    st_col = 0;
}

static void st_show_save(const struct st_state *s) {
    printf("%lx:%lx:%lx:%lx", (unsigned long)s->t.c_iflag, (unsigned long)s->t.c_oflag,
           (unsigned long)s->t.c_cflag, (unsigned long)s->t.c_lflag);
    for (int i = 0; i < ST_NCC; i++) printf(":%lx", (unsigned long)s->cc[i]);
    putchar('\n');
}

/* stty -g's string into S; 0 if it is not one */
static int st_recover(const char *arg, struct st_state *s) {
    unsigned long v[4 + ST_NCC];
    const char *p = arg;
    for (int i = 0; i < 4 + ST_NCC; i++) {
        if (i) {
            if (*p != ':') return 0;
            p++;
        }
        char *end;
        errno = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (!isxdigit((unsigned char)*p) && *p != '-' && *p != '+') return 0;
        v[i] = strtoul(p, &end, 16);
        if (end == p || errno) return 0;
        p = end;
    }
    if (*p) return 0;
    if (v[0] > 0xffffffffUL || v[1] > 0xffffffffUL || v[2] > 0xffffffffUL || v[3] > 0xffffffffUL)
        return 0;
    for (int i = 0; i < ST_NCC; i++) if (v[4 + i] > 255) return 0;
    s->t.c_iflag = (tcflag_t)v[0];
    s->t.c_oflag = (tcflag_t)v[1];
    s->t.c_cflag = (tcflag_t)v[2];
    s->t.c_lflag = (tcflag_t)v[3];
    for (int i = 0; i < ST_NCC; i++) s->cc[i] = (unsigned char)v[4 + i];
    return 1;
}

/* GNU's integer_arg: base 0, b (512) and B (1024) suffixes, at most MAX */
static unsigned long st_integer(const char *s, unsigned long max) {
    char *end;
    errno = 0;
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '-' || !*p) su_error(1, 0, "invalid integer argument: %s", su_quote(s));
    unsigned long long v = strtoull(p, &end, 0);
    int over = errno == ERANGE;
    if (end == p) su_error(1, 0, "invalid integer argument: %s", su_quote(s));
    if (*end == 'b' || *end == 'B') {
        unsigned long long m = *end == 'b' ? 512 : 1024;
        if (v > ULLONG_MAX / m) over = 1;
        else v *= m;
        end++;
    }
    if (*end) su_error(1, 0, "invalid integer argument: %s", su_quote(s));
    if (over || v > max) su_error(1, EOVERFLOW, "invalid integer argument: %s", su_quote(s));
    return (unsigned long)v;
}

static void st_combo(struct st_state *s, const char *n, int rev) {
    tcflag_t *c = &s->t.c_cflag, *i = &s->t.c_iflag, *o = &s->t.c_oflag, *l = &s->t.c_lflag;
    if (!strcmp(n, "evenp") || !strcmp(n, "parity")) {
        if (rev) *c = (*c & ~0x100u & ~0x30u) | 0x30;
        else *c = (*c & ~0x200u & ~0x30u) | 0x100 | 0x20;
    } else if (!strcmp(n, "oddp")) {
        if (rev) *c = (*c & ~0x100u & ~0x30u) | 0x30;
        else *c = (*c & ~0x30u) | 0x20 | 0x200 | 0x100;
    } else if (!strcmp(n, "nl")) {
        if (rev) { *i = (*i | 0x100) & ~0x40u & ~0x80u; *o = (*o | 0x4) & ~0x8u & ~0x20u; }
        else { *i &= ~0x100u; *o &= ~0x4u; }
    } else if (!strcmp(n, "ek")) {
        s->cc[2] = 127;
        s->cc[3] = 21;
    } else if (!strcmp(n, "sane")) {
        for (int k = 0; st_modes[k].name; k++) {
            const struct st_mode *m = &st_modes[k];
            tcflag_t *w = st_word(s, m->type);
            unsigned mask = m->mask ? m->mask : m->bits;
            if (m->flags & ST_SANE_SET) *w = (*w & ~mask) | m->bits;
            else if (m->flags & ST_SANE_UNSET) *w = *w & ~mask & ~m->bits;
        }
        for (int k = 0; st_chars[k].name; k++) s->cc[st_chars[k].index] = st_chars[k].sane;
    } else if (!strcmp(n, "cbreak")) {
        if (rev) *l |= 0x2; else *l &= ~0x2u;
    } else if (!strcmp(n, "pass8")) {
        if (rev) { *c = (*c & ~0x30u) | 0x20 | 0x100; *i |= 0x20; }
        else { *c = (*c & ~0x100u & ~0x30u) | 0x30; *i &= ~0x20u; }
    } else if (!strcmp(n, "litout")) {
        if (rev) { *c = (*c & ~0x30u) | 0x20 | 0x100; *i |= 0x20; *o |= 0x1; }
        else { *c = (*c & ~0x100u & ~0x30u) | 0x30; *i &= ~0x20u; *o &= ~0x1u; }
    } else if (!strcmp(n, "raw") || !strcmp(n, "cooked")) {
        if ((n[0] == 'r' && rev) || (n[0] == 'c' && !rev)) {
            *i |= 0x2 | 0x4 | 0x20 | 0x100 | 0x400;      /* brkint ignpar istrip icrnl ixon */
            *o |= 0x1;
            *l |= 0x1 | 0x2;
        } else {
            *i = 0;
            *o &= ~0x1u;
            *l &= ~(0x1u | 0x2u | ST_XCASE);
            s->cc[6] = 1;
            s->cc[5] = 0;
        }
    } else if (!strcmp(n, "decctlq")) {
        if (rev) *i |= 0x800; else *i &= ~0x800u;
    } else if (!strcmp(n, "tabs")) {
        *o = (*o & ~ST_TABDLY) | (rev ? 0x1800u : 0u);
    } else if (!strcmp(n, "lcase") || !strcmp(n, "LCASE")) {
        if (rev) { *l &= ~ST_XCASE; *i &= ~ST_IUCLC; *o &= ~ST_OLCUC; }
        else { *l |= ST_XCASE; *i |= ST_IUCLC; *o |= ST_OLCUC; }
    } else if (!strcmp(n, "crt")) {
        *l |= 0x10 | ST_ECHOCTL | ST_ECHOKE;
    } else if (!strcmp(n, "dec")) {
        s->cc[0] = 3;
        s->cc[2] = 127;
        s->cc[3] = 21;
        *l |= 0x10 | ST_ECHOCTL | ST_ECHOKE;
        *i &= ~0x800u;
    }
}

static void st_bad(const char *fmt, const char *arg) {
    su_error(0, 0, fmt, su_quote(arg));
    su_try_help(1);
}

/* one pass over the settings: CHECKING validates them (no terminal yet);
 * else they change S, the window size, or print. *SET: tcsetattr needed */
static void st_apply(int n, char **set, struct st_state *s, int checking, int *set_attr, int *drain) {
    for (int k = 0; k < n; k++) {
        const char *arg = set[k];
        int rev = arg[0] == '-' && arg[1];
        const char *name = rev ? arg + 1 : arg;
        int found = 0;
        for (int i = 0; st_modes[i].name; i++) {
            if (strcmp(name, st_modes[i].name)) continue;
            found = 1;
            if (rev && !(st_modes[i].flags & ST_REV)) st_bad("invalid argument %s", arg);
            if (st_modes[i].type == ST_COMBO) st_combo(s, name, rev);
            else {
                tcflag_t *w = st_word(s, st_modes[i].type);
                unsigned mask = st_modes[i].mask;
                if (rev) *w &= ~st_modes[i].bits & ~mask;
                else *w = (*w & ~mask) | st_modes[i].bits;
            }
            *set_attr = 1;
            break;
        }
        if (found) continue;
        if (!strcmp(arg, "drain") || !strcmp(arg, "-drain")) { *drain = arg[0] != '-'; continue; }
        if (rev) st_bad("invalid argument %s", arg);
        for (int i = 0; st_chars[i].name; i++) {
            if (strcmp(arg, st_chars[i].name)) continue;
            found = 1;
            if (k == n - 1) st_bad("missing argument to %s", arg);
            const char *v = set[++k];
            unsigned long val;
            if (!strcmp(arg, "min") || !strcmp(arg, "time")) val = st_integer(v, 255);
            else if (!v[0] || !v[1]) val = (unsigned char)v[0];
            else if (!strcmp(v, "^-") || !strcmp(v, "undef")) val = 0;
            else if (v[0] == '^') val = v[1] == '?' ? 127 : ((unsigned char)v[1] & ~0140u);
            else val = st_integer(v, 255);
            s->cc[st_chars[i].index] = (unsigned char)val;
            *set_attr = 1;
            break;
        }
        if (found) continue;
        if (!strcmp(arg, "ispeed") || !strcmp(arg, "ospeed")) {
            if (k == n - 1) st_bad("missing argument to %s", arg);
            const char *v = set[++k];
            int code = st_speed_code(v);
            if (code < 0) st_bad(arg[0] == 'i' ? "invalid ispeed %s" : "invalid ospeed %s", v);
            if (arg[0] == 'i') st_set_ispeed(s, (unsigned)code);
            else st_set_ospeed(s, (unsigned)code);
            *set_attr = 1;
            continue;
        }
        if (!strcmp(arg, "rows") || !strcmp(arg, "cols") || !strcmp(arg, "columns")) {
            if (k == n - 1) st_bad("missing argument to %s", arg);
            const char *v = set[++k];
            if (checking) continue;
            unsigned long val = st_integer(v, INT_MAX);
            struct st_winsize w;
            if (st_getwin(&w) != 0) su_error(1, errno, "%s", su_quotef(st_dev));
            if (arg[0] == 'r') w.row = (unsigned short)val;
            else w.col = (unsigned short)val;
            if (ioctl(0, 0x5414 /* TIOCSWINSZ */, &w) != 0) su_error(1, errno, "%s", su_quotef(st_dev));
            continue;
        }
        if (!strcmp(arg, "size")) {
            if (checking) continue;
            struct st_winsize w;
            if (st_getwin(&w) != 0) su_error(1, errno, "%s", su_quotef(st_dev));
            printf("%d %d\n", w.row, w.col);
            continue;
        }
        if (!strcmp(arg, "speed")) {
            if (!checking) st_show_speed(s, 0);
            continue;
        }
        if (!strcmp(arg, "line")) {
            if (k == n - 1) st_bad("missing argument to %s", arg);
            const char *v = set[++k];
            unsigned long val = st_integer(v, ULONG_MAX);
            if (val > 255) su_error(0, EOVERFLOW, "invalid line discipline %s", su_quote(v));
            s->t.c_line = (cc_t)val;
            *set_attr = 1;
            continue;
        }
        int code = st_speed_code(arg);
        if (code >= 0) {
            st_set_ispeed(s, (unsigned)code);
            st_set_ospeed(s, (unsigned)code);
            *set_attr = 1;
            continue;
        }
        if (st_recover(arg, s)) { *set_attr = 1; continue; }
        st_bad("invalid argument %s", arg);
    }
}

static void st_get(struct st_state *s) {
    memset(s, 0, sizeof *s);
    if (tcgetattr(0, &s->t) != 0) su_error(1, errno, "%s", su_quotef(st_dev));
    size_t kc = sizeof s->t.c_cc < ST_NCC ? sizeof s->t.c_cc : ST_NCC;
    for (size_t i = 0; i < kc; i++) s->cc[i] = s->t.c_cc[i];
}

static void st_put(struct st_state *s) {
    size_t kc = sizeof s->t.c_cc < ST_NCC ? sizeof s->t.c_cc : ST_NCC;
    for (size_t i = 0; i < kc; i++) s->t.c_cc[i] = s->cc[i];
}

static int st_same(const struct st_state *a, const struct st_state *b) {
    if (a->t.c_iflag != b->t.c_iflag || a->t.c_oflag != b->t.c_oflag ||
        a->t.c_cflag != b->t.c_cflag || a->t.c_lflag != b->t.c_lflag ||
        a->t.c_line != b->t.c_line)
        return 0;
    return memcmp(a->cc, b->cc, sizeof a->cc) == 0;
}

static int do_stty(int argc, char **argv) {
    int verbose = 0, save = 0, nset = 0;
    const char *file = 0;
    char **set = (char **)malloc((size_t)(argc + 1) * sizeof *set);
    if (!set) su_error(1, ENOMEM, "memory exhausted");
    static const char *const longs[] = { "all", "save", "file", "help", "version" };
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == '-' && a[2]) {
            const char *nm = a + 2;
            size_t len = strcspn(nm, "=");
            int hit = -1, nhits = 0;
            for (int k = 0; k < 5; k++)
                if (!strncmp(longs[k], nm, len)) {
                    if (strlen(longs[k]) == len) { hit = k; nhits = 1; break; }
                    hit = k;
                    nhits++;
                }
            if (nhits == 1 && (nm[len] != '=' || hit == 2)) {
                switch (hit) {
                case 0: verbose = 1; continue;
                case 1: save = 1; continue;
                case 2:
                    if (nm[len] == '=') { if (file) su_error(1, 0, "only one device may be specified"); file = nm + len + 1; continue; }
                    if (i + 1 < argc) {
                        if (file) su_error(1, 0, "only one device may be specified");
                        file = argv[++i];
                        continue;
                    }
                    break;
                case 3:
                    printf("Usage: %s [-F DEVICE | --file=DEVICE] [SETTING]...\n"
                           "  or:  %s [-F DEVICE | --file=DEVICE] [-a|--all]\n"
                           "  or:  %s [-F DEVICE | --file=DEVICE] [-g|--save]\n"
                           "Show or change the terminal's settings.\n"
                           "\n"
                           "  -a, --all          every setting, as people read them\n"
                           "  -g, --save         every setting, as stty reads them back\n"
                           "  -F, --file=DEVICE  DEVICE, not standard input\n"
                           "      --help         show this help and exit\n"
                           "      --version      show the version and exit\n"
                           "\n"
                           "A setting with a - in front is cleared. They include: parenb\n"
                           "parodd cs5-cs8 hupcl cstopb cread clocal crtscts ignbrk brkint\n"
                           "ignpar parmrk inpck istrip inlcr igncr icrnl ixon ixoff iuclc\n"
                           "ixany imaxbel iutf8 opost olcuc ocrnl onlcr onocr onlret ofill\n"
                           "ofdel nl0-1 cr0-3 tab0-3 bs0-1 vt0-1 ff0-1 isig icanon iexten\n"
                           "echo echoe echok echonl noflsh xcase tostop echoprt echoctl\n"
                           "echoke flusho extproc; raw cooked cbreak sane evenp oddp parity\n"
                           "pass8 litout nl ek tabs decctlq lcase crt dec; CHAR VALUE for\n"
                           "intr quit erase kill eof eol eol2 swtch start stop susp rprnt\n"
                           "werase lnext discard (^C, ^?, undef, a number); min N, time N,\n"
                           "N (a speed), ispeed N, ospeed N, rows N, cols N, size, speed,\n"
                           "line N, drain.\n",
                           su_name, su_name, su_name);
                    return 0;
                case 4: return su_version();
                }
            }
            set[nset++] = argv[i];
            continue;
        }
        if (a[0] == '-' && a[1] && strspn(a + 1, "agF") > 0) {
            /* a cluster of -a, -g and -F (which takes the rest, or the next
             * argument); anything else is a setting */
            size_t ok = strspn(a + 1, "ag");
            const char *rest = a + 1 + ok;
            if (!*rest) {
                for (const char *p = a + 1; *p; p++) { if (*p == 'a') verbose = 1; else save = 1; }
                continue;
            }
            if (*rest == 'F' && (rest[1] || i + 1 < argc)) {
                for (const char *p = a + 1; p < rest; p++) { if (*p == 'a') verbose = 1; else save = 1; }
                if (file) su_error(1, 0, "only one device may be specified");
                file = rest[1] ? rest + 1 : argv[++i];
                continue;
            }
        }
        set[nset++] = argv[i];
    }
    if (verbose && save) {
        fprintf(stderr, "%s: the options for verbose and stty-readable output styles are\n"
                        "mutually exclusive\n", su_short);
        return 1;
    }
    /* drain and -drain are GNU's options, not settings: alone, they leave
     * stty listing the settings (`alias stty='stty -drain'` works) */
    int modes = 0;
    for (int i = 0; i < nset; i++)
        if (strcmp(set[i], "drain") && strcmp(set[i], "-drain")) modes++;
    if ((verbose || save) && modes)
        su_error(1, 0, "when specifying an output style, modes may not be set");

    struct st_state s, chk;
    int set_attr = 0, drain = 1;
    memset(&chk, 0, sizeof chk);
    st_apply(nset, set, &chk, 1, &set_attr, &drain);

    if (file) {
        int fd = open(file, O_RDONLY | O_NONBLOCK);
        if (fd < 0) su_error(1, errno, "%s", su_quotef(file));
        if (fd != 0) { dup2(fd, 0); close(fd); }
        st_dev = file;
    }
    st_get(&s);

    struct st_winsize w;
    if (ioctl(1, 0x5413, &w) == 0 && w.col > 0) st_max_col = w.col;
    else {
        const char *e = getenv("COLUMNS");
        long c = e ? strtol(e, 0, 0) : 0;
        st_max_col = c > 0 && c <= INT_MAX ? (int)c : 80;
    }

    if (verbose || save || !modes) {
        if (save) st_show_save(&s);
        else st_show(&s, verbose);
        return 0;
    }
    set_attr = 0;
    st_apply(nset, set, &s, 0, &set_attr, &drain);
    if (set_attr) {
        st_put(&s);
        if (tcsetattr(0, drain ? TCSADRAIN : TCSANOW, &s.t) != 0)
            su_error(1, errno, "%s", su_quotef(st_dev));
        struct st_state now;
        st_get(&now);
        if (!st_same(&s, &now))
            su_error(1, 0, "%s: unable to perform all requested operations", su_quotef(st_dev));
    }
    return 0;
}
