/* su_tty.c -- tty; included by main.c. */

static int do_tty(void) {
    if (!isatty(0)) { printf("not a tty\n"); return 1; }
    /* ttyname(0): what /proc/self/fd/0 names -- /dev/pts/N for a pty (an
     * SSH session, a terminal window). It said /dev/console for every
     * terminal, from before procfs had those links; the console is still
     * the answer where there is no link. */
    char buf[256];
    long n = readlink("/proc/self/fd/0", buf, sizeof buf - 1);
    if (n > 5 && strncmp(buf, "/dev/", 5) == 0) {
        buf[n] = '\0';
        printf("%s\n", buf);
        return 0;
    }
    printf("/dev/console\n");
    return 0;
}
