/* su_stty.c -- stty; included by main.c. */

static int do_stty(int argc, char **argv) {
    struct termios t;
    if (tcgetattr(0, &t) != 0) {
        fprintf(stderr, "stty: standard input is not a terminal\n");
        return 1;
    }
    int changed = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int off = 0;
        if (a[0] == '-' && a[1] && strcmp(a, "-a") && strcmp(a, "-g")) { off = 1; a++; }
        if (!strcmp(a, "a") || !strcmp(a, "g")) continue;
        if (!strcmp(a, "echo"))   { if (off) t.c_lflag &= ~ECHO;   else t.c_lflag |= ECHO;   changed = 1; continue; }
        if (!strcmp(a, "icanon")) { if (off) t.c_lflag &= ~ICANON; else t.c_lflag |= ICANON; changed = 1; continue; }
        if (!strcmp(a, "raw"))    { t.c_lflag &= (tcflag_t)~(ICANON | ECHO); changed = 1; continue; }
        if (!strcmp(a, "cooked") || !strcmp(a, "sane")) {
            t.c_lflag |= (ICANON | ECHO);
            changed = 1;
            continue;
        }
    }
    if (changed) {
        if (tcsetattr(0, 0, &t) != 0) {
            fprintf(stderr, "stty: cannot set terminal attributes\n");
            return 1;
        }
        return 0;
    }
    printf("speed 38400 baud; line = 0;\n");
    printf("%sicanon %secho\n",
           (t.c_lflag & ICANON) ? "" : "-", (t.c_lflag & ECHO) ? "" : "-");
    return 0;
}
