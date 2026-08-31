/* user_admin -- the shadow-suite account tools tobyOS did not have.
 *
 *   useradd userdel usermod groupadd groupdel groupmod
 *   chsh chfn chage gpasswd vipw
 *
 * One binary dispatching on argv[0], hard-linked to each name in the initrd,
 * the same way busybox and user_coreutil are installed here.
 *
 * WHERE THE ACCOUNTS LIVE. There are two databases and they are not the same
 * thing:
 *
 *   /etc/passwd + /etc/group  -- the Unix files. `id`, `newgrp`, tsh's ~user
 *                                expansion and `compgen -A user` all read
 *                                these, so this is what the tools must edit
 *                                for a change to be visible.
 *   /data/users               -- the KERNEL's auth database (name, uid, gid,
 *                                password hash), used by SYS_LOGIN. Userland
 *                                has no syscall to modify it, so these tools
 *                                deliberately do not claim to: a new account
 *                                is a full /etc identity and can be switched
 *                                to with newgrp/su semantics, but `login`
 *                                still authenticates against the kernel's
 *                                database. That is stated by useradd itself
 *                                rather than left for someone to discover.
 *
 * PERSISTENCE. /etc is ramfs: an existing file can be rewritten, a new one
 * cannot be created, and nothing there survives a reboot. Every change is
 * therefore mirrored to /data/etc/, which the kernel restores over /etc at
 * boot. Without that mirror a useradd would evaporate on the next power
 * cycle, which is barely worth having.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>

#define PASSWD_PATH "/etc/passwd"
#define GROUP_PATH  "/etc/group"
#define MIRROR_DIR  "/data/etc"
#define MIRROR_PW   "/data/etc/passwd"
#define MIRROR_GR   "/data/etc/group"

#define MAX_LINES 256
#define LINE_CAP  512
#define FILE_CAP  (MAX_LINES * LINE_CAP)

static const char *g_prog = "user_admin";

static int errf(const char *fmt, const char *a) {
    if (a) fprintf(stderr, "%s: %s: %s\n", g_prog, a, fmt);
    else   fprintf(stderr, "%s: %s\n", g_prog, fmt);
    return 1;
}

/* ---- whole-file line list ---------------------------------------------- */
struct lines {
    char  buf[FILE_CAP];
    char *v[MAX_LINES];
    int   n;
};

static int lines_load(struct lines *L, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    L->n = 0;
    size_t used = 0;
    char tmp[LINE_CAP];
    while (fgets(tmp, sizeof tmp, f)) {
        size_t k = strlen(tmp);
        while (k && (tmp[k-1] == '\n' || tmp[k-1] == '\r')) tmp[--k] = 0;
        if (L->n >= MAX_LINES || used + k + 1 > sizeof L->buf) break;
        memcpy(L->buf + used, tmp, k + 1);
        L->v[L->n++] = L->buf + used;
        used += k + 1;
    }
    fclose(f);
    return 0;
}

/* Rewrite the file, then mirror it under /data so the change survives a
 * reboot. A failed mirror is reported but does not fail the command: the
 * account IS created for this session, and saying otherwise would be wrong
 * in the other direction. */
static int lines_save(const struct lines *L, const char *path,
                      const char *mirror) {
    FILE *f = fopen(path, "w");
    if (!f) { errf(strerror(errno), path); return 1; }
    for (int i = 0; i < L->n; i++) fprintf(f, "%s\n", L->v[i]);
    fclose(f);

    (void)mkdir(MIRROR_DIR, 0755);
    FILE *m = fopen(mirror, "w");
    if (!m) {
        fprintf(stderr, "%s: warning: cannot mirror to %s (%s); the change "
                        "is live but will not survive a reboot\n",
                g_prog, mirror, strerror(errno));
        return 0;
    }
    for (int i = 0; i < L->n; i++) fprintf(m, "%s\n", L->v[i]);
    fclose(m);
    return 0;
}

/* field n (0-based) of a colon-separated record, into out */
static void field(const char *line, int n, char *out, size_t cap) {
    out[0] = 0;
    int f = 0;
    const char *p = line;
    while (*p && f < n) { if (*p == ':') f++; p++; }
    if (f != n) return;
    size_t k = 0;
    while (*p && *p != ':' && k + 1 < cap) out[k++] = *p++;
    out[k] = 0;
}

static int find_record(const struct lines *L, const char *name) {
    size_t nl = strlen(name);
    for (int i = 0; i < L->n; i++)
        if (!strncmp(L->v[i], name, nl) && L->v[i][nl] == ':') return i;
    return -1;
}

static int max_id(const struct lines *L, int fieldno, int floor_) {
    int best = floor_;
    char b[64];
    for (int i = 0; i < L->n; i++) {
        field(L->v[i], fieldno, b, sizeof b);
        int v = atoi(b);
        /* Ignore the nobody-style high ids so the next account does not
         * land at 65535 because one reserved entry sits up there. */
        if (v > best && v < 60000) best = v;
    }
    return best;
}

/* Replace line `idx` with `text` (kept inside the buffer). */
static int lines_replace(struct lines *L, int idx, const char *text) {
    static char spare[MAX_LINES][LINE_CAP];
    static int  used;
    if (used >= MAX_LINES) return -1;
    snprintf(spare[used], LINE_CAP, "%s", text);
    L->v[idx] = spare[used++];
    return 0;
}

static int lines_append(struct lines *L, const char *text) {
    if (L->n >= MAX_LINES) return -1;
    L->v[L->n] = 0;
    L->n++;
    return lines_replace(L, L->n - 1, text);
}

static void lines_remove(struct lines *L, int idx) {
    for (int i = idx; i < L->n - 1; i++) L->v[i] = L->v[i + 1];
    L->n--;
}

/* ---- group membership -------------------------------------------------- */

/* Add `user` to the member list of group line `idx`. */
static void group_add_member(struct lines *G, int idx, const char *user) {
    char name[64], pw[16], gid[16], mem[256];
    field(G->v[idx], 0, name, sizeof name);
    field(G->v[idx], 1, pw,   sizeof pw);
    field(G->v[idx], 2, gid,  sizeof gid);
    field(G->v[idx], 3, mem,  sizeof mem);
    /* Already a member? Adding twice would make `id` report it twice. */
    const char *p = mem;
    size_t ul = strlen(user);
    while (*p) {
        const char *e = p;
        while (*e && *e != ',') e++;
        if ((size_t)(e - p) == ul && !strncmp(p, user, ul)) return;
        p = *e ? e + 1 : e;
    }
    char out[LINE_CAP];
    snprintf(out, sizeof out, "%s:%s:%s:%s%s%s", name, pw, gid,
             mem, mem[0] ? "," : "", user);
    lines_replace(G, idx, out);
}

static void group_del_member(struct lines *G, int idx, const char *user) {
    char name[64], pw[16], gid[16], mem[256], keep[256] = "";
    field(G->v[idx], 0, name, sizeof name);
    field(G->v[idx], 1, pw,   sizeof pw);
    field(G->v[idx], 2, gid,  sizeof gid);
    field(G->v[idx], 3, mem,  sizeof mem);
    size_t ul = strlen(user);
    const char *p = mem;
    while (*p) {
        const char *e = p;
        while (*e && *e != ',') e++;
        size_t len = (size_t)(e - p);
        if (!(len == ul && !strncmp(p, user, ul)) && len) {
            if (keep[0]) strncat(keep, ",", sizeof keep - strlen(keep) - 1);
            strncat(keep, p, len < sizeof keep - strlen(keep) - 1
                             ? len : sizeof keep - strlen(keep) - 1);
        }
        p = *e ? e + 1 : e;
    }
    char out[LINE_CAP];
    snprintf(out, sizeof out, "%s:%s:%s:%s", name, pw, gid, keep);
    lines_replace(G, idx, out);
}

/* Remove `user` from EVERY group. userdel must do this or the name lingers
 * in group lists and `id` reports a deleted account as a member. */
static void group_purge_user(struct lines *G, const char *user) {
    for (int i = 0; i < G->n; i++) group_del_member(G, i, user);
}

/* ---- useradd ----------------------------------------------------------- */
static int cmd_useradd(int argc, char **argv) {
    int uid = -1, gid = -1, mkhome = 0;
    const char *home = 0, *shell = "/bin/tsh", *gecos = 0, *groups = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "-u") && i + 1 < argc) { uid = atoi(argv[++i]); continue; }
        if (!strcmp(a, "-g") && i + 1 < argc) { gid = atoi(argv[++i]); continue; }
        if (!strcmp(a, "-G") && i + 1 < argc) { groups = argv[++i]; continue; }
        if (!strcmp(a, "-d") && i + 1 < argc) { home = argv[++i]; continue; }
        if (!strcmp(a, "-s") && i + 1 < argc) { shell = argv[++i]; continue; }
        if (!strcmp(a, "-c") && i + 1 < argc) { gecos = argv[++i]; continue; }
        if (!strcmp(a, "-m")) { mkhome = 1; continue; }
        if (!strcmp(a, "-r") || !strcmp(a, "-N")) continue;
        return errf("invalid option", a);
    }
    if (i >= argc) {
        fprintf(stderr, "usage: useradd [-u uid] [-g gid] [-G grp,..] "
                        "[-d home] [-s shell] [-c comment] [-m] NAME\n");
        return 1;
    }
    const char *name = argv[i];

    struct lines P, G;
    if (lines_load(&P, PASSWD_PATH) != 0) return errf("cannot read", PASSWD_PATH);
    if (lines_load(&G, GROUP_PATH)  != 0) return errf("cannot read", GROUP_PATH);
    if (find_record(&P, name) >= 0) {
        fprintf(stderr, "useradd: user '%s' already exists\n", name);
        return 9;
    }
    if (uid < 0) uid = max_id(&P, 2, 999) + 1;
    if (gid < 0) gid = 100;

    char hbuf[128];
    if (!home) { snprintf(hbuf, sizeof hbuf, "/home/%s", name); home = hbuf; }

    char line[LINE_CAP];
    snprintf(line, sizeof line, "%s:x:%d:%d:%s:%s:%s",
             name, uid, gid, gecos ? gecos : name, home, shell);
    if (lines_append(&P, line) != 0) return errf("passwd file is full", 0);
    if (lines_save(&P, PASSWD_PATH, MIRROR_PW) != 0) return 1;

    if (groups) {
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s", groups);
        char *s = tmp;
        while (*s) {
            char *c = strchr(s, ',');
            if (c) *c = 0;
            int gi = find_record(&G, s);
            if (gi < 0) fprintf(stderr, "useradd: group '%s' does not exist\n", s);
            else group_add_member(&G, gi, name);
            if (!c) break;
            s = c + 1;
        }
        if (lines_save(&G, GROUP_PATH, MIRROR_GR) != 0) return 1;
    }
    if (mkhome && mkdir(home, 0755) != 0 && errno != EEXIST)
        fprintf(stderr, "useradd: warning: cannot create %s: %s\n",
                home, strerror(errno));

    /* Say the part that is NOT done, rather than let it be discovered at a
     * login prompt: the kernel's auth database is separate and userland has
     * no syscall to add to it. */
    fprintf(stderr, "useradd: '%s' added to %s. NOTE: the kernel login "
                    "database (/data/users) is separate and unchanged, so "
                    "this account cannot log in until it is added there.\n",
            name, PASSWD_PATH);
    return 0;
}

/* ---- userdel ----------------------------------------------------------- */
static int cmd_userdel(int argc, char **argv) {
    int rmhome = 0, i = 1;
    for (; i < argc; i++) {
        if (argv[i][0] != '-' || !argv[i][1]) break;
        if (!strcmp(argv[i], "-r")) { rmhome = 1; continue; }
        if (!strcmp(argv[i], "-f")) continue;
        return errf("invalid option", argv[i]);
    }
    if (i >= argc) { fprintf(stderr, "usage: userdel [-r] NAME\n"); return 1; }
    const char *name = argv[i];

    struct lines P, G;
    if (lines_load(&P, PASSWD_PATH) != 0) return errf("cannot read", PASSWD_PATH);
    int idx = find_record(&P, name);
    if (idx < 0) {
        fprintf(stderr, "userdel: user '%s' does not exist\n", name);
        return 6;
    }
    char home[128];
    field(P.v[idx], 5, home, sizeof home);
    lines_remove(&P, idx);
    if (lines_save(&P, PASSWD_PATH, MIRROR_PW) != 0) return 1;

    if (lines_load(&G, GROUP_PATH) == 0) {
        group_purge_user(&G, name);
        lines_save(&G, GROUP_PATH, MIRROR_GR);
    }
    if (rmhome && home[0]) {
        char cmd[256];
        snprintf(cmd, sizeof cmd, "%s", home);
        if (rmdir(cmd) != 0 && errno != ENOENT)
            fprintf(stderr, "userdel: warning: %s not removed: %s\n",
                    home, strerror(errno));
    }
    return 0;
}

/* ---- usermod ----------------------------------------------------------- */
static int cmd_usermod(int argc, char **argv) {
    int uid = -1, gid = -1, append = 0;
    const char *home = 0, *shell = 0, *gecos = 0, *groups = 0, *newname = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "-u") && i + 1 < argc) { uid = atoi(argv[++i]); continue; }
        if (!strcmp(a, "-g") && i + 1 < argc) { gid = atoi(argv[++i]); continue; }
        if (!strcmp(a, "-G") && i + 1 < argc) { groups = argv[++i]; continue; }
        if (!strcmp(a, "-d") && i + 1 < argc) { home = argv[++i]; continue; }
        if (!strcmp(a, "-s") && i + 1 < argc) { shell = argv[++i]; continue; }
        if (!strcmp(a, "-c") && i + 1 < argc) { gecos = argv[++i]; continue; }
        if (!strcmp(a, "-l") && i + 1 < argc) { newname = argv[++i]; continue; }
        if (!strcmp(a, "-a")) { append = 1; continue; }
        return errf("invalid option", a);
    }
    if (i >= argc) {
        fprintf(stderr, "usage: usermod [-u uid] [-g gid] [-G grp,..] [-a] "
                        "[-d home] [-s shell] [-c comment] [-l new] NAME\n");
        return 1;
    }
    const char *name = argv[i];

    struct lines P, G;
    if (lines_load(&P, PASSWD_PATH) != 0) return errf("cannot read", PASSWD_PATH);
    int idx = find_record(&P, name);
    if (idx < 0) {
        fprintf(stderr, "usermod: user '%s' does not exist\n", name);
        return 6;
    }
    char f0[64], f1[16], f2[16], f3[16], f4[128], f5[128], f6[128];
    field(P.v[idx], 0, f0, sizeof f0); field(P.v[idx], 1, f1, sizeof f1);
    field(P.v[idx], 2, f2, sizeof f2); field(P.v[idx], 3, f3, sizeof f3);
    field(P.v[idx], 4, f4, sizeof f4); field(P.v[idx], 5, f5, sizeof f5);
    field(P.v[idx], 6, f6, sizeof f6);

    char line[LINE_CAP];
    char sid[16], sgid[16];
    snprintf(sid,  sizeof sid,  "%d", uid >= 0 ? uid : atoi(f2));
    snprintf(sgid, sizeof sgid, "%d", gid >= 0 ? gid : atoi(f3));
    snprintf(line, sizeof line, "%s:%s:%s:%s:%s:%s:%s",
             newname ? newname : f0, f1, sid, sgid,
             gecos ? gecos : f4, home ? home : f5, shell ? shell : f6);
    lines_replace(&P, idx, line);
    if (lines_save(&P, PASSWD_PATH, MIRROR_PW) != 0) return 1;

    if (groups && lines_load(&G, GROUP_PATH) == 0) {
        /* Without -a, -G REPLACES the supplementary set, which is the
         * behaviour that surprises people the most -- so it is exactly the
         * behaviour to reproduce. */
        if (!append) group_purge_user(&G, newname ? newname : name);
        char tmp[256];
        snprintf(tmp, sizeof tmp, "%s", groups);
        char *s = tmp;
        while (*s) {
            char *c = strchr(s, ',');
            if (c) *c = 0;
            int gi = find_record(&G, s);
            if (gi < 0) fprintf(stderr, "usermod: group '%s' does not exist\n", s);
            else group_add_member(&G, gi, newname ? newname : name);
            if (!c) break;
            s = c + 1;
        }
        lines_save(&G, GROUP_PATH, MIRROR_GR);
    } else if (newname && lines_load(&G, GROUP_PATH) == 0) {
        group_purge_user(&G, name);
        lines_save(&G, GROUP_PATH, MIRROR_GR);
    }
    return 0;
}

/* ---- groupadd / groupdel / groupmod ------------------------------------ */
static int cmd_groupadd(int argc, char **argv) {
    int gid = -1, i = 1;
    for (; i < argc; i++) {
        if (argv[i][0] != '-' || !argv[i][1]) break;
        if (!strcmp(argv[i], "-g") && i + 1 < argc) { gid = atoi(argv[++i]); continue; }
        if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "-r")) continue;
        return errf("invalid option", argv[i]);
    }
    if (i >= argc) { fprintf(stderr, "usage: groupadd [-g gid] NAME\n"); return 1; }
    const char *name = argv[i];
    struct lines G;
    if (lines_load(&G, GROUP_PATH) != 0) return errf("cannot read", GROUP_PATH);
    if (find_record(&G, name) >= 0) {
        fprintf(stderr, "groupadd: group '%s' already exists\n", name);
        return 9;
    }
    if (gid < 0) gid = max_id(&G, 2, 99) + 1;
    char line[LINE_CAP];
    snprintf(line, sizeof line, "%s:x:%d:", name, gid);
    if (lines_append(&G, line) != 0) return errf("group file is full", 0);
    return lines_save(&G, GROUP_PATH, MIRROR_GR);
}

static int cmd_groupdel(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: groupdel NAME\n"); return 1; }
    struct lines G;
    if (lines_load(&G, GROUP_PATH) != 0) return errf("cannot read", GROUP_PATH);
    int idx = find_record(&G, argv[1]);
    if (idx < 0) {
        fprintf(stderr, "groupdel: group '%s' does not exist\n", argv[1]);
        return 6;
    }
    lines_remove(&G, idx);
    return lines_save(&G, GROUP_PATH, MIRROR_GR);
}

static int cmd_groupmod(int argc, char **argv) {
    int gid = -1; const char *newname = 0; int i = 1;
    for (; i < argc; i++) {
        if (argv[i][0] != '-' || !argv[i][1]) break;
        if (!strcmp(argv[i], "-g") && i + 1 < argc) { gid = atoi(argv[++i]); continue; }
        if (!strcmp(argv[i], "-n") && i + 1 < argc) { newname = argv[++i]; continue; }
        return errf("invalid option", argv[i]);
    }
    if (i >= argc) { fprintf(stderr, "usage: groupmod [-g gid] [-n new] NAME\n"); return 1; }
    struct lines G;
    if (lines_load(&G, GROUP_PATH) != 0) return errf("cannot read", GROUP_PATH);
    int idx = find_record(&G, argv[i]);
    if (idx < 0) {
        fprintf(stderr, "groupmod: group '%s' does not exist\n", argv[i]);
        return 6;
    }
    char f0[64], f1[16], f2[16], f3[256];
    field(G.v[idx], 0, f0, sizeof f0); field(G.v[idx], 1, f1, sizeof f1);
    field(G.v[idx], 2, f2, sizeof f2); field(G.v[idx], 3, f3, sizeof f3);
    char sgid[16], line[LINE_CAP];
    snprintf(sgid, sizeof sgid, "%d", gid >= 0 ? gid : atoi(f2));
    snprintf(line, sizeof line, "%s:%s:%s:%s", newname ? newname : f0, f1, sgid, f3);
    lines_replace(&G, idx, line);
    return lines_save(&G, GROUP_PATH, MIRROR_GR);
}

/* ---- gpasswd ----------------------------------------------------------- */
static int cmd_gpasswd(int argc, char **argv) {
    const char *addu = 0, *delu = 0;
    int i = 1;
    for (; i < argc; i++) {
        if (argv[i][0] != '-' || !argv[i][1]) break;
        if (!strcmp(argv[i], "-a") && i + 1 < argc) { addu = argv[++i]; continue; }
        if (!strcmp(argv[i], "-d") && i + 1 < argc) { delu = argv[++i]; continue; }
        return errf("invalid option", argv[i]);
    }
    if (i >= argc) {
        fprintf(stderr, "usage: gpasswd [-a user] [-d user] GROUP\n");
        return 1;
    }
    struct lines G;
    if (lines_load(&G, GROUP_PATH) != 0) return errf("cannot read", GROUP_PATH);
    int idx = find_record(&G, argv[i]);
    if (idx < 0) {
        fprintf(stderr, "gpasswd: group '%s' does not exist\n", argv[i]);
        return 2;
    }
    if (!addu && !delu) {
        /* Setting a GROUP password needs a store this system has not got --
         * /etc/gshadow does not exist and /etc cannot take new files. */
        fprintf(stderr, "gpasswd: group passwords are not supported "
                        "(no /etc/gshadow on this system); use -a / -d\n");
        return 1;
    }
    if (addu) group_add_member(&G, idx, addu);
    if (delu) group_del_member(&G, idx, delu);
    return lines_save(&G, GROUP_PATH, MIRROR_GR);
}

/* ---- chsh / chfn ------------------------------------------------------- */
static int edit_passwd_field(int fieldno, const char *value, const char *who,
                             const char *what) {
    struct lines P;
    if (lines_load(&P, PASSWD_PATH) != 0) return errf("cannot read", PASSWD_PATH);
    int idx = find_record(&P, who);
    if (idx < 0) {
        fprintf(stderr, "%s: user '%s' does not exist\n", g_prog, who);
        return 1;
    }
    char f[7][128];
    for (int k = 0; k < 7; k++) field(P.v[idx], k, f[k], sizeof f[k]);
    snprintf(f[fieldno], sizeof f[0], "%s", value);
    char line[LINE_CAP];
    snprintf(line, sizeof line, "%s:%s:%s:%s:%s:%s:%s",
             f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
    lines_replace(&P, idx, line);
    if (lines_save(&P, PASSWD_PATH, MIRROR_PW) != 0) return 1;
    printf("%s changed for %s\n", what, who);
    return 0;
}

static const char *current_user(void) {
    const char *u = getenv("USER");
    if (u && *u) return u;
    u = getenv("LOGNAME");
    return (u && *u) ? u : "root";
}

static int cmd_chsh(int argc, char **argv) {
    const char *shell = 0; int i = 1;
    for (; i < argc; i++) {
        if (argv[i][0] != '-' || !argv[i][1]) break;
        if (!strcmp(argv[i], "-s") && i + 1 < argc) { shell = argv[++i]; continue; }
        return errf("invalid option", argv[i]);
    }
    const char *who = (i < argc) ? argv[i] : current_user();
    if (!shell) {
        fprintf(stderr, "usage: chsh -s SHELL [USER]\n");
        return 1;
    }
    /* Refuse a shell that is not there: a bad entry here is how an account
     * stops being able to log in at all. */
    struct stat st;
    if (stat(shell, &st) != 0) {
        fprintf(stderr, "chsh: %s: no such file -- refusing to set a shell "
                        "that does not exist\n", shell);
        return 1;
    }
    return edit_passwd_field(6, shell, who, "Shell");
}

static int cmd_chfn(int argc, char **argv) {
    const char *full = 0; int i = 1;
    for (; i < argc; i++) {
        if (argv[i][0] != '-' || !argv[i][1]) break;
        if (!strcmp(argv[i], "-f") && i + 1 < argc) { full = argv[++i]; continue; }
        return errf("invalid option", argv[i]);
    }
    const char *who = (i < argc) ? argv[i] : current_user();
    if (!full) { fprintf(stderr, "usage: chfn -f NAME [USER]\n"); return 1; }
    return edit_passwd_field(4, full, who, "Name");
}

/* ---- chage ------------------------------------------------------------- */
static int cmd_chage(int argc, char **argv) {
    int list = 0, i = 1;
    for (; i < argc; i++) {
        if (argv[i][0] != '-' || !argv[i][1]) break;
        if (!strcmp(argv[i], "-l")) { list = 1; continue; }
        /* The setting options all need a shadow file to write to. */
        fprintf(stderr, "chage: password aging is not configurable: this "
                        "system has no /etc/shadow, and /etc cannot take new "
                        "files\n");
        return 1;
    }
    if (i >= argc) { fprintf(stderr, "usage: chage -l USER\n"); return 1; }
    struct lines P;
    if (lines_load(&P, PASSWD_PATH) != 0) return errf("cannot read", PASSWD_PATH);
    if (find_record(&P, argv[i]) < 0) {
        fprintf(stderr, "chage: user '%s' does not exist\n", argv[i]);
        return 1;
    }
    if (list) {
        /* Report the truth: no aging data exists, so every field is
         * "never". Inventing dates would be worse than saying so. */
        printf("Last password change\t\t\t\t\t: never\n");
        printf("Password expires\t\t\t\t\t: never\n");
        printf("Password inactive\t\t\t\t\t: never\n");
        printf("Account expires\t\t\t\t\t\t: never\n");
        printf("Minimum number of days between password change\t\t: 0\n");
        printf("Maximum number of days between password change\t\t: 99999\n");
        printf("Number of days of warning before password expires\t: 7\n");
        return 0;
    }
    return 0;
}

/* ---- vipw / vigr ------------------------------------------------------- */
static int cmd_vipw(int argc, char **argv) {
    const char *target = PASSWD_PATH;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-g") || !strcmp(argv[i], "--group"))
            target = GROUP_PATH;
    const char *ed = getenv("EDITOR");
    if (!ed || !*ed) ed = "/bin/vi";
    char *av[3];
    av[0] = (char *)ed;
    av[1] = (char *)target;
    av[2] = 0;
    /* No lock file: /etc cannot take new files, so the .lock a real vipw
     * creates is impossible here. Say so rather than imply serialisation
     * that is not happening. */
    fprintf(stderr, "vipw: editing %s (no lock file is possible on this "
                    "system; do not edit from two places at once)\n", target);
    execv(ed, av);
    return errf(strerror(errno), ed);
}

/* ---- dispatch ---------------------------------------------------------- */
struct applet { const char *name; int (*fn)(int, char **); };
static const struct applet g_applets[] = {
    { "useradd",  cmd_useradd },
    { "userdel",  cmd_userdel },
    { "usermod",  cmd_usermod },
    { "groupadd", cmd_groupadd },
    { "groupdel", cmd_groupdel },
    { "groupmod", cmd_groupmod },
    { "gpasswd",  cmd_gpasswd },
    { "chsh",     cmd_chsh },
    { "chfn",     cmd_chfn },
    { "chage",    cmd_chage },
    { "vipw",     cmd_vipw },
    { "vigr",     cmd_vipw },
    { 0, 0 }
};

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "user_admin";
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
    fprintf(stderr, "user_admin: applets:");
    for (int i = 0; g_applets[i].name; i++) fprintf(stderr, " %s", g_applets[i].name);
    fprintf(stderr, "\n");
    return 1;
}
