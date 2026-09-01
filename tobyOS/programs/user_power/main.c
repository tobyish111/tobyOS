/* shutdown / poweroff / halt / reboot
 *
 * One multi-call binary, dispatching on argv[0]. Four commands every distro
 * has, and none of them existed: the kernel has had acpi_shutdown() and
 * acpi_reboot() since ACPI bring-up, but nothing in the ABI reached them.
 * ABI_SYS_POWER does now.
 *
 *     shutdown [-r|-h|-H|-P] [-k] [TIME] [message]
 *     poweroff [-f]        halt [-p]        reboot [-f]
 *
 * TIME accepts `now`, `+m`, and `hh:mm`. A scheduled shutdown WAITS in the
 * foreground: there is no init to hand the job to, and pretending to
 * background it would leave the caller thinking the machine was going down
 * when nothing was going to happen.
 *
 * -k is the important flag for anyone testing this: it does everything
 * except pull the trigger.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <tobyos/abi/abi.h>

static long sc1(long n, long a1) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a1)
                     : "rcx", "r11", "memory");
    return r;
}

static const char *prog = "shutdown";

static void broadcast(const char *when, const char *msg) {
    /* wall(1) does not exist, and there is no utmp to enumerate logged-in
     * terminals from, so the warning goes to this terminal only. Said
     * plainly rather than silently reaching nobody. */
    fprintf(stderr, "\nBroadcast message from %s:\n", prog);
    fprintf(stderr, "The system is going down %s!\n", when);
    if (msg && *msg) fprintf(stderr, "%s\n", msg);
    fprintf(stderr, "(this terminal only: there is no wall and no utmp to "
                    "find other sessions from)\n\n");
}

/* Returns seconds to wait, or -1 if the time could not be parsed. */
static long parse_when(const char *t) {
    if (!t || !*t) return -1;
    if (!strcmp(t, "now")) return 0;
    if (t[0] == '+') {
        long m = 0;
        for (const char *p = t + 1; *p; p++) {
            if (*p < '0' || *p > '9') return -1;
            m = m * 10 + (*p - '0');
        }
        return m * 60;
    }
    /* hh:mm -- relative to the current wall clock. */
    const char *colon = strchr(t, ':');
    if (colon) {
        long hh = 0, mm = 0;
        for (const char *p = t; p < colon; p++) {
            if (*p < '0' || *p > '9') return -1;
            hh = hh * 10 + (*p - '0');
        }
        for (const char *p = colon + 1; *p; p++) {
            if (*p < '0' || *p > '9') return -1;
            mm = mm * 10 + (*p - '0');
        }
        if (hh > 23 || mm > 59) return -1;
        time_t now = time(NULL);
        struct tm *lt = localtime(&now);
        if (!lt) return -1;
        long target = hh * 3600 + mm * 60;
        long cur    = lt->tm_hour * 3600 + lt->tm_min * 60 + lt->tm_sec;
        long d = target - cur;
        if (d < 0) d += 24 * 3600;      /* tomorrow */
        return d;
    }
    return -1;
}

static int do_power(unsigned action, int dry) {
    static const char *names[] = { "power off", "reboot", "halt" };
    if (dry) {
        printf("%s: would %s now (-k given, so nothing was done)\n",
               prog, names[action <= 2 ? action : 0]);
        return 0;
    }
    fflush(stdout);
    fflush(stderr);
    long rc = sc1(ABI_SYS_POWER, (long)action);
    /* Only reached when the kernel could not act. */
    if (rc == -ABI_EPERM)
        fprintf(stderr, "%s: must be root\n", prog);
    else
        fprintf(stderr, "%s: the kernel could not %s (rc=%ld)\n",
                prog, names[action <= 2 ? action : 0], rc);
    return 1;
}

static void usage(void) {
    fprintf(stderr,
        "usage: %s [-r|-h|-H|-P] [-k] [TIME] [message]\n"
        "       poweroff [-f] | halt [-p] | reboot [-f]\n"
        "TIME is `now`, `+MINUTES`, or hh:mm.\n", prog);
    exit(1);
}

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "shutdown";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    prog = base;

    unsigned action = ABI_POWER_OFF;
    int dry = 0;
    int have_time = 0;
    long wait_s = 0;
    const char *msg = NULL;
    char msgbuf[512];

    if (!strcmp(base, "reboot"))   action = ABI_POWER_REBOOT;
    if (!strcmp(base, "halt"))     action = ABI_POWER_HALT;
    if (!strcmp(base, "poweroff")) action = ABI_POWER_OFF;

    int is_shutdown = !strcmp(base, "shutdown");

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            if (!strcmp(a, "--help")) usage();
            if (!strcmp(a, "--poweroff")) { action = ABI_POWER_OFF; continue; }
            if (!strcmp(a, "--reboot"))   { action = ABI_POWER_REBOOT; continue; }
            if (!strcmp(a, "--halt"))     { action = ABI_POWER_HALT; continue; }
            for (const char *p = a + 1; *p; p++) {
                switch (*p) {
                case 'r': action = ABI_POWER_REBOOT; break;
                case 'h': action = ABI_POWER_HALT;   break;  /* -h: halt */
                case 'H': action = ABI_POWER_HALT;   break;
                case 'P': action = ABI_POWER_OFF;    break;
                case 'p': action = ABI_POWER_OFF;    break;  /* halt -p */
                case 'k': dry = 1; break;
                case 'f': break;   /* force: no fsck to skip here */
                case 'n': break;
                default:
                    fprintf(stderr, "%s: invalid option -- '%c'\n", prog, *p);
                    usage();
                }
            }
            continue;
        }
        if (is_shutdown && !have_time) {
            long w = parse_when(a);
            if (w < 0) {
                fprintf(stderr, "%s: invalid time `%s'\n", prog, a);
                usage();
            }
            wait_s = w;
            have_time = 1;
            continue;
        }
        /* Everything after the time is the broadcast message. */
        if (!msg) {
            size_t off = 0;
            for (int k = i; k < argc && off + 1 < sizeof msgbuf; k++) {
                size_t l = strlen(argv[k]);
                if (off + l + 2 >= sizeof msgbuf) break;
                if (off) msgbuf[off++] = ' ';
                memcpy(msgbuf + off, argv[k], l);
                off += l;
            }
            msgbuf[off] = 0;
            msg = msgbuf;
        }
        break;
    }

    /* `shutdown` with no time at all is a usage error in every distro --
     * exactly so that a typo does not take the machine down. */
    if (is_shutdown && !have_time) usage();

    if (wait_s > 0) {
        char when[64];
        snprintf(when, sizeof when, "in %ld minute%s",
                 wait_s / 60, (wait_s / 60) == 1 ? "" : "s");
        broadcast(when, msg);
        /* Foreground, and said so: there is no init to hand this to. */
        printf("%s: waiting %ld seconds. Interrupt to cancel.\n", prog, wait_s);
        fflush(stdout);
        while (wait_s > 0) {
            unsigned step = wait_s > 60 ? 60u : (unsigned)wait_s;
            sleep(step);
            wait_s -= step;
        }
    } else if (is_shutdown) {
        broadcast("NOW", msg);
    }

    return do_power(action, dry);
}
