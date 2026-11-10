/* date -- print the date and time.
 *
 * POSIX XCU:  date [-u] [+format]
 * and the GNU/busybox options scripts lean on:
 *   -d STRING   format STRING instead of now: @EPOCH, YYYY-MM-DD,
 *               YYYY-MM-DD[T ]HH:MM[:SS], now, today, yesterday, tomorrow
 *   -r FILE     format FILE's modification time
 *   -R          RFC 2822 (`Tue, 14 Nov 2023 22:13:20 +0000`)
 *   -I[TIMESPEC] ISO 8601: date (default), hours, minutes, seconds, ns
 *   -s STRING   refused: nothing here can set the RTC, and pretending to
 *               would be worse than saying so
 *
 * HISTORY. This printed "System uptime" once, and then a real-looking date
 * counted from boot on a pretend 2025-01-01 base, because libtoby had no
 * realtime clock to ask: the native ABI exposed only milliseconds since
 * boot. On the real EliteDesk it said `Wed Jan 1 00:01:11 UTC 2025` seventy
 * seconds after a boot whose RTC read 2026-09-01, while `busybox date` on
 * the same machine was right. ABI_SYS_CLOCK_NS (2026-09-03) closed that;
 * this program formats what time(2) now honestly returns, and the distro
 * gate holds its output against busybox's, conversion by conversion.
 *
 * There are no time zones, so -u is accepted and changes nothing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static int digits(const char **pp, int count, int *out) {
    const char *p = *pp;
    int v = 0;
    for (int i = 0; i < count; i++) {
        if (p[i] < '0' || p[i] > '9') return 0;
        v = v * 10 + (p[i] - '0');
    }
    *pp = p + count;
    *out = v;
    return 1;
}

/* The subset of GNU's -d grammar that scripts actually write. Returns 0
 * on success. */
static int parse_date(const char *s, time_t now, time_t *out) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '@') {
        char *end;
        long v = strtol(s + 1, &end, 10);
        if (end == s + 1) return -1;
        while (*end == ' ') end++;
        if (*end) return -1;
        *out = (time_t)v;
        return 0;
    }
    if (!strcmp(s, "now") || !strcmp(s, "today")) { *out = now; return 0; }
    if (!strcmp(s, "yesterday")) { *out = now - 86400; return 0; }
    if (!strcmp(s, "tomorrow"))  { *out = now + 86400; return 0; }

    /* YYYY-MM-DD[[T ]HH:MM[:SS]][Z] */
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    int y, mo, d, h = 0, mi = 0, se = 0;
    if (!digits(&s, 4, &y) || *s != '-') return -1;
    s++;
    if (!digits(&s, 2, &mo) || *s != '-') return -1;
    s++;
    if (!digits(&s, 2, &d)) return -1;
    if (*s == 'T' || *s == ' ') {
        s++;
        if (!digits(&s, 2, &h) || *s != ':') return -1;
        s++;
        if (!digits(&s, 2, &mi)) return -1;
        if (*s == ':') {
            s++;
            if (!digits(&s, 2, &se)) return -1;
        }
    }
    if (*s == 'Z') s++;
    while (*s == ' ') s++;
    if (*s) return -1;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60)
        return -1;
    tm.tm_year = y - 1900;
    tm.tm_mon  = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min  = mi;
    tm.tm_sec  = se;
    *out = timegm(&tm);
    return 0;
}

static void usage(void) {
    fprintf(stderr, "usage: date [-u] [-d STRING] [-r FILE] [-R] [-I[TIMESPEC]] [+format]\n");
}

int main(int argc, char **argv) {
    /* POSIX's default output format. */
    const char *fmt = "%a %b %e %H:%M:%S %Z %Y";
    const char *when_str = 0, *ref_file = 0;
    int i = 1;

    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "-u") || !strcmp(a, "--utc") || !strcmp(a, "--universal")) {
            i++;
            continue;
        }
        if (!strcmp(a, "-R") || !strcmp(a, "--rfc-email") || !strcmp(a, "--rfc-2822")) {
            fmt = "%a, %d %b %Y %H:%M:%S %z";
            i++;
            continue;
        }
        if (!strncmp(a, "-I", 2) || !strncmp(a, "--iso-8601", 10)) {
            const char *spec = a[1] == 'I' ? a + 2 : (a[10] == '=' ? a + 11 : "");
            if (!*spec || !strcmp(spec, "date"))    fmt = "%Y-%m-%d";
            else if (!strcmp(spec, "hours"))        fmt = "%Y-%m-%dT%H+00:00";
            else if (!strcmp(spec, "minutes"))      fmt = "%Y-%m-%dT%H:%M+00:00";
            else if (!strcmp(spec, "seconds"))      fmt = "%Y-%m-%dT%H:%M:%S+00:00";
            else if (!strcmp(spec, "ns"))           fmt = "%Y-%m-%dT%H:%M:%S,%N+00:00";
            else {
                fprintf(stderr, "date: invalid argument '%s' for '--iso-8601'\n", spec);
                return 1;
            }
            i++;
            continue;
        }
        if (!strcmp(a, "-d") || !strcmp(a, "--date")) {
            if (i + 1 >= argc) { fprintf(stderr, "date: option requires an argument -- 'd'\n"); return 1; }
            when_str = argv[i + 1];
            i += 2;
            continue;
        }
        if (!strncmp(a, "--date=", 7)) { when_str = a + 7; i++; continue; }
        if (!strncmp(a, "-d", 2))      { when_str = a + 2; i++; continue; }
        if (!strcmp(a, "-r") || !strcmp(a, "--reference")) {
            if (i + 1 >= argc) { fprintf(stderr, "date: option requires an argument -- 'r'\n"); return 1; }
            ref_file = argv[i + 1];
            i += 2;
            continue;
        }
        if (!strncmp(a, "--reference=", 12)) { ref_file = a + 12; i++; continue; }
        if (!strncmp(a, "-r", 2))            { ref_file = a + 2;  i++; continue; }
        if (!strcmp(a, "-s") || !strcmp(a, "--set") || !strncmp(a, "--set=", 6)) {
            fprintf(stderr, "date: cannot set date: the clock is the RTC and nothing here writes it\n");
            return 1;
        }
        if (!strcmp(a, "--help")) { usage(); return 0; }
        if (!strcmp(a, "--version")) { printf("date (tobyOS) 1.1\n"); return 0; }
        fprintf(stderr, "date: unknown option '%s'\n", a);
        usage();
        return 1;
    }

    if (i < argc) {
        /* An operand that is not a `+format` is a date to SET, which needs
         * privileges this has never had -- and `date %x` (a format someone
         * forgot the `+` on) must fail rather than silently print something
         * else. */
        if (argv[i][0] != '+') {
            fprintf(stderr, "date: invalid date '%s'\n", argv[i]);
            return 1;
        }
        fmt = argv[i] + 1;
        i++;
    }
    if (i < argc) {
        fprintf(stderr, "date: extra operand '%s'\n", argv[i]);
        return 1;
    }

    time_t now = time(0);
    time_t when = now;
    if (ref_file) {
        struct stat st;
        if (stat(ref_file, &st) != 0) {
            fprintf(stderr, "date: %s: No such file or directory\n", ref_file);
            return 1;
        }
        when = st.st_mtime;
    }
    if (when_str && parse_date(when_str, now, &when) != 0) {
        fprintf(stderr, "date: invalid date '%s'\n", when_str);
        return 1;
    }

    struct tm *tm = localtime(&when);
    if (!tm) {
        fprintf(stderr, "date: cannot read the clock\n");
        return 1;
    }

    char buf[1024];
    size_t n = strftime(buf, sizeof buf, fmt, tm);
    /* strftime returns 0 both for "would not fit" and for an empty result;
     * an empty format is legal and prints just the newline. */
    if (n == 0) buf[0] = '\0';
    printf("%s\n", buf);
    return 0;
}
