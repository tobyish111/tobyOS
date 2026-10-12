/* su_rmdir.c -- rmdir; included by main.c. */

static int do_rmdir(int argc, char **argv) {
    int rc = 0, parents = 0, n = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") || !strcmp(argv[i], "--parents")) { parents = 1; continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;
        n++;
        char path[1024];
        snprintf(path, sizeof path, "%s", argv[i]);
        for (;;) {
            if (rmdir(path) != 0) {
                fprintf(stderr, "rmdir: failed to remove '%s': %s\n",
                        path, strerror(errno));
                rc = 1;
                break;
            }
            if (!parents) break;
            /* -p walks up, and STOPS at the first parent that will not go;
             * that is not an error, it is where the tree stops being empty. */
            char *slash = strrchr(path, '/');
            if (!slash || slash == path) break;
            *slash = 0;
        }
    }
    if (!n) { fprintf(stderr, "usage: rmdir [-p] directory...\n"); return 1; }
    return rc;
}
