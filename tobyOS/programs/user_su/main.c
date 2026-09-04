/* user_su -- su, native (2026-09-04, native-parity batch). Staged 4755 like
 * sudo: it must verify a password through ABI_SYS_AUTH_CHECK (root only)
 * and then become the target user. passwd(1) is deliberately NOT in this
 * binary: hard links share the setuid bit, and a setuid passwd would let the
 * kernel see uid 0 and skip the old-password check.
 *
 *   su [-] [-l] [-c CMD] [-s SHELL] [-p] [USER]
 *
 * Root needs no password. Anyone else is asked for the TARGET user's
 * password ("Password: ", echo off). `-`/`-l` gives a login shell: cd to
 * the target's home, HOME/USER/LOGNAME/SHELL set, argv[0] "-tsh". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/stat.h>
#include <tobyos/abi/abi.h>

static long sc2(long n, long a, long b) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}

struct pw { char name[64]; int uid, gid; char home[256]; char shell[128]; };

static int pw_lookup(const char *want_name, int want_uid, struct pw *out) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *fld[8]; int k = 0; fld[k++] = line;
        for (char *p = line; *p && k < 8; p++) if (*p == ':') { *p = '\0'; fld[k++] = p + 1; }
        if (k < 7) continue;
        int uid = atoi(fld[2]);
        if (want_name ? strcmp(fld[0], want_name) != 0 : uid != want_uid) continue;
        snprintf(out->name, sizeof out->name, "%s", fld[0]);
        out->uid = uid; out->gid = atoi(fld[3]);
        snprintf(out->home, sizeof out->home, "%s", fld[5]);
        char *sh = fld[6]; sh[strcspn(sh, "\n")] = '\0';
        snprintf(out->shell, sizeof out->shell, "%s", sh);
        fclose(f);
        return 0;
    }
    fclose(f);
    return -1;
}

static int read_password(char *out, size_t cap) {
    struct termios saved, raw;
    int tty = isatty(0);
    if (tty && tcgetattr(0, &saved) == 0) {
        raw = saved;
        raw.c_lflag &= (unsigned)~ECHO;
        tcsetattr(0, TCSANOW, &raw);
    }
    /* the prompt only makes sense on a terminal; a piped password must
     * not leave "Password: " glued to the next line of output */
    if (tty) { fputs("Password: ", stderr); fflush(stderr); }
    int ok = fgets(out, (int)cap, stdin) != NULL;
    if (tty) { tcsetattr(0, TCSANOW, &saved); fputc('\n', stderr); }
    if (!ok) return 0;
    out[strcspn(out, "\r\n")] = '\0';
    return 1;
}

int main(int argc, char **argv) {
    int login = 0, preserve = 0;
    const char *cmd = NULL, *shell = NULL, *target = NULL;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-")) { login = 1; continue; }
        if (!strcmp(a, "-l") || !strcmp(a, "--login")) { login = 1; continue; }
        if (!strcmp(a, "-p") || !strcmp(a, "-m") || !strcmp(a, "--preserve-environment")) { preserve = 1; continue; }
        if (!strcmp(a, "-c") && i + 1 < argc) { cmd = argv[++i]; continue; }
        if (!strncmp(a, "--command=", 10)) { cmd = a + 10; continue; }
        if (!strcmp(a, "-s") && i + 1 < argc) { shell = argv[++i]; continue; }
        if (!strncmp(a, "--shell=", 8)) { shell = a + 8; continue; }
        if (!strcmp(a, "--help")) { printf("Usage: su [options] [-] [USER [ARG]...]\n\n  -c, --command COMMAND   pass COMMAND to the invoked shell\n  -l, --login             make the shell a login shell\n  -p, --preserve-environment\n  -s, --shell SHELL       run SHELL if /etc/shells allows it\n"); return 0; }
        if (!strcmp(a, "--version")) { printf("su (tobyOS) 1.0\n"); return 0; }
        if (a[0] == '-' && a[1]) { fprintf(stderr, "su: invalid option -- '%c'\n", a[1]); return 1; }
        target = a; i++;
        break;
    }
    if (!target) target = "root";
    struct pw t;
    if (pw_lookup(target, 0, &t)) { fprintf(stderr, "su: user %s does not exist\n", target); return 1; }
    /* the REAL uid, not getuid()'s EFFECTIVE one -- su is setuid-root, so
     * getuid() already reads 0 and would skip the password for everyone */
    int ruid = (int)sc2(ABI_SYS_GETRUID, 0, 0);
    if (ruid != 0) {
        char pw[128];
        if (!read_password(pw, sizeof pw)) { fprintf(stderr, "su: Authentication failure\n"); return 1; }
        long rc = sc2(ABI_SYS_AUTH_CHECK, (long)(uintptr_t)t.name, (long)(uintptr_t)pw);
        memset(pw, 0, sizeof pw);
        if (rc != 0) { fprintf(stderr, "su: Authentication failure\n"); return 1; }
    }
    /* group first: setuid() drops the right to setgid() */
    if (setgid(t.gid) != 0 && t.gid != 0) { fprintf(stderr, "su: cannot set groups: %s\n", strerror(errno)); return 1; }
    if (setuid(t.uid) != 0) { fprintf(stderr, "su: cannot set user id: %s\n", strerror(errno)); return 1; }
    if ((int)getuid() != t.uid) { fprintf(stderr, "su: cannot set user id\n"); return 1; }
    const char *sh = shell ? shell : (t.shell[0] ? t.shell : "/bin/tsh");
    struct stat st;
    if (stat(sh, &st) != 0) sh = "/bin/tsh";
    if (login) {
        if (chdir(t.home) != 0) fprintf(stderr, "su: warning: cannot change directory to %s: %s\n", t.home, strerror(errno));
        setenv("HOME", t.home, 1);
        setenv("USER", t.name, 1);
        setenv("LOGNAME", t.name, 1);
        setenv("SHELL", sh, 1);
        setenv("PATH", t.uid == 0 ? "/usr/sbin:/usr/bin:/sbin:/bin" : "/usr/bin:/bin", 1);
    } else if (!preserve) {
        setenv("USER", t.name, 1);
        setenv("LOGNAME", t.name, 1);
        setenv("HOME", t.home, 1);
        setenv("SHELL", sh, 1);
    }
    char argv0[128];
    const char *base = sh; for (const char *p = sh; *p; p++) if (*p == '/') base = p + 1;
    snprintf(argv0, sizeof argv0, "%s%s", login ? "-" : "", base);
    char *av[64]; int n = 0;
    av[n++] = argv0;
    if (cmd) { av[n++] = (char *)"-c"; av[n++] = (char *)cmd; }
    for (; i < argc && n < 62; i++) av[n++] = argv[i];
    av[n] = NULL;
    execv(sh, av);
    fprintf(stderr, "su: failed to execute %s: %s\n", sh, strerror(errno));
    return 127;
}
