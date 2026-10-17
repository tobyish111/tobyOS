/* user_moreutils -- init and passwd (2026-09-04, native-parity batch; one
 * binary dispatched on argv[0]).
 *
 * passwd follows shadow-utils'. The rest of what was here -- arch chgrp
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

/* ============================================================== dispatch */
struct applet { const char *name; int (*fn)(int, char **); };
static const struct applet g_applets[] = {
    { "init",    cmd_init },
    { "passwd",  cmd_passwd },
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
