/* su_du.c -- du; included by main.c. */

static long du_walk(const char *path, int depth, int all, int summarise,
                    int human, long *total)
{
    struct stat st;
    /* lstat: du counts a link, it does not follow one (GNU's -P) */
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "du: cannot access '%s': %s\n", path, strerror(errno));
        return 0;
    }
    long kb = 0;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char sub[1024];
                snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
                kb += du_walk(sub, depth + 1, all, summarise, human, total);
            }
            closedir(d);
        }
    } else {
        kb = (long)((st.st_size + 1023) / 1024);
        if (kb == 0) kb = 1;      /* du counts an allocated block, not zero */
    }
    int show = S_ISDIR(st.st_mode) ? 1 : all;
    if (summarise && depth > 0) show = 0;
    if (show) {
        if (human && kb >= 1024) printf("%ldM\t%s\n", kb / 1024, path);
        else if (human)          printf("%ldK\t%s\n", kb, path);
        else                     printf("%ld\t%s\n", kb, path);
    }
    *total += 0;
    return kb;
}

static int do_du(int argc, char **argv) {
    int all = 0, summarise = 0, human = 0;
    const char *paths[64];
    int np = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1]) {
            for (const char *p = a + 1; *p; p++) {
                if (*p == 'a') all = 1;
                else if (*p == 's') summarise = 1;
                else if (*p == 'h') human = 1;
                else if (*p == 'k') human = 0;
            }
            continue;
        }
        if (np < 64) paths[np++] = a;
    }
    if (!np) paths[np++] = ".";
    long total = 0;
    for (int i = 0; i < np; i++) du_walk(paths[i], 0, all, summarise, human, &total);
    return 0;
}
