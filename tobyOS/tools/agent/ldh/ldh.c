/* ldh.c -- shell_ldouble.inc against glibc's strtold and printf("%L..."):
 * random and edge operands through every conversion, precision and flag
 * that the two C libraries agree on (not %a: glibc's long double %a does not
 * normalise as musl's does). Prints each mismatch; "ALL OK" at the end. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#define kmalloc malloc
#define kfree free

struct shell_sb { char *p; size_t len, cap; };
static void sb_init(struct shell_sb *b) { b->p = 0; b->len = b->cap = 0; }
static void sb_free(struct shell_sb *b) { free(b->p); b->p = 0; b->len = b->cap = 0; }
#define SH_SB(name) struct shell_sb name __attribute__((cleanup(sb_free))); sb_init(&name)
static int sb_putn(struct shell_sb *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 64;
        while (nc < b->len + n + 1) nc *= 2;
        b->p = realloc(b->p, nc);
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
    return 0;
}
static int sb_putc(struct shell_sb *b, char c) { return sb_putn(b, &c, 1); }
static int sb_puts(struct shell_sb *b, const char *s) { return sb_putn(b, s, strlen(s)); }

#include "shell_ldouble.inc"

static int bad, total;

static void check(const char *arg, char conv, int prec, const char *flags, int width) {
    struct sh_ld v;
    bool er = false;
    const char *end = ld_scan(arg, &v, &er);
    errno = 0;
    char *gend;
    long double x = strtold(arg, &gend);
    int gerr = errno == ERANGE;
    /* musl's strtold gives "-nan" no sign; glibc's keeps it */
    if (isnan(x)) x = fabsl(x);
    total++;
    if ((size_t)(end - arg) != (size_t)(gend - arg)) {
        bad++;
        printf("END  %-24s tsh=%d glibc=%d\n", arg, (int)(end - arg), (int)(gend - arg));
        return;
    }
    if (er != gerr && !(isnan(x))) {
        /* glibc and musl differ on ERANGE for some subnormals; report */
        printf("ERANGE %-22s tsh=%d glibc=%d\n", arg, er, gerr);
    }
    char fmt[32];
    if (prec >= 0) snprintf(fmt, sizeof fmt, "%%%s%d.%dL%c", flags, width, prec, conv);
    else snprintf(fmt, sizeof fmt, "%%%s%dL%c", flags, width, conv);
    static char want[100000];
    snprintf(want, sizeof want, fmt, x);
    SH_SB(got);
    bool plus = strchr(flags, '+'), space = strchr(flags, ' '), left = strchr(flags, '-');
    bool zero = strchr(flags, '0'), alt = strchr(flags, '#');
    ld_format(&got, &v, conv, prec, alt, plus, space, left, zero, width);
    if (strcmp(got.p ? got.p : "", want) != 0) {
        bad++;
        if (bad < 60)
            printf("FMT  %-24s %-10s tsh=[%s] glibc=[%s]\n", arg, fmt, got.p ? got.p : "", want);
    }
}

int main(void) {
    static const char *fixed[] = {
        "0", "-0", "1", "-1", "0.1", "2.675", "1.1", "0.3", "1e300", "1e-300",
        "1e-310", "0.5", "1.5", "2.5", "3.5", "123456789012345678901234567890",
        "1e4932", "1.18973149535723176502e4932", "1.2e4932", "1e-4950",
        "3.6e-4951", "1e-4951", "4e-4951", "1e-5000", "1e5000", "0x10",
        "0x1p-16445", "0x1.fffffffffffffffep16383", "0x1p16384", "0x.8",
        "0x1.8p1", "0X1P3", "0x", "0x.", "1e", "1e+", "1e-", ".5", "5.",
        ".", "+", "-", "", "  7", "7  ", "inf", "-inf", "INFINITY", "infinx",
        "nan", "-nan", "NaN(123)", "nan(", "in", "na", "0.0000001",
        "999999999999999999999", "18446744073709551615", "18446744073709551616",
        "9.999999999999999999999", "0.000000000000000000000000000001",
        "4.9406564584124654e-324", "2.2250738585072014e-308",
        "1.7976931348623157e308", "3.3621031431120935063e-4932",
        "123.456", "-0.0005", "99.995", "0.045", "1e22", "1e23", "5e-324",
        "8.5", "0.125", "0.375", "1234567.890123", "6.02214076e23",
        "0x1.23456789abcdef0123p0", "0x123456789ABCDEF0123", "1e-4945",
        0 };
    static const char convs[] = "feEgG";
    static const int precs[] = { -1, 0, 1, 2, 3, 5, 10, 17, 18, 19, 20, 25, 30, 40 };
    static const char *flagsets[] = { "", "+", " ", "#", "-", "0", "+0", "#0", "-+" };
    for (int i = 0; fixed[i]; i++)
        for (int c = 0; convs[c]; c++)
            for (size_t pi = 0; pi < sizeof precs / sizeof precs[0]; pi++)
                for (size_t fi = 0; fi < sizeof flagsets / sizeof flagsets[0]; fi++)
                    check(fixed[i], convs[c], precs[pi], flagsets[fi], fi % 3 ? 12 : 0);
    /* random decimals and exponents */
    srand(12345);
    char buf[128];
    for (int t = 0; t < 40000; t++) {
        int nd = 1 + rand() % 25;
        int n = 0;
        if (rand() % 4 == 0) buf[n++] = '-';
        int pt = rand() % (nd + 1);
        for (int k = 0; k < nd; k++) {
            if (k == pt) buf[n++] = '.';
            buf[n++] = (char)('0' + rand() % 10);
        }
        if (rand() % 2) n += sprintf(buf + n, "e%d", rand() % 9900 - 4950);
        buf[n] = 0;
        check(buf, convs[rand() % 5], precs[rand() % 14], flagsets[rand() % 9], rand() % 30);
    }
    printf("%s: %d of %d differ\n", bad ? "DIFFER" : "ALL OK", bad, total);
    return bad != 0;
}
