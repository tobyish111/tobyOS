/* su_xargs.c -- xargs; included by main.c. */

static int run_argv(char **av) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) { execvp(av[0], av); _exit(127); }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* xargs [-n N] command [args]: run command with the words read from stdin
 * appended, as many commands as it takes.
 *
 * EVERY WORD, AND EVERY WORD WHOLE. This kept the first 4095 words and
 * dropped the rest, cut any word at 8 KiB, and packed up to 4150 words into
 * one command whatever their size:
 *
 *     seq 5000 | xargs echo | wc -w         GNU: 5000    here: 4095
 *
 * A command is now filled to XARGS_CMD_BYTES of argument text -- GNU's
 * default -s is 128 KiB, well inside the kernel's ABI_ARGS_TOTAL_MAX --
 * or to -n words, and as many commands run as the input needs. */
#define XARGS_CMD_BYTES (128 * 1024)

static int do_xargs(int argc, char **argv) {
    char **base = (char **)malloc((size_t)(argc + 1) * sizeof(char *));
    if (!base) { fprintf(stderr, "xargs: out of memory\n"); return 1; }
    int nb = 0;
    int maxargs = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) { maxargs = atoi(argv[++i]); continue; }
        if (!strcmp(argv[i], "-0") || !strcmp(argv[i], "-r")) continue;
        if (!strcmp(argv[i], "-t")) continue;
        base[nb++] = argv[i];
    }
    if (nb == 0) base[nb++] = (char *)"echo";
    size_t base_bytes = 0;
    for (int k = 0; k < nb; k++) base_bytes += strlen(base[k]) + 1;

    char  **items = 0;
    int     ni = 0, icap = 0;
    char   *word = 0;
    size_t  wl = 0, wcap = 0;
    int     c;
    int     oom = 0;
    /* Whitespace-separated, which is xargs's default; -0 is accepted and
     * behaves the same here because nothing in this tree emits NUL-separated
     * lists yet. */
    for (;;) {
        c = fgetc(stdin);
        int sep = (c == EOF || c == ' ' || c == '\t' || c == '\n' ||
                   c == '\r' || c == 0);
        if (sep) {
            if (wl) {
                if (ni >= icap) {
                    int ncap = icap ? icap * 2 : 256;
                    char **ni_ = (char **)realloc(items, (size_t)ncap * sizeof(char *));
                    if (!ni_) { oom = 1; break; }
                    items = ni_;
                    icap = ncap;
                }
                word[wl] = 0;
                items[ni] = strdup(word);
                if (!items[ni]) { oom = 1; break; }
                ni++;
                wl = 0;
            }
            if (c == EOF) break;
            continue;
        }
        if (wl + 1 >= wcap) {
            size_t ncap = wcap ? wcap * 2 : 256;
            char *nw = (char *)realloc(word, ncap);
            if (!nw) { oom = 1; break; }
            word = nw;
            wcap = ncap;
        }
        word[wl++] = (char)c;
    }
    free(word);
    if (oom) {
        fprintf(stderr, "xargs: out of memory\n");
        for (int k = 0; k < ni; k++) free(items[k]);
        free(items);
        free(base);
        return 1;
    }

    char **av = (char **)malloc((size_t)(nb + ni + 1) * sizeof(char *));
    if (!av) {
        fprintf(stderr, "xargs: out of memory\n");
        for (int k = 0; k < ni; k++) free(items[k]);
        free(items);
        free(base);
        return 1;
    }
    int rc = 0;
    int i = 0;
    do {
        int n = 0;
        for (int k = 0; k < nb; k++) av[n++] = base[k];
        size_t bytes = base_bytes;
        int taken = 0;
        while (i < ni && (!maxargs || taken < maxargs)) {
            size_t wb = strlen(items[i]) + 1;
            /* Always at least one word: a word longer than the budget
             * still gets its command, and the kernel says if it is too
             * big. */
            if (taken > 0 && bytes + wb > XARGS_CMD_BYTES) break;
            av[n++] = items[i++];
            bytes += wb;
            taken++;
        }
        av[n] = NULL;
        if (taken == 0 && ni > 0) break;
        int r = run_argv(av);
        if (r != 0) rc = r;
    } while (i < ni);
    for (int k = 0; k < ni; k++) free(items[k]);
    free(items);
    free(av);
    free(base);
    return rc;
}
