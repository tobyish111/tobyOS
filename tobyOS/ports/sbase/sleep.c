/* sleep -- suspend execution for an interval.
 *
 *     sleep NUMBER[SUFFIX]...
 *     sleep --help | --version
 *
 * AS GNU COREUTILS' SLEEP READS ITS OPERANDS, measured on coreutils 9.7 in
 * the C locale (tools/agent/probes/b23/sleepcases*.sh; logs/b23-gnu-
 * sleep*.out), which is what a Linux user has in their fingers:
 *
 *   - NUMBER as strtod reads it: blanks before it, a '+', digits with a
 *     fraction and an exponent, a hex number with a binary exponent, inf
 *     or infinity in any case; then at most one SUFFIX -- s, m, h or d --
 *     and nothing else. Not negative (-0 is zero), not nan. Several
 *     operands add up; too long to count, or inf, is forever.
 *   - Every bad operand is named, "invalid time interval 'X'", then one
 *     "Try 'sleep --help' for more information.", status 1, no sleep.
 *     No operand at all is "missing operand" -- `sleep --` sleeps nothing.
 *   - Options wherever they stand, as getopt permutes them, until `--`:
 *     --help and --version (any prefix of either), each without an
 *     argument; any other is an error, named as getopt names it.
 *
 * It said "usage: sleep seconds" for anything but one plain number --
 * no suffix, no sum, no inf, no hex, no --help (finding 129).
 *
 * Integer arithmetic only: the ports are built without SSE. An operand
 * becomes nanoseconds, saturating at ULLONG_MAX, which means forever. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

#define NS_PER_S  1000000000ULL
#define FOREVER   (~0ULL)

static const char *g_name;     /* argv[0] as typed: getopt's and the Try line's */
static const char *g_short;    /* its last component: an error's prefix */

static void try_help(void) {
    fprintf(stderr, "Try '%s --help' for more information.\n", g_name);
    exit(1);
}

static unsigned long long sat_mul(unsigned long long a, unsigned long long b) {
    if (a == 0 || b == 0) return 0;
    if (a == FOREVER || b == FOREVER || b > FOREVER / a) return FOREVER;
    return a * b;
}

static unsigned long long sat_add(unsigned long long a, unsigned long long b) {
    return a > FOREVER - b ? FOREVER : a + b;
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Does P begin with the lower-case word W, in any case? */
static int starts_ci(const char *p, const char *w) {
    for (; *w; p++, w++)
        if (lower((unsigned char)*p) != *w) return 0;
    return 1;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lower(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* An exponent's digits after e or p, signed, if any follow: *P moves past
 * them; without digits it stays on the e or p, which strtod leaves. */
static long take_exponent(const char **pp) {
    const char *q = *pp + 1;
    long sign = 1, e = 0;
    if (*q == '+' || *q == '-') { if (*q == '-') sign = -1; q++; }
    if (*q < '0' || *q > '9') return 0;
    while (*q >= '0' && *q <= '9') {
        if (e < 100000) e = e * 10 + (*q - '0');
        q++;
    }
    *pp = q;
    return sign * e;
}

/* One operand into nanoseconds (FOREVER for inf or past counting); 0 if
 * it is one, -1 if not. */
static int parse_interval(const char *arg, unsigned long long *out) {
    const char *p = arg;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;      /* isspace */
    int neg = 0;
    if (*p == '+') p++;
    else if (*p == '-') { neg = 1; p++; }

    enum { DEC, HEX, INF } kind;
    unsigned long long m = 0;      /* the digits, at most 18 decimal / 15 hex */
    long e = 0;                    /* the exponent: of 10 (DEC) or of 2 (HEX) */
    if (starts_ci(p, "inf")) {
        kind = INF;
        p += starts_ci(p, "infinity") ? 8 : 3;
    } else if (starts_ci(p, "nan")) {
        return -1;
    } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
               (hexval(p[2]) >= 0 || (p[2] == '.' && hexval(p[3]) >= 0))) {
        kind = HEX;
        p += 2;
        for (; hexval(*p) >= 0; p++) {
            if (m < (1ULL << 56)) m = m * 16 + (unsigned)hexval(*p);
            else e += 4;
        }
        if (*p == '.')
            for (p++; hexval(*p) >= 0; p++)
                if (m < (1ULL << 56)) { m = m * 16 + (unsigned)hexval(*p); e -= 4; }
        if (*p == 'p' || *p == 'P') e += take_exponent(&p);
    } else {
        kind = DEC;
        int digits = 0;
        for (; *p >= '0' && *p <= '9'; p++, digits++) {
            if (m < 100000000000000000ULL) m = m * 10 + (unsigned)(*p - '0');
            else e++;
        }
        if (*p == '.')
            for (p++; *p >= '0' && *p <= '9'; p++, digits++)
                if (m < 100000000000000000ULL) { m = m * 10 + (unsigned)(*p - '0'); e--; }
        if (!digits) return -1;
        if (*p == 'e' || *p == 'E') e += take_exponent(&p);
    }

    unsigned long long mult = 1;
    switch (*p) {
    case '\0': break;
    case 's': mult = 1; p++; break;
    case 'm': mult = 60; p++; break;
    case 'h': mult = 3600; p++; break;
    case 'd': mult = 86400; p++; break;
    default:  return -1;
    }
    if (*p) return -1;
    if (neg && (kind == INF || m != 0)) return -1;

    unsigned long long ns;
    if (kind == INF) {
        ns = FOREVER;
    } else if (m == 0) {
        ns = 0;
    } else if (kind == DEC) {
        /* m * mult * 10^(e+9): scale down first where the product would
         * not fit, a digit dropped below a nanosecond being nothing */
        long k = e + 9;
        ns = m;
        while (k < 0 && ns > FOREVER / mult) { ns /= 10; k++; }
        ns = sat_mul(ns, mult);
        for (; k > 0 && ns != FOREVER; k--) ns = sat_mul(ns, 10);
        for (; k < 0 && ns; k++) ns /= 10;
    } else {
        /* m * mult * 2^e seconds, the same way */
        ns = sat_mul(m, mult);
        while (e < 0 && ns > FOREVER / NS_PER_S) { ns >>= 1; e++; }
        ns = sat_mul(ns, NS_PER_S);
        for (; e > 0 && ns != FOREVER; e--) ns = sat_mul(ns, 2);
        for (; e < 0 && ns; e++) ns >>= 1;
    }
    *out = ns;
    return 0;
}

static void print_help(void) {
    printf("Usage: %s NUMBER[SUFFIX]...\n"
           "  or:  %s OPTION\n"
           "Wait NUMBER seconds, then exit. NUMBER may be whole or fractional;\n"
           "SUFFIX is s for seconds (the default), m for minutes, h for hours or\n"
           "d for days. Several NUMBERs are waited in turn; inf waits forever.\n"
           "\n"
           "      --help     show this help and exit\n"
           "      --version  show the version and exit\n", g_name, g_name);
}

static void nap(unsigned long long ns) {
    const unsigned long long day = 86400ULL * NS_PER_S;
    while (ns) {
        unsigned long long chunk = ns > day ? day : ns;
        struct timespec ts = { (time_t)(chunk / NS_PER_S), (long)(chunk % NS_PER_S) };
        nanosleep(&ts, 0);
        if (ns != FOREVER) ns -= chunk;
    }
}

int main(int argc, char *argv[]) {
    g_name = (argc > 0 && argv[0] && argv[0][0]) ? argv[0] : "sleep";
    g_short = strrchr(g_name, '/') ? strrchr(g_name, '/') + 1 : g_name;
    util_argv0_set(g_short);

    if (argc < 2) {
        fprintf(stderr, "%s: missing operand\n", g_short);
        try_help();
    }

    /* the options first, in order, wherever they stand, up to `--` */
    int dashdash = argc;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) { dashdash = i; break; }
        if (a[0] != '-' || a[1] == '\0') continue;          /* an operand */
        if (a[1] == '-') {
            const char *name = a + 2;
            size_t n = strcspn(name, "=");
            int help = n > 0 && n <= 4 && strncmp(name, "help", n) == 0;
            int ver  = n > 0 && n <= 7 && strncmp(name, "version", n) == 0;
            if (!help && !ver) {
                fprintf(stderr, "%s: unrecognized option '%s'\n", g_name, a);
                try_help();
            }
            if (name[n] == '=') {
                fprintf(stderr, "%s: option '--%s' doesn't allow an argument\n",
                        g_name, help ? "help" : "version");
                try_help();
            }
            if (help) print_help();
            else printf("sleep (tobyOS) 1.0\n");
            return 0;
        }
        fprintf(stderr, "%s: invalid option -- '%c'\n", g_name, a[1]);
        try_help();
    }

    unsigned long long total = 0;
    int bad = 0;
    for (int i = 1; i < argc; i++) {
        if (i == dashdash) continue;
        unsigned long long ns;
        if (parse_interval(argv[i], &ns) != 0) {
            fprintf(stderr, "%s: invalid time interval '%s'\n", g_short, argv[i]);
            bad = 1;
            continue;
        }
        total = sat_add(total, ns);
    }
    if (bad) try_help();
    nap(total);
    return 0;
}
