/* user_moreutils -- init, passwd and chpasswd (2026-09-04, native-parity
 * batch; one binary dispatched on argv[0]).
 *
 * passwd and chpasswd follow shadow-utils'. The rest of what was here -- arch chgrp
 * factor false link logname nproc printf pwd shred sum test [ true unlink
 * uptime users who hostid -- is user_shellutils' now, each measured
 * against GNU coreutils or procps (logs/utilgate/, finding 151). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <tobyos/abi/abi.h>

static const char *g_prog = "moreutils";

static long sc3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}


static int pw_name_of(int uid, char *name, size_t cap) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *fld[8]; int k = 0; fld[k++] = line;
        for (char *p = line; *p && k < 8; p++) if (*p == ':') { *p = '\0'; fld[k++] = p + 1; }
        if (k < 4) continue;
        if (atoi(fld[2]) == uid) { snprintf(name, cap, "%s", fld[0]); fclose(f); return 0; }
    }
    fclose(f);
    return -1;
}


/* ================================================================= init */
static int cmd_init(int argc, char **argv) {
    (void)argc; (void)argv;
    if (getpid() != 1) { fprintf(stderr, "init: must be run as PID 1\n"); return 1; }
    /* tobyOS boots from an in-kernel init; a userspace PID 1 never happens.
     * If it ever did, there is nothing sensible to run: say so and idle. */
    fprintf(stderr, "init: tobyOS's init is in-kernel; nothing to do\n");
    for (;;) sleep(3600);
}

/* =============================================================== passwd */
static int read_pw(const char *prompt, char *out, size_t cap) {
    struct termios saved, raw; int tty = isatty(0);
    if (tty && tcgetattr(0, &saved) == 0) { raw = saved; raw.c_lflag &= (unsigned)~ECHO; tcsetattr(0, TCSANOW, &raw); }
    fputs(prompt, stderr);
    int ok = fgets(out, (int)cap, stdin) != NULL;
    if (tty) { tcsetattr(0, TCSANOW, &saved); }
    fputc('\n', stderr);
    if (!ok) return 0;
    out[strcspn(out, "\r\n")] = '\0';
    return 1;
}

/* passwd as shadow-utils' answers it (finding 213; measured in WSL as a
 * user, and as root with -R on a throwaway root, logs/b33/pwmeasure*):
 *   passwd [-k] [-q] [-s] [LOGIN]     change the password (LOGIN's: root)
 *   passwd -S [-a] [LOGIN]            "NAME STATE DATE MIN MAX WARN INACTIVE":
 *       STATE P, NP (no password) or L (locked); DATE the last change,
 *       YYYY-MM-DD, "never" when there is none; an empty number -1. -a:
 *       every account /etc/passwd has (root's alone). Another's: root's.
 *   passwd {-l|-u|-d|-e|-n N|-x N|-w N|-i N}... LOGIN    root's alone,
 *       "passwd: Permission denied." for anyone else: -l puts a '!' before
 *       the hash, -u takes it off (status 3 and two lines if that would
 *       leave no password), -d empties it, -e makes the last change 0 (a
 *       change at the next login), -n -x -w -i set the aging days (-1
 *       empties one). Each says "passwd: password changed." unless -q.
 *   - Without LOGIN a changing option is the usage, status 2; so is -a
 *     without -S, and -S with a changing option. A number below -1 or not
 *     one: "passwd: invalid numeric argument 'X'" and the usage, status 6.
 *     No such account: "passwd: user 'X' does not exist", status 1.
 *   - -k changes only an expired password; -s reads the new one from
 *     standard input. -r, -R, -P: the accounts are the kernel's, not a
 *     directory's -- refused.
 * The kernel keeps the account table (ABI_SYS_SHADOW). */
static void pw_usage(FILE *o) {
    fputs("Usage: passwd [options] [LOGIN]\n"
          "\n"
          "Options:\n"
          "  -a, --all            with -S, every account\n"
          "  -d, --delete         leave LOGIN with no password\n"
          "  -e, --expire         make LOGIN change the password at the next login\n"
          "  -h, --help           print this help and exit\n"
          "  -k, --keep-tokens    change the password only if it has expired\n"
          "  -i, --inactive DAYS  the days after expiry before the account is shut\n"
          "  -l, --lock           lock LOGIN's password\n"
          "  -n, --mindays DAYS   the fewest days between changes\n"
          "  -q, --quiet          say nothing when it works\n"
          "  -r, --repository R   the account store (not kept here)\n"
          "  -R, --root DIR       the accounts under DIR (not kept here)\n"
          "  -P, --prefix DIR     the account files under DIR (not kept here)\n"
          "  -S, --status         say LOGIN's password state\n"
          "  -u, --unlock         unlock LOGIN's password\n"
          "  -w, --warndays DAYS  the days of warning before expiry\n"
          "  -x, --maxdays DAYS   the most days between changes\n"
          "  -s, --stdin          read the new password from standard input\n"
          "\n", o);
}

/* YYYY-MM-DD of DAYS since 1970 */
static void pw_date(long long days, char *out, size_t cap) {
    long long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    long long doe = z - era * 146097;
    long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long long mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1;
    long long m = mp < 10 ? mp + 3 : mp - 9;
    snprintf(out, cap, "%04lld-%02lld-%02lld", y + (m <= 2), m, d);
}

static int pw_status(const char *name) {
    struct abi_shadow sh;
    long r = sc3(ABI_SYS_SHADOW, ABI_SHADOW_GET, (long)(uintptr_t)name, (long)(uintptr_t)&sh);
    if (r == -ABI_ENOENT) { fprintf(stderr, "passwd: user '%s' does not exist\n", name); return 1; }
    if (r == -ABI_EPERM) { fprintf(stderr, "passwd: You may not view or modify password information for %s.\n", name); return 1; }
    if (r != 0) { fprintf(stderr, "passwd: Authentication token manipulation error\n"); return 1; }
    char date[16];
    if (sh.lastchg < 0) snprintf(date, sizeof date, "never");
    else pw_date(sh.lastchg, date, sizeof date);
    printf("%s %s %s %lld %lld %lld %lld\n", name,
           sh.state == 2 ? "L" : sh.state == 1 ? "P" : "NP", date,
           (long long)sh.minage, (long long)sh.maxage, (long long)sh.warn,
           (long long)sh.inactive);
    return 0;
}

/* a day count as shadow reads one: -1 or more, else status 6 */
static long long pw_days(const char *v) {
    char *e;
    long long n = strtoll(v, &e, 10);
    if (!*v || *e || n < -1) {
        fprintf(stderr, "passwd: invalid numeric argument '%s'\n", v);
        pw_usage(stderr);
        exit(6);
    }
    return n;
}

static int cmd_passwd(int argc, char **argv) {
    int all = 0, del = 0, exp_ = 0, keep = 0, lock = 0, quiet = 0, stat_ = 0,
        unlock = 0, from_stdin = 0;
    long long nmin = -2, nmax = -2, nwarn = -2, ninact = -2;
    const char *target = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) {
            if (target) { pw_usage(stderr); return 2; }
            target = a;
            continue;
        }
        if (!strcmp(a, "--")) {
            if (i + 1 < argc) target = argv[i + 1];
            if (i + 2 < argc) { pw_usage(stderr); return 2; }
            break;
        }
        static const struct { const char *l; char s; int arg; } opts[] = {
            { "--all", 'a', 0 }, { "--delete", 'd', 0 }, { "--expire", 'e', 0 },
            { "--help", 'h', 0 }, { "--keep-tokens", 'k', 0 }, { "--inactive", 'i', 1 },
            { "--lock", 'l', 0 }, { "--mindays", 'n', 1 }, { "--quiet", 'q', 0 },
            { "--repository", 'r', 1 }, { "--root", 'R', 1 }, { "--prefix", 'P', 1 },
            { "--status", 'S', 0 }, { "--unlock", 'u', 0 }, { "--warndays", 'w', 1 },
            { "--maxdays", 'x', 1 }, { "--stdin", 's', 0 }, { 0, 0, 0 } };
        char cl[64];
        const char *rest = 0;
        if (a[1] == '-') {
            const char *eq = strchr(a, '=');
            size_t n = eq ? (size_t)(eq - a) : strlen(a);
            int k = 0;
            while (opts[k].l && (strlen(opts[k].l) != n || strncmp(opts[k].l, a, n))) k++;
            if (!opts[k].l) {
                fprintf(stderr, "passwd: unrecognized option '%s'\n", a);
                pw_usage(stderr);
                return 2;
            }
            cl[0] = opts[k].s; cl[1] = 0;
            if (opts[k].arg) {
                if (eq) rest = eq + 1;
                else if (i + 1 < argc) rest = argv[++i];
                else {
                    fprintf(stderr, "passwd: option '%s' requires an argument\n", a);
                    pw_usage(stderr);
                    return 2;
                }
            }
        } else {
            snprintf(cl, sizeof cl, "%s", a + 1);
        }
        for (const char *c = cl; *c; c++) {
            int k = 0;
            while (opts[k].l && opts[k].s != *c) k++;
            if (!opts[k].l) {
                fprintf(stderr, "passwd: invalid option -- '%c'\n", *c);
                pw_usage(stderr);
                return 2;
            }
            const char *v = rest;
            if (opts[k].arg && !v) {
                if (c[1]) { v = a + (c - cl) + 2; }
                else if (i + 1 < argc) v = argv[++i];
                else {
                    fprintf(stderr, "passwd: option requires an argument -- '%c'\n", *c);
                    pw_usage(stderr);
                    return 2;
                }
            }
            switch (*c) {
            case 'a': all = 1; break;
            case 'd': del = 1; break;
            case 'e': exp_ = 1; break;
            case 'h': pw_usage(stdout); return 0;
            case 'k': keep = 1; break;
            case 'i': ninact = pw_days(v); break;
            case 'l': lock = 1; break;
            case 'n': nmin = pw_days(v); break;
            case 'q': quiet = 1; break;
            case 'S': stat_ = 1; break;
            case 'u': unlock = 1; break;
            case 'w': nwarn = pw_days(v); break;
            case 'x': nmax = pw_days(v); break;
            case 's': from_stdin = 1; break;
            case 'r': case 'R': case 'P':
                fprintf(stderr, "passwd: -%c is not supported: the accounts are the "
                        "kernel's, not a directory's\n", *c);
                return 1;
            }
            if (opts[k].arg) break;            /* the rest was its argument */
        }
    }
    int uid = (int)getuid();
    char me[64];
    if (pw_name_of(uid, me, sizeof me)) snprintf(me, sizeof me, "uid%d", uid);
    int change = del || exp_ || lock || unlock || nmin != -2 || nmax != -2 ||
                 nwarn != -2 || ninact != -2;
    if ((all && !stat_) || (stat_ && change) || (change && !target)) {
        pw_usage(stderr);
        return 2;
    }
    if (stat_) {
        if (all) {
            if (uid != 0) { fprintf(stderr, "passwd: Permission denied.\n"); return 1; }
            FILE *f = fopen("/etc/passwd", "r");
            char line[512];
            int rc = 0;
            while (f && fgets(line, sizeof line, f)) {
                char *c = strchr(line, ':');
                if (!c) continue;
                *c = 0;
                rc |= pw_status(line);
            }
            if (f) fclose(f);
            return rc;
        }
        return pw_status(target ? target : me);
    }
    if (change) {
        if (uid != 0) { fprintf(stderr, "passwd: Permission denied.\n"); return 1; }
        long r = 0;
        if (del) r = sc3(ABI_SYS_SHADOW, ABI_SHADOW_DELETE, (long)(uintptr_t)target, 0);
        if (!r && lock) r = sc3(ABI_SYS_SHADOW, ABI_SHADOW_LOCK, (long)(uintptr_t)target, 0);
        if (!r && unlock) {
            r = sc3(ABI_SYS_SHADOW, ABI_SHADOW_UNLOCK, (long)(uintptr_t)target, 0);
            if (r == -ABI_EINVAL) {
                fprintf(stderr, "passwd: unlocking the password would result in a passwordless account.\n"
                        "You should set a password with usermod -p to unlock the password of this account.\n");
                return 3;
            }
        }
        if (!r && exp_) r = sc3(ABI_SYS_SHADOW, ABI_SHADOW_EXPIRE, (long)(uintptr_t)target, 0);
        if (!r && (nmin != -2 || nmax != -2 || nwarn != -2 || ninact != -2)) {
            struct abi_shadow sh;
            memset(&sh, 0, sizeof sh);
            sh.minage = nmin; sh.maxage = nmax; sh.warn = nwarn; sh.inactive = ninact;
            r = sc3(ABI_SYS_SHADOW, ABI_SHADOW_AGE, (long)(uintptr_t)target, (long)(uintptr_t)&sh);
        }
        if (r == -ABI_ENOENT) { fprintf(stderr, "passwd: user '%s' does not exist\n", target); return 1; }
        if (r == -ABI_EPERM) { fprintf(stderr, "passwd: Permission denied.\n"); return 1; }
        if (r != 0) { fprintf(stderr, "passwd: Authentication token manipulation error\n"); return 1; }
        if (!quiet) printf("passwd: password changed.\n");
        return 0;
    }
    if (!target) target = me;
    if (uid != 0 && strcmp(target, me)) { fprintf(stderr, "passwd: You may not view or modify password information for %s.\n", target); return 1; }
    if (keep) {                                  /* only an expired one */
        struct abi_shadow sh;
        if (sc3(ABI_SYS_SHADOW, ABI_SHADOW_GET, (long)(uintptr_t)target, (long)(uintptr_t)&sh) == 0 &&
            sh.lastchg != 0) return 0;
    }
    if (!quiet) fprintf(stderr, "Changing password for %s.\n", target);
    char old[128] = "", n1[128], n2[128];
    if (from_stdin) {
        if (!fgets(n1, sizeof n1, stdin)) { fprintf(stderr, "passwd: password unchanged\n"); return 1; }
        n1[strcspn(n1, "\r\n")] = '\0';
        snprintf(n2, sizeof n2, "%s", n1);
    } else {
        if (uid != 0) { if (!read_pw("Current password: ", old, sizeof old)) return 1; }
        if (!read_pw("New password: ", n1, sizeof n1)) return 1;
        if (!read_pw("Retype new password: ", n2, sizeof n2)) return 1;
    }
    if (strcmp(n1, n2)) { fprintf(stderr, "Sorry, passwords do not match.\npasswd: Authentication token manipulation error\npasswd: password unchanged\n"); return 1; }
    if (!n1[0]) { fprintf(stderr, "No password has been supplied.\npasswd: password unchanged\n"); return 1; }
    long rc = sc3(ABI_SYS_SETPASSWD, (long)(uintptr_t)target, (long)(uintptr_t)n1, (long)(uintptr_t)old);
    memset(old, 0, sizeof old); memset(n1, 0, sizeof n1); memset(n2, 0, sizeof n2);
    if (rc == -ABI_EACCES) { fprintf(stderr, "passwd: Authentication failure\npasswd: password unchanged\n"); return 1; }
    if (rc == -ABI_ENOENT) { fprintf(stderr, "passwd: user '%s' does not exist\n", target); return 1; }
    if (rc == -ABI_EPERM) { fprintf(stderr, "passwd: You may not view or modify password information for %s.\n", target); return 1; }
    if (rc != 0) { fprintf(stderr, "passwd: Authentication token manipulation error\npasswd: password unchanged\n"); return 1; }
    if (!quiet) fprintf(stderr, "passwd: password updated successfully\n");
    return 0;
}

/* ============================================================= chpasswd */
/* chpasswd as shadow-utils' answers it (finding 194): NAME:PASSWORD lines
 * on stdin, the password everything after the first colon. Every line is
 * checked before any is set, so one bad line changes none. It was
 * busybox's applet, which wrote an /etc/shadow that no login here reads:
 * the account it "changed" stayed locked. The password goes to the
 * kernel's account table as passwd's does; the kernel keeps one hash of
 * its own, so -c and -s are taken and checked, not obeyed. -e takes each
 * as a hash made elsewhere and keeps it as it is, as shadow-utils' does
 * -- a SHA-crypt one ($5$, $6$) logs in, the kernel checks those (finding
 * 213; it was refused). -c, -e and -m are exclusive. */
static void chp_usage(FILE *o) {
    fputs("Usage: chpasswd [options]\n"
          "\n"
          "Options:\n"
          "  -c, --crypt-method METHOD     hash with METHOD: NONE DES MD5 SHA256 SHA512 or\n"
          "                                YESCRYPT\n"
          "  -e, --encrypted               each password given is already a hash\n"
          "  -h, --help                    print this help and exit\n"
          "  -m, --md5                     hash with MD5 (as -c MD5)\n"
          "  -R, --root CHROOT_DIR         work on the accounts under CHROOT_DIR\n"
          "  -P, --prefix PREFIX_DIR       read the account files under PREFIX_DIR\n"
          "  -s, --sha-rounds              rounds for the SHA, BCRYPT and YESCRYPT\n"
          "                                methods\n"
          "\n", o);
}

static int chp_known(const char *name) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return 0;
    char line[512];
    size_t n = strlen(name);
    int hit = 0;
    while (!hit && fgets(line, sizeof line, f))
        hit = !strncmp(line, name, n) && line[n] == ':';
    fclose(f);
    return hit;
}

static int cmd_chpasswd(int argc, char **argv) {
    static const char *const methods[] = { "NONE", "DES", "MD5", "SHA256",
                                           "SHA512", "YESCRYPT", 0 };
    int crypt = 0, rounds = 0, enc = 0, md5 = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val = 0;
        int need = !strcmp(a, "-c") || !strcmp(a, "--crypt-method") ||
                   !strcmp(a, "-s") || !strcmp(a, "--sha-rounds") ||
                   !strcmp(a, "-R") || !strcmp(a, "--root") ||
                   !strcmp(a, "-P") || !strcmp(a, "--prefix");
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { chp_usage(stdout); return 0; }
        if (!strcmp(a, "-e") || !strcmp(a, "--encrypted")) { enc = 1; continue; }
        if (!strcmp(a, "-m") || !strcmp(a, "--md5")) { md5 = 1; continue; }
        if (a[0] != '-' || !a[1]) continue;          /* operands: ignored */
        if (!need) {
            if (a[1] == '-') fprintf(stderr, "chpasswd: unrecognized option '%s'\n", a);
            else fprintf(stderr, "chpasswd: invalid option -- '%c'\n", a[1]);
            chp_usage(stderr);
            return 2;
        }
        if (i + 1 >= argc) {
            if (a[1] == '-') fprintf(stderr, "chpasswd: option '%s' requires an argument\n", a);
            else fprintf(stderr, "chpasswd: option requires an argument -- '%c'\n", a[1]);
            chp_usage(stderr);
            return 2;
        }
        val = argv[++i];
        if (a[1] == 'c' || !strcmp(a, "--crypt-method")) {
            int k = 0;
            while (methods[k] && strcmp(methods[k], val)) k++;
            if (!methods[k]) {
                fprintf(stderr, "chpasswd: unsupported crypt method: %s\n", val);
                chp_usage(stderr);
                return 2;
            }
            crypt = 1;
        } else if (a[1] == 's' || !strcmp(a, "--sha-rounds")) {
            rounds = 1;
        } else {
            fprintf(stderr, "chpasswd: %s is not supported: the accounts are the "
                    "kernel's, not a directory's\n", a);
            return 1;
        }
    }
    if (crypt + enc + md5 > 1) {
        fprintf(stderr, "chpasswd: the -c, -e, and -m flags are exclusive\n");
        chp_usage(stderr);
        return 2;
    }
    if (rounds && !crypt) {
        fprintf(stderr, "chpasswd: -s flag is only allowed with the -c flag\n");
        chp_usage(stderr);
        return 2;
    }
    /* read every line, then check every line, then set */
    size_t cap = 16, n = 0;
    char **v = malloc(cap * sizeof *v);
    char line[4096];
    int bad = 0;
    while (v && fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = '\0';
        if (n == cap) {
            char **nv = realloc(v, (cap *= 2) * sizeof *v);
            if (!nv) break;
            v = nv;
        }
        v[n++] = strdup(line);
    }
    if (!v) { fprintf(stderr, "chpasswd: out of memory\n"); return 1; }
    for (size_t i = 0; i < n; i++) {
        char *c = v[i] ? strchr(v[i], ':') : 0;
        if (!c) {
            fprintf(stderr, "chpasswd: line %zu: missing new password\n", i + 1);
            bad = 1;
            continue;
        }
        *c = '\0';
        if (!chp_known(v[i])) {
            fprintf(stderr, "chpasswd: line %zu: user '%s' does not exist\n", i + 1, v[i]);
            bad = 1;
        }
        *c = ':';
    }
    if (bad) {
        fprintf(stderr, "chpasswd: error detected, changes ignored\n");
        return 1;
    }
    int rc = 0;
    for (size_t i = 0; i < n; i++) {
        char *c = strchr(v[i], ':');
        *c = '\0';
        long r = enc ? sc3(ABI_SYS_SHADOW, ABI_SHADOW_SETHASH, (long)(uintptr_t)v[i],
                           (long)(uintptr_t)(c + 1))
                     : sc3(ABI_SYS_SETPASSWD, (long)(uintptr_t)v[i], (long)(uintptr_t)(c + 1), 0);
        if (r != 0) {
            fprintf(stderr, "chpasswd: (line %zu, user %s) password not changed\n", i + 1, v[i]);
            rc = 1;
        }
        memset(c + 1, 0, strlen(c + 1));
        free(v[i]);
    }
    free(v);
    return rc;
}

/* ============================================================== dispatch */
struct applet { const char *name; int (*fn)(int, char **); };
static const struct applet g_applets[] = {
    { "init",    cmd_init },
    { "passwd",  cmd_passwd },
    { "chpasswd", cmd_chpasswd },
    { 0, 0 }
};

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "moreutils";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    g_prog = base;
    for (int i = 0; g_applets[i].name; i++)
        if (!strcmp(base, g_applets[i].name))
            return g_applets[i].fn(argc, argv);
    if (argc > 1) {
        for (int i = 0; g_applets[i].name; i++) {
            if (strcmp(argv[1], g_applets[i].name)) continue;
            g_prog = g_applets[i].name;
            return g_applets[i].fn(argc - 1, argv + 1);
        }
    }
    fprintf(stderr, "moreutils: applets:");
    for (int i = 0; g_applets[i].name; i++) fprintf(stderr, " %s", g_applets[i].name);
    fprintf(stderr, "\n");
    return 1;
}
