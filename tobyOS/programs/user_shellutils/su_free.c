/* su_free.c -- free, as procps-ng 4.0.4's (logs/utilgate/free.sh).
 *
 *   free [options]
 *
 *   - Mem and Swap rows (total used free shared buff/cache available), -w
 *     buffers and cache apart, -l Low and High, -t a Total row, -v Comm
 *     (CommitLimit, Committed_AS, what is left -- negative when overcommitted),
 *     -L all on one line; -s N repeats every N seconds (fractions too), -c N
 *     N times (each second without -s), a blank line between tables.
 *   - procps's arithmetic: used = total - available; available is MemFree
 *     when MemAvailable is missing or above MemTotal; cache = Cached +
 *     SReclaimable; swap used is never below 0; Low is all of memory when
 *     there is no LowTotal.
 *   - Units: kibibytes, or -b, -k -m -g --tebi --pebi, --kilo --mega --giga
 *     --tera --peta (--si: powers of 1000), truncated; one unit option
 *     only ("Multiple unit options don't make sense."). -h as procps prints
 *     it: the first unit whose value fits four characters ("3.8Gi", "96Mi",
 *     "0B"), one decimal of a single-precision float or the value
 *     truncated.
 *   - procps's messages: "seconds argument `N' is not positive number",
 *     "failed to parse count argument: 'N'", usage on stderr (status 1) for
 *     a bad option or an operand.
 *
 * FREE_MEMINFO, if set, names the file read in place of /proc/meminfo:
 * utilgate's hook, so fixed numbers can be compared with procps's (which
 * reads them through a bind mount there). Nothing else sets it.
 *
 * It took -h -m -g -k only, printed whole mebibytes for -h, counted buffers
 * and cache as used, and said nothing of the rest. */

struct fr_mem {
    uint64_t total, free, avail, buffers, cached, shmem, srecl, swtotal, swfree;
    uint64_t lowtotal, lowfree, hightotal, highfree, commitlimit, committed;
    int have_avail, have_low;
};

static int fr_unit_exp, fr_si, fr_human;

static int fr_read(struct fr_mem *m) {
    const char *path = getenv("FREE_MEMINFO");
    if (!path || !*path) path = "/proc/meminfo";
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    memset(m, 0, sizeof *m);
    static const struct { const char *k; size_t off; } keys[] = {
        { "MemTotal", offsetof(struct fr_mem, total) },
        { "MemFree", offsetof(struct fr_mem, free) },
        { "MemAvailable", offsetof(struct fr_mem, avail) },
        { "Buffers", offsetof(struct fr_mem, buffers) },
        { "Cached", offsetof(struct fr_mem, cached) },
        { "Shmem", offsetof(struct fr_mem, shmem) },
        { "SReclaimable", offsetof(struct fr_mem, srecl) },
        { "SwapTotal", offsetof(struct fr_mem, swtotal) },
        { "SwapFree", offsetof(struct fr_mem, swfree) },
        { "LowTotal", offsetof(struct fr_mem, lowtotal) },
        { "LowFree", offsetof(struct fr_mem, lowfree) },
        { "HighTotal", offsetof(struct fr_mem, hightotal) },
        { "HighFree", offsetof(struct fr_mem, highfree) },
        { "CommitLimit", offsetof(struct fr_mem, commitlimit) },
        { "Committed_AS", offsetof(struct fr_mem, committed) },
    };
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
            if (strcmp(line, keys[i].k)) continue;
            *(uint64_t *)((char *)m + keys[i].off) = strtoull(colon + 1, 0, 10);
            if (!strcmp(line, "MemAvailable")) m->have_avail = 1;
            if (!strcmp(line, "LowTotal")) m->have_low = 1;
        }
    }
    fclose(f);
    if (!m->have_avail || m->avail > m->total) m->avail = m->free;
    if (!m->have_low) { m->lowtotal = m->total; m->lowfree = m->free; m->hightotal = m->highfree = 0; }
    return 0;
}

/* KB kibibytes (two's complement when an overcommit made it "negative")
 * as the options print it */
static void fr_fmt(uint64_t kb, char *out, size_t cap) {
    int64_t bytes = (int64_t)(kb * 1024u);
    double base = fr_si ? 1000.0 : 1024.0;
    if (!fr_human) {
        if (fr_unit_exp == 1) { snprintf(out, cap, "%" PRId64, bytes); return; }
        double div = fr_unit_exp == 0 ? base : 1;
        for (int i = 1; fr_unit_exp && i < fr_unit_exp; i++) div *= base;
        volatile double q = (double)bytes / div;
        snprintf(out, cap, "%ld", (long)q);
        return;
    }
    snprintf(out, cap, "%" PRId64 "B", bytes);
    if (strlen(out) <= 4) return;
    static const char units[] = "KMGTP";
    double div = 1;
    for (int i = 0; units[i]; i++) {
        div *= base;
        volatile double q = (double)bytes / div;
        volatile float fq = (float)q;
        struct sn_sb b = { 0, 0, 0 };
        struct su_fmtspec fs = { 0, 0, 0, 0, 0, -1, 1, 'f' };
        su_ldfmt(&b, (long double)fq, &fs);
        size_t lim = fr_si ? 4 : 5;
        snprintf(out, cap, "%s%c%s", b.s ? b.s : "0", units[i], fr_si ? "" : "i");
        free(b.s);
        if (strlen(out) <= lim) return;
        snprintf(out, cap, "%ld%c%s", (long)q, units[i], fr_si ? "" : "i");
        if (strlen(out) <= lim) return;
    }
}

static void fr_row(const char *label, int n, ...) {
    va_list ap;
    va_start(ap, n);
    printf("%-9s", label);
    for (int i = 0; i < n; i++) {
        char v[64];
        fr_fmt(va_arg(ap, uint64_t), v, sizeof v);
        printf(i ? " %11s" : "%11s", v);
    }
    va_end(ap);
    putchar('\n');
}

static void fr_usage(FILE *out) {
    fprintf(out, "\nUsage:\n %s [options]\n\nOptions:\n"
            " -b, --bytes         in bytes\n"
            "     --kilo          in kilobytes (1000)\n"
            "     --mega          in megabytes\n"
            "     --giga          in gigabytes\n"
            "     --tera          in terabytes\n"
            "     --peta          in petabytes\n"
            " -k, --kibi          in kibibytes (1024, the default)\n"
            " -m, --mebi          in mebibytes\n"
            " -g, --gibi          in gibibytes\n"
            "     --tebi          in tebibytes\n"
            "     --pebi          in pebibytes\n"
            " -h, --human         in the unit that suits each value\n"
            "     --si            powers of 1000, not 1024\n"
            " -l, --lohi          low and high memory rows too\n"
            " -L, --line          everything on one line\n"
            " -t, --total         a RAM + swap total row\n"
            " -v, --committed     the commit limit and what is committed\n"
            " -s N, --seconds N   show again every N seconds\n"
            " -c N, --count N     show N times, then stop\n"
            " -w, --wide          buffers and cache in columns of their own\n"
            "\n"
            "     --help     show this help and exit\n"
            " -V, --version  show the version and exit\n",
            su_name);
}

static int do_free(int argc, char **argv) {
    enum { KILO = 0x100, MEGA, GIGA, TERA, PETA, TEBI, PEBI, SI, HELP };
    static const struct su_option longs[] = {
        { "bytes", SU_NO_ARG, 0, 'b' }, { "kilo", SU_NO_ARG, 0, KILO },
        { "mega", SU_NO_ARG, 0, MEGA }, { "giga", SU_NO_ARG, 0, GIGA },
        { "tera", SU_NO_ARG, 0, TERA }, { "peta", SU_NO_ARG, 0, PETA },
        { "kibi", SU_NO_ARG, 0, 'k' }, { "mebi", SU_NO_ARG, 0, 'm' },
        { "gibi", SU_NO_ARG, 0, 'g' }, { "tebi", SU_NO_ARG, 0, TEBI },
        { "pebi", SU_NO_ARG, 0, PEBI }, { "human", SU_NO_ARG, 0, 'h' },
        { "si", SU_NO_ARG, 0, SI }, { "lohi", SU_NO_ARG, 0, 'l' },
        { "line", SU_NO_ARG, 0, 'L' }, { "total", SU_NO_ARG, 0, 't' },
        { "committed", SU_NO_ARG, 0, 'v' }, { "seconds", SU_REQ_ARG, 0, 's' },
        { "count", SU_REQ_ARG, 0, 'c' }, { "wide", SU_NO_ARG, 0, 'w' },
        { "help", SU_NO_ARG, 0, HELP }, { "version", SU_NO_ARG, 0, 'V' },
        { 0, 0, 0, 0 }
    };
    int lohi = 0, line = 0, total = 0, committed = 0, wide = 0, c;
    long double seconds = 0;
    int have_seconds = 0;
    long count = -1;
    while ((c = su_getopt_long(argc, argv, "bkmghlLtvc:ws:V", longs, 0)) != -1) {
        int exp = 0, si = 0;
        switch (c) {
        case 'b': exp = 1; break;
        case KILO: exp = 2; si = 1; break;
        case MEGA: exp = 3; si = 1; break;
        case GIGA: exp = 4; si = 1; break;
        case TERA: exp = 5; si = 1; break;
        case PETA: exp = 6; si = 1; break;
        case 'k': exp = 2; break;
        case 'm': exp = 3; break;
        case 'g': exp = 4; break;
        case TEBI: exp = 5; break;
        case PEBI: exp = 6; break;
        case 'h': fr_human = 1; break;
        case SI: fr_si = 1; break;
        case 'l': lohi = 1; break;
        case 'L': line = 1; break;
        case 't': total = 1; break;
        case 'v': committed = 1; break;
        case 'w': wide = 1; break;
        case 's': {
            char *end;
            errno = 0;
            long double v = su_strtold(su_optarg, &end);
            if (end == su_optarg || *end || errno) {
                su_error(1, errno ? errno : EINVAL, "seconds argument failed: %s", su_quote(su_optarg));
            }
            if (!(v > 0)) su_error(1, 0, "seconds argument `%s' is not positive number", su_optarg);
            seconds = v;
            have_seconds = 1;
            break;
        }
        case 'c': {
            char *end;
            errno = 0;
            long v = strtol(su_optarg, &end, 10);
            if (end == su_optarg || *end) su_error(1, 0, "failed to parse count argument: %s", su_quote(su_optarg));
            if (errno || v < 1) su_error(1, ERANGE, "failed to parse count argument: %s", su_quote(su_optarg));
            count = v;
            break;
        }
        case HELP: fr_usage(stdout); return 0;
        case 'V': return su_version();
        default: fr_usage(stderr); return 1;
        }
        if (exp) {
            if (fr_unit_exp) su_error(1, 0, "Multiple unit options don't make sense.");
            fr_unit_exp = exp;
            if (si) fr_si = 1;
        }
    }
    if (su_optind < argc) { fr_usage(stderr); return 1; }
    if (count > 0 && !have_seconds) seconds = 1;
    for (long round = 0;; round++) {
        struct fr_mem m;
        if (fr_read(&m) != 0) su_error(1, errno, "Unable to create meminfo structure");
        uint64_t cache = m.cached + m.srecl;
        uint64_t used = m.total - m.avail;
        uint64_t swused = m.swtotal > m.swfree ? m.swtotal - m.swfree : 0;
        if (line) {
            char a[64], b[64], d[64], e[64];
            fr_fmt(swused, a, sizeof a);
            fr_fmt(m.buffers + cache, b, sizeof b);
            fr_fmt(used, d, sizeof d);
            fr_fmt(m.free, e, sizeof e);
            printf("SwapUse %11s CachUse %11s  MemUse %11s MemFree %11s \n", a, b, d, e);
        } else {
            if (wide)
                printf("%-9s%11s %11s %11s %11s %11s %11s %11s\n", "", "total", "used", "free",
                       "shared", "buffers", "cache", "available");
            else
                printf("%-9s%11s %11s %11s %11s %11s %11s\n", "", "total", "used", "free",
                       "shared", "buff/cache", "available");
            if (wide) fr_row("Mem:", 7, m.total, used, m.free, m.shmem, m.buffers, cache, m.avail);
            else fr_row("Mem:", 6, m.total, used, m.free, m.shmem, m.buffers + cache, m.avail);
            if (lohi) {
                fr_row("Low:", 3, m.lowtotal, m.lowtotal - m.lowfree, m.lowfree);
                fr_row("High:", 3, m.hightotal, m.hightotal - m.highfree, m.highfree);
            }
            fr_row("Swap:", 3, m.swtotal, swused, m.swfree);
            if (total)
                fr_row("Total:", 3, m.total + m.swtotal, used + swused, m.free + m.swfree);
            if (committed)
                fr_row("Comm:", 3, m.commitlimit, m.committed, m.commitlimit - m.committed);
        }
        fflush(stdout);
        if (count > 0 && round + 1 >= count) break;
        if (!have_seconds && count <= 0) break;
        if (!line) putchar('\n');
        fflush(stdout);
        struct timespec ts;
        ts.tv_sec = (time_t)seconds;
        ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
        nanosleep(&ts, 0);
    }
    return 0;
}
