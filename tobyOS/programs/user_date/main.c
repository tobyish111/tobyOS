/* date -- print the date and time.
 *
 * POSIX XCU:  date [-u] [+format]
 *
 * This used to print "System uptime: HH:MM:SS" and the millisecond count
 * since boot, ignoring every argument. That is not what any script asking
 * for `date` wants, and it is NOT DETERMINISTIC: the conformance gate runs
 * each case under bash and then under tsh, and the two runs are seconds
 * apart, so a case that merely mentioned `date` reported a shell divergence
 * that was a clock reading.
 *
 * The clock underneath is libtoby's, which counts from boot against a fixed
 * 2025-01-01 base -- there is no RTC sync yet. That is wrong in absolute
 * terms and right in every relative one, which is what `date +%x` inside a
 * script actually depends on.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv) {
    /* POSIX's default output format. */
    const char *fmt = "%a %b %e %H:%M:%S %Z %Y";
    int i = 1;

    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        /* -u is accepted and ignored: there are no timezones here, so
         * everything already IS UTC. */
        if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--utc") == 0 ||
            strcmp(argv[i], "--universal") == 0) {
            i++;
            continue;
        }
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        fprintf(stderr, "date: unknown option '%s'\n", argv[i]);
        fprintf(stderr, "usage: date [-u] [+format]\n");
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
    struct tm *tm = localtime(&now);
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
