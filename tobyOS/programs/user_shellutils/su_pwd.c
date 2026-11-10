/* su_pwd.c -- pwd, the binary, as GNU coreutils' (logs/utilgate/pwd.sh).
 *
 * -P (the default) prints the physical working directory, getcwd's; -L,
 * or POSIXLY_CORRECT without -P, prints $PWD instead when it is a fit name
 * for it: absolute, with no "." or ".." component, and naming the same
 * file as "." (device and inode). Otherwise -L falls back to -P. The last
 * of -L and -P wins. Operands are not an error: "ignoring non-option
 * arguments" on stderr, and the directory anyway.
 *
 * It compared $PWD with getcwd's string -- libtoby's stat had no inode
 * numbers then -- so a $PWD through a symlink was never fit and -L printed
 * the physical path; options were exact strings, "-LP" an invalid option. */

static int pw_fit(const char *pwd) {
    if (!pwd || pwd[0] != '/') return 0;
    for (const char *p = pwd; (p = strstr(p, "/.")); p++) {
        if (p[2] == '\0' || p[2] == '/') return 0;                    /* /. */
        if (p[2] == '.' && (p[3] == '\0' || p[3] == '/')) return 0;   /* /.. */
    }
    struct stat a, b;
    if (stat(pwd, &a) != 0 || stat(".", &b) != 0) return 0;
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

/* WHEN getcwd(3) FAILS -- a removed working directory, ENOENT -- GNU's pwd
 * finds the name itself: the entry of ".." with the device and inode of
 * ".", then the same one level up, to the root. A level whose ".." has no
 * such entry ends it: "couldn't find directory entry in '..' with matching
 * i-node", status 1 (measured: coreutils 9.7 in a removed cwd). */
static void pw_walk(void) {
    char up[4096] = "..", out[4096] = "", tmp[4096], ent[8192];
    struct stat cur, par, st;
    if (stat(".", &cur) != 0) su_error(1, errno, "failed to stat %s", su_quote("."));
    for (;;) {
        if (stat(up, &par) != 0) su_error(1, errno, "failed to stat %s", su_quote(up));
        if (par.st_dev == cur.st_dev && par.st_ino == cur.st_ino) break;   /* the root */
        DIR *d = opendir(up);
        if (!d) su_error(1, errno, "cannot open directory %s", su_quote(up));
        struct dirent *de;
        int found = 0;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.' && (!de->d_name[1] ||
                (de->d_name[1] == '.' && !de->d_name[2]))) continue;
            snprintf(ent, sizeof ent, "%s/%s", up, de->d_name);
            if (lstat(ent, &st) == 0 && st.st_dev == cur.st_dev && st.st_ino == cur.st_ino) {
                snprintf(tmp, sizeof tmp, "/%s%s", de->d_name, out);
                memcpy(out, tmp, strlen(tmp) + 1);
                found = 1;
                break;
            }
        }
        closedir(d);
        if (!found)
            su_error(1, 0, "couldn't find directory entry in %s with matching i-node",
                     su_quote(up));
        cur = par;
        if (strlen(up) + 4 >= sizeof up) su_error(1, ENAMETOOLONG, "%s", su_quote(up));
        strcat(up, "/..");
    }
    puts(out[0] ? out : "/");
}

static int do_pwd(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "logical", SU_NO_ARG, 0, 'L' }, { "physical", SU_NO_ARG, 0, 'P' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    int c, logical = getenv("POSIXLY_CORRECT") != 0;
    while ((c = su_getopt_long(argc, argv, "LP", longs, 0)) != -1) {
        switch (c) {
        case 'L': logical = 1; break;
        case 'P': logical = 0; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]...\n"
                   "Print the name of the working directory.\n"
                   "\n"
                   "  -L, --logical   the name $PWD gives it, symlinks and all, when\n"
                   "                  that name is absolute, has no . or .. in it and\n"
                   "                  is this directory\n"
                   "  -P, --physical  the name with every symlink resolved (the default)\n"
                   "      --help      show this help and exit\n"
                   "      --version   show the version and exit\n", su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (su_optind < argc) su_error(0, 0, "ignoring non-option arguments");
    if (logical) {
        const char *pwd = getenv("PWD");
        if (pw_fit(pwd)) { puts(pwd); return 0; }
    }
    char buf[4096];
    if (!getcwd(buf, sizeof buf)) {
        pw_walk();
        return 0;
    }
    puts(buf);
    return 0;
}
