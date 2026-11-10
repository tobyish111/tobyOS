/* su_pinky.c -- pinky, as GNU coreutils 9.7's (logs/utilgate/staging/
 * pinky.sh): pinky [OPTION]... [USER]...
 *
 * The short format (the default, -s) lists the users logged in -- the
 * USER_PROCESS records of /var/run/utmp (su_who.c reads them), only
 * those of the USERs named when there are any -- under a heading (-f
 * leaves it out):
 *   LOGIN(%-8s) [ NAME(%-19.19s) ] MESG LINE(%-8s) [ IDLE(%-6s) ] WHEN [ HOST ]
 * NAME is the passwd entry's GECOS up to its first comma, each '&' the
 * login with its first letter capitalised; a user with no entry shows
 * "???" right-aligned in the 19. MESG and IDLE come from /dev/LINE: ' '
 * when its group may write to it, '*' when not, '?' (and "?????") when
 * there is no such file; IDLE is blank under a minute (or in the future),
 * "hh:mm" under a day, "Nd" past it. WHEN is "%b %e %H:%M". -w drops
 * NAME, -i NAME and HOST, -q NAME HOST and IDLE.
 *
 * The long format (-l, which needs USERs: "no username specified; at
 * least one must be specified when using -l") reads only the passwd
 * entry: "Login name: %-28s" then "In real life:  NAME" -- "???" and
 * nothing more for no such user --, "Directory: %-29s" then "Shell:
 * SHELL" (not with -b), "Project: " and ~/.project as it is (not with
 * -h), "Plan:\n" and ~/.plan as it is (not with -p), then an empty line.
 * A file that opens but cannot be read (a directory) prints only its
 * label. The status is 0 even for unknown users. The last of -s and -l
 * wins; --lookup would canonicalise host names through DNS, which a
 * numeric or empty host does not change (and tobyOS has no resolver here).
 *
 * Measured in WSL, where GNU's pinky reads systemd-logind's session files
 * and never utmp: the fixture gives it sessions and gives this pinky the
 * same logins as utmp records.
 *
 * The old pinky listed every /etc/passwd line under a heading it made up. */

struct sy_pkpw {
    char *name, *gecos, *dir, *shell;
    char *mem;
};

/* NAME's passwd entry; 1 when there is one */
static int sy_pkgetpw(const char *name, struct sy_pkpw *pw) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return 0;
    char *line = 0;
    size_t cap = 0;
    int found = 0;
    while (!found && getline(&line, &cap, f) > 0) {
        line[strcspn(line, "\n")] = 0;
        char *fld[7] = { 0 };
        int n = 0;
        char *p = line;
        fld[n++] = p;
        for (; *p && n < 7; p++) if (*p == ':') { *p = 0; fld[n++] = p + 1; }
        if (n < 7 || strcmp(fld[0], name)) continue;
        pw->mem = line;
        pw->name = fld[0];
        pw->gecos = fld[4];
        pw->dir = fld[5];
        pw->shell = fld[6];
        line = 0;
        found = 1;
    }
    free(line);
    fclose(f);
    return found;
}

/* the GECOS name: to the first comma, '&' the login capitalised */
static char *sy_pkname(const char *gecos, const char *login) {
    size_t cap = strlen(gecos) * (strlen(login) + 1) + 1;
    char *o = (char *)malloc(cap), *q = o;
    if (!o) su_error(1, ENOMEM, "memory exhausted");
    for (const char *p = gecos; *p && *p != ','; p++) {
        if (*p == '&') {
            for (const char *l = login; *l; l++)
                *q++ = (char)(l == login ? toupper((unsigned char)*l) : *l);
        } else *q++ = *p;
    }
    *q = 0;
    return o;
}

/* FILE's bytes to stdout, after LABEL, when it opens */
static void sy_pkcat(const char *dir, const char *file, const char *label) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    int fd = su_open_read(path, 0);
    if (fd < 0) return;
    fputs(label, stdout);
    char buf[4096];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) fwrite(buf, 1, (size_t)r, stdout);
    close(fd);
}

static int do_pinky(int argc, char **argv) {
    enum { LOOKUP_OPT = 0x100 };
    static const struct su_option longs[] = {
        { "lookup", SU_NO_ARG, 0, LOOKUP_OPT },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    int longfmt = 0, heading = 1, fullname = 1, where = 1, idle = 1;
    int homeshell = 1, project = 1, plan = 1, c;
    while ((c = su_getopt_long(argc, argv, "sfwiqbhlp", longs, 0)) != -1) {
        switch (c) {
        case 's': longfmt = 0; break;
        case 'l': longfmt = 1; break;
        case 'f': heading = 0; break;
        case 'w': fullname = 0; break;
        case 'i': fullname = 0; where = 0; break;
        case 'q': fullname = 0; where = 0; idle = 0; break;
        case 'b': homeshell = 0; break;
        case 'h': project = 0; break;
        case 'p': plan = 0; break;
        case LOOKUP_OPT: break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... [USER]...\n"
                   "\n"
                   "  -l              the long format, for the USERs named\n"
                   "  -b              no home directory or shell in the long format\n"
                   "  -h              no ~/.project in the long format\n"
                   "  -p              no ~/.plan in the long format\n"
                   "  -s              the short format (the default)\n"
                   "  -f              no heading line in the short format\n"
                   "  -w              no full name in the short format\n"
                   "  -i              no full name or remote host in the short format\n"
                   "  -q              no full name, remote host or idle time in the\n"
                   "                  short format\n"
                   "      --lookup    canonicalize host names through DNS\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "A small 'finger': who is logged in, and what the user database says\n"
                   "of them. The logins are read from %s.\n", su_name, WHO_FILE);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    int nusers = argc - su_optind;
    if (longfmt) {
        if (nusers == 0) {
            su_error(0, 0, "no username specified; at least one must be specified when using -l");
            su_try_help(1);
        }
        for (int i = su_optind; i < argc; i++) {
            struct sy_pkpw pw;
            printf("Login name: %-28s", argv[i]);
            if (!sy_pkgetpw(argv[i], &pw)) {
                printf("In real life:  ???\n");
                continue;
            }
            char *n = sy_pkname(pw.gecos, pw.name);
            printf("In real life:  %s\n", n);
            free(n);
            if (homeshell) printf("Directory: %-29sShell:  %s\n", pw.dir, pw.shell);
            if (project) sy_pkcat(pw.dir, ".project", "Project: ");
            if (plan) sy_pkcat(pw.dir, ".plan", "Plan:\n");
            putchar('\n');
            free(pw.mem);
        }
        if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
        return 0;
    }

    if (heading) {
        printf("%-8s", "Login");
        if (fullname) printf(" %-19s", "Name");
        printf("  %-8s", "TTY");
        if (idle) printf(" %-6s", "Idle");
        printf(" %-12s", "When");
        if (where) printf(" Where");
        putchar('\n');
    }
    size_t n;
    struct who_ut *u = who_read(WHO_FILE, &n);
    time_t now = time(0);
    for (size_t i = 0; i < n; i++) {
        char user[40], line[40], host[264], when[64];
        who_str(user, sizeof user, u[i].ut_user, sizeof u[i].ut_user);
        if (u[i].ut_type != 7 || !user[0]) continue;
        if (nusers) {
            int want = 0;
            for (int a = su_optind; a < argc && !want; a++) want = !strcmp(argv[a], user);
            if (!want) continue;
        }
        who_str(line, sizeof line, u[i].ut_line, sizeof u[i].ut_line);
        who_str(host, sizeof host, u[i].ut_host, sizeof u[i].ut_host);
        who_time(when, sizeof when, u[i].tv_sec);
        printf("%-8s", user);
        if (fullname) {
            struct sy_pkpw pw;
            if (sy_pkgetpw(user, &pw)) {
                char *nm = sy_pkname(pw.gecos, pw.name);
                printf(" %-19.19s", nm);
                free(nm);
                free(pw.mem);
            } else printf(" %19s", "???");
        }
        char path[80], mesg = '?', idlebuf[32] = "?????";
        struct stat st;
        snprintf(path, sizeof path, "/dev/%s", line);
        if (stat(path, &st) == 0) {
            mesg = (st.st_mode & S_IWGRP) ? ' ' : '*';
            long d = (long)(now - st.st_atime);
            if (d < 60) idlebuf[0] = 0;
            else if (d < 24 * 3600) snprintf(idlebuf, sizeof idlebuf, "%02ld:%02ld", d / 3600, d % 3600 / 60);
            else snprintf(idlebuf, sizeof idlebuf, "%ldd", d / 86400);
        }
        printf(" %c%-8s", mesg, line);
        if (idle) printf(" %-6s", idlebuf);
        printf(" %s", when);
        if (where && host[0]) printf(" %s", host);
        putchar('\n');
    }
    free(u);
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return 0;
}
