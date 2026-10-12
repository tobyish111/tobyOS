/* su_watch.c -- watch; included by main.c. */

static int do_watch(int argc, char **argv) {
    long interval = 2;
    int argi = 1;
    for (; argi < argc; argi++) {
        if (!strcmp(argv[argi], "-n") && argi + 1 < argc) { interval = atol(argv[++argi]); continue; }
        if (!strcmp(argv[argi], "-t") || !strcmp(argv[argi], "-d")) continue;
        if (argv[argi][0] == '-' && argv[argi][1]) continue;
        break;
    }
    if (argi >= argc) { fprintf(stderr, "usage: watch [-n secs] command\n"); return 1; }
    if (interval < 1) interval = 1;

    /* Into a pipe, run ONCE and stop: a watch that loops forever in a
     * script would simply hang, and a script is the only place a
     * non-terminal stdout comes from. */
    int once = !isatty(1);
    for (;;) {
        if (!once) fputs("\033[H\033[2J", stdout);
        fflush(stdout);
        run_argv(argv + argi);
        fflush(stdout);
        if (once) break;
        sleep((unsigned)interval);
    }
    return 0;
}
