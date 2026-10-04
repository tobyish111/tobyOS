/* setsid -- run a program in a new session, as util-linux's setsid does.
 *
 *     setsid [-c] [-f] [-w] program [argument ...]
 *
 * The program runs as the leader of a new session and process group with
 * no controlling terminal (setsid(2)) -- so /dev/tty is ENXIO for it, and
 * an interactive shell started this way says it has no job control, as on
 * Linux. A process-group leader cannot call setsid, so when this one is
 * (every process pid 0 starts is) or -f asks, it forks and the child does
 * it; the parent then exits 0, or with -w waits and exits with the
 * program's status. -c makes the terminal on standard input the new
 * session's controlling terminal (TIOCSCTTY). tobyOS had no setsid
 * command and no setsid(2) for native programs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>

#define TIOCSCTTY 0x540Eul

static void usage(FILE *o) {
    fprintf(o, "Usage:\n setsid [options] <program> [arguments ...]\n");
}

int main(int argc, char **argv) {
    int ctty = 0, forcefork = 0, waitp = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) { i++; break; }
        if (a[0] != '-' || !a[1]) break;
        if (strcmp(a, "--ctty") == 0) { ctty = 1; continue; }
        if (strcmp(a, "--fork") == 0) { forcefork = 1; continue; }
        if (strcmp(a, "--wait") == 0) { waitp = 1; continue; }
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { usage(stdout); return 0; }
        if (a[1] == '-') {
            fprintf(stderr, "setsid: unrecognized option '%s'\n", a);
            usage(stderr);
            return 1;
        }
        for (const char *c = a + 1; *c; c++) {
            if (*c == 'c') ctty = 1;
            else if (*c == 'f') forcefork = 1;
            else if (*c == 'w') waitp = 1;
            else {
                fprintf(stderr, "setsid: invalid option -- '%c'\n", *c);
                usage(stderr);
                return 1;
            }
        }
    }
    if (i >= argc) {
        fprintf(stderr, "setsid: no command specified\n");
        usage(stderr);
        return 1;
    }

    if (forcefork || getpgrp() == getpid()) {
        pid_t pid = fork();
        if (pid < 0) {
            fprintf(stderr, "setsid: fork failed: %s\n", strerror(errno));
            return 1;
        }
        if (pid > 0) {
            if (!waitp) return 0;
            int st = 0;
            if (waitpid(pid, &st, 0) < 0) {
                fprintf(stderr, "setsid: waitpid failed: %s\n", strerror(errno));
                return 1;
            }
            if (WIFEXITED(st)) return WEXITSTATUS(st);
            return 1;
        }
    }
    if (setsid() < 0) {
        fprintf(stderr, "setsid: setsid failed: %s\n", strerror(errno));
        return 1;
    }
    if (ctty && ioctl(0, TIOCSCTTY, (void *)1) != 0) {
        fprintf(stderr, "setsid: failed to set the controlling terminal: %s\n",
                strerror(errno));
        return 1;
    }
    execvp(argv[i], &argv[i]);
    fprintf(stderr, "setsid: failed to execute %s: %s\n", argv[i], strerror(errno));
    return errno == ENOENT ? 127 : 126;
}
