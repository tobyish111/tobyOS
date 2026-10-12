/* su_nohup.c -- nohup; included by main.c. */

static int do_nohup(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: nohup command [args]\n"); return 125; }
    signal(SIGHUP, SIG_IGN);
    /* Real nohup redirects stdout to nohup.out when it is a terminal, so
     * output survives the session going away. */
    if (isatty(1)) {
        int fd = open("nohup.out", O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd >= 0) {
            fprintf(stderr, "nohup: appending output to 'nohup.out'\n");
            dup2(fd, 1);
            if (isatty(2)) dup2(fd, 2);
            close(fd);
        }
    }
    execvp(argv[1], argv + 1);
    fprintf(stderr, "nohup: cannot run '%s': %s\n", argv[1], strerror(errno));
    return 127;
}
