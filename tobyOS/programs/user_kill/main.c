/* kill -- send a signal to a process.
 *
 * POSIX XCU:  kill -s SIGNAL pid...
 *             kill -SIGNAL  pid...
 *             kill -l [status...]
 *
 * There was no /bin/kill at all, which is not a gap a shell can paper over:
 * `sh -c "kill -USR1 $$"` is how a script signals itself from a child, and
 * with no such program every shell in the image reported 127 instead. It also
 * made the conformance oracle wrong -- the real bash could not deliver a
 * signal it was being tested on.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static const struct { int num; const char *name; } g_sigs[] = {
    { 1,  "HUP"  }, { 2,  "INT"  }, { 3,  "QUIT" }, { 4,  "ILL"  },
    { 5,  "TRAP" }, { 6,  "ABRT" }, { 7,  "BUS"  }, { 8,  "FPE"  },
    { 9,  "KILL" }, { 10, "USR1" }, { 11, "SEGV" }, { 12, "USR2" },
    { 13, "PIPE" }, { 14, "ALRM" }, { 15, "TERM" }, { 17, "CHLD" },
    { 18, "CONT" }, { 19, "STOP" }, { 20, "TSTP" }, { 21, "TTIN" },
    { 22, "TTOU" }, { 23, "URG"  }, { 24, "XCPU" }, { 25, "XFSZ" },
    { 26, "VTALRM" }, { 27, "PROF" }, { 28, "WINCH" }, { 29, "IO" },
    { 30, "PWR" }, { 31, "SYS" },
};
#define NSIGS ((int)(sizeof g_sigs / sizeof g_sigs[0]))

static int sig_by_name(const char *s) {
    if (!s || !*s) return -1;
    if (strncmp(s, "SIG", 3) == 0) s += 3;
    for (int i = 0; i < NSIGS; i++)
        if (strcmp(g_sigs[i].name, s) == 0) return g_sigs[i].num;
    /* A bare number is also a valid name here: `kill -s 9`. */
    int v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        v = v * 10 + (*p - '0');
    }
    return v;
}

static const char *name_by_sig(int n) {
    for (int i = 0; i < NSIGS; i++)
        if (g_sigs[i].num == n) return g_sigs[i].name;
    return 0;
}

static int do_list(int argc, char **argv, int first) {
    if (first >= argc) {
        for (int i = 0; i < NSIGS; i++)
            printf("%2d) SIG%-7s%s", g_sigs[i].num, g_sigs[i].name,
                   (i % 4 == 3) ? "\n" : " ");
        if (NSIGS % 4 != 0) printf("\n");
        return 0;
    }
    int rc = 0;
    for (int i = first; i < argc; i++) {
        int v = atoi(argv[i]);
        /* A wait status carries the signal in its low bits. */
        if (v > 128) v -= 128;
        const char *nm = name_by_sig(v);
        if (nm) printf("%s\n", nm);
        else { fprintf(stderr, "kill: %s: invalid signal\n", argv[i]); rc = 1; }
    }
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: kill [-s SIGNAL | -SIGNAL] pid...\n");
        return 2;
    }

    int sig = SIGTERM;
    int first = 1;

    if (strcmp(argv[1], "-l") == 0 || strcmp(argv[1], "-L") == 0)
        return do_list(argc, argv, 2);

    if (strcmp(argv[1], "-s") == 0) {
        if (argc < 3) { fprintf(stderr, "kill: -s needs a signal\n"); return 2; }
        sig = sig_by_name(argv[2]);
        if (sig < 0) {
            fprintf(stderr, "kill: %s: invalid signal specification\n", argv[2]);
            return 1;
        }
        first = 3;
    } else if (argv[1][0] == '-' && argv[1][1]) {
        sig = sig_by_name(argv[1] + 1);
        if (sig < 0) {
            fprintf(stderr, "kill: %s: invalid signal specification\n",
                    argv[1] + 1);
            return 1;
        }
        first = 2;
    }

    if (first >= argc) {
        fprintf(stderr, "usage: kill [-s SIGNAL | -SIGNAL] pid...\n");
        return 2;
    }

    int rc = 0;
    for (int i = first; i < argc; i++) {
        char *end = argv[i];
        int neg = 0;
        if (*end == '-') { neg = 1; end++; }
        int pid = 0;
        int any = 0;
        for (; *end; end++) {
            if (*end < '0' || *end > '9') { any = -1; break; }
            pid = pid * 10 + (*end - '0');
            any = 1;
        }
        if (any != 1) {
            fprintf(stderr, "kill: %s: arguments must be process IDs\n", argv[i]);
            rc = 1;
            continue;
        }
        if (kill(neg ? -pid : pid, sig) != 0) {
            fprintf(stderr, "kill: (%d) - no such process\n", pid);
            rc = 1;
        }
    }
    return rc;
}
