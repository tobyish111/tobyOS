/* su_id.c -- id, as GNU coreutils 9.7's (logs/utilgate/staging/id.sh):
 * id [OPTION]... [USER]...
 *
 * Measured in WSL (and as root there, to give a process real and
 * effective ids that differ):
 *
 * WITHOUT USER it describes this process: "uid=R(name) gid=R(name)", then
 * " euid=E(name)" and " egid=E(name)" only where they differ from the
 * real ones, then " groups=" -- the effective gid and then getgroups'
 * list, each once (the real gid is NOT in it unless getgroups has it).
 * -G lists the real gid, the effective gid, then getgroups', each once.
 * A USER is a name in the user database, else a number (blanks and '+'
 * allowed, (uid_t)-1 not) that has an entry; its ids are the entry's and
 * its groups are its login group then every group that lists it. "''"
 * is "no such user: No such file or directory"; any other miss is "'X':
 * no such user"; the next USER is still done, the status is 1.
 *
 * -u, -g, -G print only that (effective; real with -r, which -G
 * ignores); -n prints names -- an id with no name is printed as its
 * number with "cannot find name for user ID N" (or group), status 1. In
 * the full form a nameless id is the bare number, and no error. -z ends
 * each entry with a NUL instead of the space or newline; with -G and more
 * than one USER, each user's list ends with one more NUL. -a is ignored.
 * -Z: there is no SELinux, so "--context (-Z) works only on an
 * SELinux-enabled kernel", status 1, before anything else is looked at.
 * Then, in this order: two of -u -g -G ("cannot print "only" of more than
 * one choice"), -n or -r without them ("printing only names or real IDs
 * requires -u, -g, or -G"), -z without them ("option --zero not
 * permitted in default format").
 *
 * The user and group databases are /etc/passwd and /etc/group, read
 * directly (tobyOS has no NSS). libtoby's getuid and getgid answer the
 * EFFECTIVE ids, so on tobyOS the real ones come from /proc/self/status.
 *
 * The old id had no -z -a -Z, no long options, printed the effective ids
 * as real, and listed at most 32 groups. */

struct sy_pw {
    char name[256];
    unsigned long uid, gid;
};

/* the next ':' field of *P, NUL-terminated in place; 0 at the end */
static char *sy_idfield(char **p) {
    if (!*p) return 0;
    char *s = *p, *c = strchr(s, ':');
    if (c) { *c = 0; *p = c + 1; }
    else *p = 0;
    return s;
}

/* a decimal id field, all digits, below 2^32 */
static int sy_idnum(const char *s, unsigned long *out) {
    if (!isdigit((unsigned char)*s)) return 0;
    unsigned long long v = 0;
    for (; *s; s++) {
        if (!isdigit((unsigned char)*s)) return 0;
        v = v * 10 + (unsigned)(*s - '0');
        if (v > 0xFFFFFFFFull) return 0;
    }
    *out = (unsigned long)v;
    return 1;
}

/* NAME's (or, with NAME 0, UID's) entry in /etc/passwd; 1 when found */
static int sy_getpw(const char *name, unsigned long uid, struct sy_pw *pw) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return 0;
    char *line = 0;
    size_t cap = 0;
    int found = 0;
    while (!found && getline(&line, &cap, f) > 0) {
        line[strcspn(line, "\n")] = 0;
        char *p = line;
        char *n = sy_idfield(&p);
        sy_idfield(&p);
        char *u = sy_idfield(&p), *g = sy_idfield(&p);
        if (!n || !u || !g || !*n || *n == '+' || *n == '-' || *n == '#') continue;
        unsigned long uv, gv;
        if (!sy_idnum(u, &uv) || !sy_idnum(g, &gv)) continue;
        if (name ? strcmp(n, name) != 0 : uv != uid) continue;
        snprintf(pw->name, sizeof pw->name, "%s", n);
        pw->uid = uv;
        pw->gid = gv;
        found = 1;
    }
    free(line);
    fclose(f);
    return found;
}

static char sy_idbuf[2][256];

/* the name of uid ID (USER) or gid ID, or 0 */
static const char *sy_idname(int user, unsigned long id) {
    if (user) {
        struct sy_pw pw;
        if (!sy_getpw(0, id, &pw)) return 0;
        snprintf(sy_idbuf[0], sizeof sy_idbuf[0], "%s", pw.name);
        return sy_idbuf[0];
    }
    FILE *f = fopen("/etc/group", "r");
    if (!f) return 0;
    char *line = 0;
    size_t cap = 0;
    const char *r = 0;
    while (!r && getline(&line, &cap, f) > 0) {
        line[strcspn(line, "\n")] = 0;
        char *p = line;
        char *n = sy_idfield(&p);
        sy_idfield(&p);
        char *g = sy_idfield(&p);
        unsigned long gv;
        if (!n || !g || !*n || *n == '+' || *n == '-' || !sy_idnum(g, &gv) || gv != id) continue;
        snprintf(sy_idbuf[1], sizeof sy_idbuf[1], "%s", n);
        r = sy_idbuf[1];
    }
    free(line);
    fclose(f);
    return r;
}

struct sy_gl { unsigned long *v; int n, cap; };

/* add G unless the list has it */
static void sy_gladd(struct sy_gl *l, unsigned long g) {
    for (int i = 0; i < l->n; i++) if (l->v[i] == g) return;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 32;
        l->v = (unsigned long *)realloc(l->v, (size_t)l->cap * sizeof *l->v);
        if (!l->v) su_error(1, ENOMEM, "memory exhausted");
    }
    l->v[l->n++] = g;
}

/* every group in /etc/group whose members name USER, in the file's order */
static void sy_glmember(struct sy_gl *l, const char *user) {
    FILE *f = fopen("/etc/group", "r");
    if (!f) return;
    char *line = 0;
    size_t cap = 0;
    while (getline(&line, &cap, f) > 0) {
        line[strcspn(line, "\n")] = 0;
        char *p = line;
        char *n = sy_idfield(&p);
        sy_idfield(&p);
        char *g = sy_idfield(&p);
        unsigned long gv;
        if (!n || !g || !p || !sy_idnum(g, &gv)) continue;
        for (char *m = strtok(p, ","); m; m = strtok(0, ","))
            if (!strcmp(m, user)) { sy_gladd(l, gv); break; }
    }
    free(line);
    fclose(f);
}

/* this process's supplementary groups, in getgroups' order */
static void sy_glprocess(struct sy_gl *l) {
    int n = getgroups(0, 0);
    if (n <= 0) return;
    gid_t *g = (gid_t *)malloc((size_t)n * sizeof *g);
    if (!g) su_error(1, ENOMEM, "memory exhausted");
    n = getgroups(n, g);
    for (int i = 0; i < n; i++) sy_gladd(l, (unsigned long)g[i]);
    free(g);
}

/* the real uid and gid: libtoby's getuid/getgid answer the effective
 * ones, /proc/self/status has all of them */
static void sy_idreal(unsigned long *ruid, unsigned long *rgid) {
    *ruid = (unsigned long)getuid();
    *rgid = (unsigned long)getgid();
#ifndef SU_HOST
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "Uid:", 4)) *ruid = strtoul(line + 4, 0, 10);
        else if (!strncmp(line, "Gid:", 4)) *rgid = strtoul(line + 4, 0, 10);
    }
    fclose(f);
#endif
}

static int sy_idok;

/* one id: its name with -n (or the number, and an error) */
static void sy_idput(int user, unsigned long id, int names) {
    if (names) {
        const char *n = sy_idname(user, id);
        if (n) { fputs(n, stdout); return; }
        su_error(0, 0, "cannot find name for %s ID %lu", user ? "user" : "group", id);
        sy_idok = 0;
    }
    printf("%lu", id);
}

/* "ID(name)", or the bare number */
static void sy_idfull(int user, unsigned long id) {
    const char *n = sy_idname(user, id);
    printf("%lu", id);
    if (n) printf("(%s)", n);
}

static int do_id(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "context", SU_NO_ARG, 0, 'Z' },
        { "group", SU_NO_ARG, 0, 'g' },
        { "groups", SU_NO_ARG, 0, 'G' },
        { "name", SU_NO_ARG, 0, 'n' },
        { "real", SU_NO_ARG, 0, 'r' },
        { "user", SU_NO_ARG, 0, 'u' },
        { "zero", SU_NO_ARG, 0, 'z' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    int ou = 0, og = 0, oG = 0, on = 0, orl = 0, oz = 0, oZ = 0, c;
    while ((c = su_getopt_long(argc, argv, "agnruzGZ", longs, 0)) != -1) {
        switch (c) {
        case 'a': break;
        case 'g': og = 1; break;
        case 'G': oG = 1; break;
        case 'n': on = 1; break;
        case 'r': orl = 1; break;
        case 'u': ou = 1; break;
        case 'z': oz = 1; break;
        case 'Z': oZ = 1; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [USER]...\n"
                   "Show the user and group identity of each USER -- or, with no USER,\n"
                   "of this process.\n"
                   "\n"
                   "  -a             accepted and ignored (other ids have it)\n"
                   "  -Z, --context  the process's security context (needs SELinux)\n"
                   "  -g, --group    the effective group ID alone\n"
                   "  -G, --groups   every group ID\n"
                   "  -n, --name     a name, not a number (with -u, -g or -G)\n"
                   "  -r, --real     the real ID, not the effective one (with -u, -g or -G)\n"
                   "  -u, --user     the effective user ID alone\n"
                   "  -z, --zero     end each entry with a NUL, not a space or newline;\n"
                   "                   only with -u, -g or -G\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "With no option, it shows all of the ids, each with its name.\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (oZ) su_error(1, 0, "--context (-Z) works only on an SELinux-enabled kernel");
    if (ou + og + oG > 1) su_error(1, 0, "cannot print \"only\" of more than one choice");
    int only = ou + og + oG;
    if (!only && (on || orl))
        su_error(1, 0, "printing only names or real IDs requires -u, -g, or -G");
    if (!only && oz) su_error(1, 0, "option --zero not permitted in default format");

    sy_idok = 1;
    int nusers = argc - su_optind;
    for (int ui = su_optind; ui < argc || (nusers == 0 && ui == su_optind); ui++) {
        unsigned long ruid, euid, rgid, egid;
        struct sy_gl full = { 0, 0, 0 }, list = { 0, 0, 0 };
        const char *user = nusers ? argv[ui] : 0;
        if (user) {
            struct sy_pw pw;
            int found = sy_getpw(user, 0, &pw), errnum = 0;
            if (!found) {
                uintmax_t v;
                char *e;
                int r = su_strtoumax(user, &e, &v, 0);
                if (r == SU_NUM_OK && !*e && v < 0xFFFFFFFFu)
                    found = sy_getpw(0, (unsigned long)v, &pw);
                /* digits past uintmax_t leave ERANGE behind, whatever
                 * follows them; the empty name leaves ENOENT */
                if (r == SU_NUM_OVERFLOW) errnum = ERANGE;
                else if (!*user) errnum = ENOENT;
            }
            if (!found) {
                su_error(0, errnum, "%s: no such user", su_quote(user));
                sy_idok = 0;
                continue;
            }
            ruid = euid = pw.uid;
            rgid = egid = pw.gid;
            sy_gladd(&full, pw.gid);
            sy_glmember(&full, pw.name);
            sy_gladd(&list, pw.gid);
            sy_glmember(&list, pw.name);
        } else {
            sy_idreal(&ruid, &rgid);
            euid = (unsigned long)geteuid();
            egid = (unsigned long)getegid();
            sy_gladd(&full, egid);
            sy_glprocess(&full);
            sy_gladd(&list, rgid);
            sy_gladd(&list, egid);
            sy_glprocess(&list);
        }
        char end = oz ? '\0' : '\n';
        if (ou) {
            sy_idput(1, orl ? ruid : euid, on);
            putchar(end);
        } else if (og) {
            sy_idput(0, orl ? rgid : egid, on);
            putchar(end);
        } else if (oG) {
            for (int i = 0; i < list.n; i++) {
                if (i) putchar(oz ? '\0' : ' ');
                sy_idput(0, list.v[i], on);
            }
            putchar(end);
            if (oz && nusers > 1) putchar('\0');
        } else {
            fputs("uid=", stdout);
            sy_idfull(1, ruid);
            fputs(" gid=", stdout);
            sy_idfull(0, rgid);
            if (euid != ruid) { fputs(" euid=", stdout); sy_idfull(1, euid); }
            if (egid != rgid) { fputs(" egid=", stdout); sy_idfull(0, egid); }
            fputs(" groups=", stdout);
            for (int i = 0; i < full.n; i++) {
                if (i) putchar(',');
                sy_idfull(0, full.v[i]);
            }
            putchar('\n');
        }
        free(full.v);
        free(list.v);
        if (!nusers) break;
    }
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return sy_idok ? 0 : 1;
}
