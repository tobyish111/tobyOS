/* su_sleep.c -- sleep, as GNU coreutils 9.7's (logs/utilgate/staging/
 * sleep.sh): pause for the sum of its operands. The behaviour of the
 * earlier native sleep (ports/sbase/sleep.c, batch 23, finding 129),
 * moved here; its number reading is su_txio.c's tx_strtons.
 *
 * WHAT WAS MEASURED, in the C locale:
 *   - NUMBER as strtod reads it: blanks before it, a sign, digits with a
 *     fraction and an exponent, hex digits with a binary exponent, inf or
 *     infinity in any case; then at most one SUFFIX -- s, m, h or d --
 *     and nothing else. Not negative (-0 is zero), not nan. The operands
 *     add up; too long to count, or inf, is forever.
 *   - Every bad operand is named, "invalid time interval 'X'", then one
 *     "Try" line, status 1, and no pause. No argument at all is "missing
 *     operand"; `sleep --` pauses for nothing.
 *   - The options wherever they stand (getopt permutes them) until --:
 *     --help and --version, any prefix of either; any other, -1 too, is
 *     getopt's error, before any operand is looked at.
 *
 * Integer arithmetic only: the guest's programs are built without SSE. */

#include "su_txio.c"

/* one operand into nanoseconds; 0, or -1 when it is not one */
static int txs_interval(const char *arg, unsigned long long *out) {
    unsigned long long ns;
    int neg, isnan;
    const char *p;
    if (tx_strtons(arg, &ns, &neg, &isnan, &p) < 0 || isnan) return -1;
    unsigned long long mult = 1;
    switch (*p) {
    case 0: break;
    case 's': p++; break;
    case 'm': mult = 60; p++; break;
    case 'h': mult = 3600; p++; break;
    case 'd': mult = 86400; p++; break;
    default: return -1;
    }
    if (*p) return -1;
    if (neg && ns != 0) return -1;
    *out = ns == TX_FOREVER ? TX_FOREVER : tx_sat_mul(ns, mult);
    return 0;
}

static void txs_help(void) {
    tx_printf("Usage: %s NUMBER[SUFFIX]...\n"
              "  or:  %s OPTION\n"
              "Wait NUMBER seconds, then exit. NUMBER may be whole or fractional;\n"
              "SUFFIX is s for seconds (the default), m for minutes, h for hours or\n"
              "d for days. Several NUMBERs are waited in turn; inf waits forever.\n"
              "\n"
              "      --help     show this help and exit\n"
              "      --version  show the version and exit\n", su_name, su_name);
}

static int do_sleep(int argc, char **argv) {
    static const struct su_option longs[] = { SU_LONG_HV, { 0, 0, 0, 0 } };
    tx_begin(1);
    if (argc < 2) {
        tx_error(0, 0, "missing operand");
        tx_try_help(1);
    }
    int c;
    while ((c = su_getopt_long(argc, argv, "", longs, 0)) != -1) {
        switch (c) {
        case SU_HELP_OPT: txs_help(); return 0;
        case SU_VERSION_OPT: return tx_version();
        default: tx_try_help(1);
        }
    }
    unsigned long long total = 0;
    int bad = 0;
    for (int i = su_optind; i < argc; i++) {
        unsigned long long ns;
        if (txs_interval(argv[i], &ns) < 0) {
            tx_error(0, 0, "invalid time interval %s", tx_quote(argv[i]));
            bad = 1;
            continue;
        }
        total = tx_sat_add(total, ns);
    }
    if (bad) tx_try_help(1);
    tx_nap(total);
    return 0;
}
