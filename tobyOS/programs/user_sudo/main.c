/* sudo -- run a command as another user, subject to /etc/sudoers.
 *
 *     sudo [-u user] [-n] [-k] COMMAND [ARG ...]
 *     sudo -l                              list what you may run
 *     sudo -v                              refresh the timestamp (no-op here)
 *
 * This is a REAL setuid-root program: it is staged mode 4755 and the kernel
 * honours S_ISUID on exec (and ignores it on a nosuid mount). That is what
 * makes the whole thing work, and also what makes every decision below
 * matter -- a bug here is a root shell.
 *
 * The rules that hold:
 *
 *   * The REAL uid identifies the caller. exec's setuid transition sets the
 *     effective uid and leaves the real one alone precisely so a setuid
 *     program can still tell who invoked it; reading the effective uid
 *     here would mean every caller looked like root.
 *   * A caller with no matching sudoers rule is refused, and the refusal is
 *     the default -- the parser starts from "no" and only a rule that
 *     matches makes it "yes".
 *   * A rule without NOPASSWD demands the caller's own password, checked
 *     through ABI_SYS_AUTH_CHECK, which verifies and nothing else. There is
 *     no ticket file: every invocation authenticates. That is stricter than
 *     real sudo's 15-minute timestamp, and it is stricter on purpose --
 *     a timestamp file is state this system has nowhere durable to keep.
 *
 * /etc/sudoers syntax understood:
 *     root    ALL=(ALL) ALL
 *     %wheel  ALL=(ALL) ALL
 *     toby    ALL=(ALL) NOPASSWD: ALL
 *     # comments and blank lines
 * Host and runas fields are parsed and required to be ALL; anything else is
 * REFUSED rather than approximated, because a rule this program half-reads
 * is a rule that does not mean what it says.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <tobyos/abi/abi.h>

#define SUDOERS "/etc/sudoers"

static long sc2(long n, long a, long b) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b)
                     : "rcx", "r11", "memory");
    return r;
}

static int auth_check(const char *user, const char *pass) {
    return (int)sc2(ABI_SYS_AUTH_CHECK, (long)(uintptr_t)user, (long)(uintptr_t)pass);
}

/* ---- the identity of the caller --------------------------------------- */

static int name_of_uid(int uid, char *out, size_t cap) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char name[128];
        int u;
        /* name:x:uid:gid:... */
        if (sscanf(line, "%127[^:]:%*[^:]:%d:", name, &u) == 2 && u == uid) {
            snprintf(out, cap, "%s", name);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static int uid_of_name(const char *name, int *uid_out, int *gid_out) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        char n[128];
        int u, g;
        if (sscanf(line, "%127[^:]:%*[^:]:%d:%d:", n, &u, &g) == 3 && !strcmp(n, name)) {
            *uid_out = u;
            *gid_out = g;
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static int in_group(const char *user, const char *group) {
    FILE *f = fopen("/etc/group", "r");
    if (!f) return 0;
    char line[1024];
    int yes = 0;
    while (fgets(line, sizeof line, f)) {
        char g[128], members[768];
        members[0] = 0;
        /* group:x:gid:m1,m2,... -- the member list may be empty */
        if (sscanf(line, "%127[^:]:%*[^:]:%*d:%767[^\n]", g, members) < 1) continue;
        if (strcmp(g, group)) continue;
        char *p = members;
        while (*p) {
            char *e = p;
            while (*e && *e != ',' && *e != '\n' && *e != '\r') e++;
            char save = *e;
            *e = 0;
            if (!strcmp(p, user)) { yes = 1; break; }
            *e = save;
            if (!*e) break;
            p = e + 1;
        }
        break;
    }
    fclose(f);
    return yes;
}

/* ---- sudoers ----------------------------------------------------------- */

struct rule { int matched; int nopasswd; };

/* Returns 1 if `user` has a rule, filling *nopasswd. The scan does NOT stop
 * at the first match: later rules override earlier ones, which is how
 * sudoers has always worked and how a NOPASSWD exception after a general
 * rule is meant to take effect. */
static int sudoers_lookup(const char *user, int *nopasswd, char *why, size_t whycap) {
    FILE *f = fopen(SUDOERS, "r");
    if (!f) {
        snprintf(why, whycap, "%s is missing", SUDOERS);
        return 0;
    }
    char line[1024];
    int matched = 0;
    *nopasswd = 0;
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char who[128], hostspec[256];
        if (sscanf(line, "%127s %255[^\n]", who, hostspec) != 2) continue;

        int applies = 0;
        if (who[0] == '%')      applies = in_group(user, who + 1);
        else                    applies = !strcmp(who, user);
        if (!applies) continue;

        /* hostspec is "ALL=(ALL) [NOPASSWD:] ALL". Anything whose host or
         * runas field is not ALL is refused rather than guessed at. */
        char host[64], runas[64], rest[192];
        rest[0] = 0;
        if (sscanf(hostspec, "%63[^=]=(%63[^)]) %191[^\n]", host, runas, rest) != 3)
            continue;
        /* trim trailing spaces on host */
        for (int i = (int)strlen(host) - 1; i >= 0 && host[i] == ' '; i--) host[i] = 0;
        if (strcmp(host, "ALL") || strcmp(runas, "ALL")) {
            snprintf(why, whycap,
                     "a rule for %s names host '%s' runas '%s'; only ALL is "
                     "understood here", user, host, runas);
            continue;
        }
        char *cmds = rest;
        int np = 0;
        while (*cmds == ' ') cmds++;
        if (!strncmp(cmds, "NOPASSWD:", 9)) { np = 1; cmds += 9; while (*cmds == ' ') cmds++; }
        else if (!strncmp(cmds, "PASSWD:", 7)) { cmds += 7; while (*cmds == ' ') cmds++; }
        /* Only the ALL command set is understood. A per-command rule that
         * this parser silently widened to ALL would be a privilege
         * escalation written by a typo. */
        for (int i = (int)strlen(cmds) - 1; i >= 0 && (cmds[i] == ' ' || cmds[i] == '\r'); i--)
            cmds[i] = 0;
        if (strcmp(cmds, "ALL")) {
            snprintf(why, whycap,
                     "a rule for %s lists commands '%s'; only ALL is "
                     "understood here, so the rule is ignored", user, cmds);
            continue;
        }
        matched = 1;
        *nopasswd = np;
    }
    fclose(f);
    return matched;
}

/* ---- the password prompt ----------------------------------------------- */

static int read_password(const char *user, char *out, size_t cap) {
    struct termios old, raw;
    int have_tty = (tcgetattr(0, &old) == 0);
    printf("[sudo] password for %s: ", user);
    fflush(stdout);
    if (have_tty) {
        raw = old;
        raw.c_lflag &= (unsigned)~ECHO;
        tcsetattr(0, TCSANOW, &raw);
    }
    int ok = fgets(out, (int)cap, stdin) != NULL;
    if (have_tty) { tcsetattr(0, TCSANOW, &old); printf("\n"); }
    if (!ok) return 0;
    char *nl = strpbrk(out, "\r\n");
    if (nl) *nl = 0;
    return 1;
}

int main(int argc, char **argv) {
    const char *target = "root";
    int nonint = 0, list = 0, validate = 0, first = argc;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { first = i + 1; break; }
        if (!strcmp(a, "-u") && i + 1 < argc) { target = argv[++i]; continue; }
        if (!strcmp(a, "-n")) { nonint = 1; continue; }
        if (!strcmp(a, "-l") || !strcmp(a, "--list")) { list = 1; continue; }
        if (!strcmp(a, "-v")) { validate = 1; continue; }
        if (!strcmp(a, "-k") || !strcmp(a, "-K")) {
            /* There is no timestamp file to invalidate: every invocation
             * authenticates. Succeed silently, as sudo -k does. */
            return 0;
        }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            printf("usage: sudo [-u user] [-n] COMMAND [ARG ...]\n"
                   "       sudo -l\n");
            return 0;
        }
        if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
            printf("sudo (tobyOS) 1.0\n");
            return 0;
        }
        if (a[0] == '-' && a[1]) { fprintf(stderr, "sudo: unknown option %s\n", a); return 1; }
        first = i;
        break;
    }

    int ruid = (int)getuid();
    char caller[128];
    if (!name_of_uid(ruid, caller, sizeof caller))
        snprintf(caller, sizeof caller, "#%d", ruid);

    int nopasswd = 0;
    char why[256] = "";
    int allowed = sudoers_lookup(caller, &nopasswd, why, sizeof why);

    /* Root is always allowed, the way sudo has always treated uid 0 --
     * there is nothing to escalate to. */
    if (ruid == 0) { allowed = 1; nopasswd = 1; }

    if (list) {
        if (!allowed) {
            printf("User %s is not allowed to run sudo on this host.\n", caller);
            if (why[0]) printf("sudo: %s\n", why);
            return 1;
        }
        printf("User %s may run the following commands on this host:\n", caller);
        printf("    (ALL) %sALL\n", nopasswd ? "NOPASSWD: " : "");
        return 0;
    }

    if (!allowed) {
        fprintf(stderr, "%s is not in the sudoers file. This incident will be "
                        "reported.\n", caller);
        if (why[0]) fprintf(stderr, "sudo: %s\n", why);
        return 1;
    }

    if (!nopasswd) {
        if (nonint) {
            fprintf(stderr, "sudo: a password is required\n");
            return 1;
        }
        char pw[128];
        int tries = 0;
        for (;;) {
            if (!read_password(caller, pw, sizeof pw)) {
                fprintf(stderr, "sudo: no password given\n");
                return 1;
            }
            int rc = auth_check(caller, pw);
            memset(pw, 0, sizeof pw);          /* do not leave it in memory */
            if (rc == 0) break;
            if (rc == -ABI_EPERM) {
                fprintf(stderr, "sudo: cannot verify a password -- this binary "
                                "is not setuid root, so the check is refused\n");
                return 1;
            }
            if (++tries >= 3) {
                fprintf(stderr, "sudo: %d incorrect password attempts\n", tries);
                return 1;
            }
            fprintf(stderr, "Sorry, try again.\n");
        }
    }

    if (validate) return 0;

    if (first >= argc) {
        fprintf(stderr, "usage: sudo [-u user] [-n] COMMAND [ARG ...]\n");
        return 1;
    }

    int tuid = 0, tgid = 0;
    if (strcmp(target, "root") && !uid_of_name(target, &tuid, &tgid)) {
        fprintf(stderr, "sudo: unknown user: %s\n", target);
        return 1;
    }

    /* GID FIRST. setuid() drops the ability to call setgid(), so doing it
     * the other way round leaves the process running with the invoker's
     * group -- privilege that was supposed to change and did not. */
    if (setgid(tgid) != 0 && tgid != 0) {
        fprintf(stderr, "sudo: cannot set gid %d: %s\n", tgid, strerror(errno));
        return 1;
    }
    if (setuid(tuid) != 0) {
        fprintf(stderr, "sudo: cannot set uid %d: %s\n", tuid, strerror(errno));
        return 1;
    }
    /* Verify rather than assume: if the drop silently failed we would run
     * the command as root having told the user it was someone else. */
    if ((int)getuid() != tuid) {
        fprintf(stderr, "sudo: the uid change did not take effect\n");
        return 1;
    }

    execvp(argv[first], &argv[first]);
    fprintf(stderr, "sudo: %s: command not found\n", argv[first]);
    return 127;
}
