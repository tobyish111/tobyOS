/* su_nice.c -- nice; included by main.c. */

static int do_nice(int argc, char **argv) {
    int argi = 1;
    for (; argi < argc; argi++) {
        if (!strcmp(argv[argi], "-n") && argi + 1 < argc) { argi++; continue; }
        if (argv[argi][0] == '-' && argv[argi][1]) continue;
        break;
    }
    if (argi >= argc) {
        fprintf(stderr, "nice: no setpriority syscall is exposed, so there "
                        "is no niceness to report\n");
        return 1;
    }
    /* The command still RUNS; only the priority change is impossible, and
     * that is said once rather than silently dropped. */
    fprintf(stderr, "nice: running without a priority change: no setpriority "
                    "syscall is exposed to userland\n");
    execvp(argv[argi], argv + argi);
    fprintf(stderr, "nice: cannot run %s: %s\n", argv[argi], strerror(errno));
    return 127;
}
