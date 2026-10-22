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

static int cmd_passwd(int argc, char **argv) {
    const char *target = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { printf("Usage: passwd [options] [LOGIN]\n\nOptions:\n  -h, --help   display this help message and exit\n"); return 0; }
        if (argv[i][0] == '-') { fprintf(stderr, "passwd: unrecognized option '%s'\n", argv[i]); return 1; }
        target = argv[i];
    }
    int uid = (int)getuid();
    char me[64];
    if (pw_name_of(uid, me, sizeof me)) snprintf(me, sizeof me, "uid%d", uid);
    if (!target) target = me;
    if (uid != 0 && strcmp(target, me)) { fprintf(stderr, "passwd: You may not view or modify password information for %s.\n", target); return 1; }
    fprintf(stderr, "Changing password for %s.\n", target);
    char old[128] = "", n1[128], n2[128];
    if (uid != 0) { if (!read_pw("Current password: ", old, sizeof old)) return 1; }
    if (!read_pw("New password: ", n1, sizeof n1)) return 1;
    if (!read_pw("Retype new password: ", n2, sizeof n2)) return 1;
    if (strcmp(n1, n2)) { fprintf(stderr, "Sorry, passwords do not match.\npasswd: Authentication token manipulation error\npasswd: password unchanged\n"); return 1; }
    if (!n1[0]) { fprintf(stderr, "No password has been supplied.\npasswd: password unchanged\n"); return 1; }
    long rc = sc3(ABI_SYS_SETPASSWD, (long)(uintptr_t)target, (long)(uintptr_t)n1, (long)(uintptr_t)old);
    memset(old, 0, sizeof old); memset(n1, 0, sizeof n1); memset(n2, 0, sizeof n2);
    if (rc == -ABI_EACCES) { fprintf(stderr, "passwd: Authentication failure\npasswd: password unchanged\n"); return 1; }
    if (rc == -ABI_ENOENT) { fprintf(stderr, "passwd: user '%s' does not exist\n", target); return 1; }
    if (rc == -ABI_EPERM) { fprintf(stderr, "passwd: You may not view or modify password information for %s.\n", target); return 1; }
    if (rc != 0) { fprintf(stderr, "passwd: Authentication token manipulation error\npasswd: password unchanged\n"); return 1; }
    fprintf(stderr, "passwd: password updated successfully\n");
    return 0;
}

/* ============================================================= chpasswd */
/* chpasswd as shadow-utils' answers it (finding 194): NAME:PASSWORD lines
 * on stdin, the password everything after the first colon. Every line is
 * checked before any is set, so one bad line changes none. It was
 * busybox's applet, which wrote an /etc/shadow that no login here reads:
 * the account it "changed" stayed locked. The password goes to the
 * kernel's account table as passwd's does; the kernel keeps one hash of
 * its own, so -c and -s are taken and checked, not obeyed, and -e (a hash
 * made elsewhere) is refused rather than stored unverifiable. */
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
    int crypt = 0, rounds = 0, enc = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *val = 0;
        int need = !strcmp(a, "-c") || !strcmp(a, "--crypt-method") ||
                   !strcmp(a, "-s") || !strcmp(a, "--sha-rounds") ||
                   !strcmp(a, "-R") || !strcmp(a, "--root") ||
                   !strcmp(a, "-P") || !strcmp(a, "--prefix");
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { chp_usage(stdout); return 0; }
        if (!strcmp(a, "-e") || !strcmp(a, "--encrypted")) { enc = 1; continue; }
        if (!strcmp(a, "-m") || !strcmp(a, "--md5")) { crypt = 1; continue; }
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
    if (rounds && !crypt) {
        fprintf(stderr, "chpasswd: -s flag is only allowed with the -c flag\n");
        chp_usage(stderr);
        return 2;
    }
    if (enc) {
        fprintf(stderr, "chpasswd: -e is not supported: the kernel keeps its own "
                "hash and cannot check one made elsewhere\n");
        return 1;
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
        long r = sc3(ABI_SYS_SETPASSWD, (long)(uintptr_t)v[i], (long)(uintptr_t)(c + 1), 0);
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
