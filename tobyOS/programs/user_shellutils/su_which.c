/* su_which.c -- which; included by main.c. */

static int do_which(int argc, char **argv) {
    const char *path = getenv("PATH");
    if (!path || !*path) path = "/bin:/usr/bin";
    int rc = 0, all = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) { all = 1; continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;
        int found = 0;
        const char *p = path;
        while (*p) {
            const char *e = p;
            while (*e && *e != ':') e++;
            char dir[512];
            size_t dl = (size_t)(e - p);
            if (dl >= sizeof dir) dl = sizeof dir - 1;
            memcpy(dir, p, dl); dir[dl] = 0;
            char cand[1024];
            snprintf(cand, sizeof cand, "%s/%s", dl ? dir : ".", argv[i]);
            struct stat st;
            if (stat(cand, &st) == 0 && !S_ISDIR(st.st_mode)) {
                printf("%s\n", cand);
                found = 1;
                if (!all) break;
            }
            p = *e ? e + 1 : e;
        }
        if (!found) rc = 1;
    }
    return rc;
}
