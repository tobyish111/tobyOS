/* su_yes.c -- yes; included by main.c. */

static int do_yes(int argc, char **argv) {
    char line[4096];
    size_t off = 0;
    for (int i = 1; i < argc; i++) {
        size_t l = strlen(argv[i]);
        if (off + l + 2 >= sizeof line) break;
        if (off) line[off++] = ' ';
        memcpy(line + off, argv[i], l);
        off += l;
    }
    if (!off) { line[off++] = 'y'; }
    line[off++] = '\n';
    for (;;) {
        if (fwrite(line, 1, off, stdout) != off) return 1;
    }
}
