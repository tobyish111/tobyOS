/* su_dd.c -- dd; included by main.c. */

static long parse_size(const char *s) {
    long v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    if (*s == 'k' || *s == 'K') v *= 1024;
    else if (*s == 'M' || *s == 'm') v *= 1024 * 1024;
    else if (*s == 'b') v *= 512;
    return v;
}

static int do_dd(int argc, char **argv) {
    const char *inf = NULL, *outf = NULL;
    long bs = 512, count = -1, skip = 0, seek = 0;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "if=", 3))    inf = argv[i] + 3;
        else if (!strncmp(argv[i], "of=", 3))  outf = argv[i] + 3;
        else if (!strncmp(argv[i], "bs=", 3))  bs = parse_size(argv[i] + 3);
        else if (!strncmp(argv[i], "count=", 6)) count = parse_size(argv[i] + 6);
        else if (!strncmp(argv[i], "skip=", 5))  skip = parse_size(argv[i] + 5);
        else if (!strncmp(argv[i], "seek=", 5))  seek = parse_size(argv[i] + 5);
        else if (!strncmp(argv[i], "status=", 7)) continue;
        else if (!strncmp(argv[i], "conv=", 5))  continue;
    }
    if (bs <= 0) bs = 512;

    int ifd = inf ? open(inf, O_RDONLY) : 0;
    if (ifd < 0) { fprintf(stderr, "dd: %s: %s\n", inf, strerror(errno)); return 1; }
    int ofd = outf ? open(outf, O_WRONLY | O_CREAT | O_TRUNC, 0666) : 1;
    if (ofd < 0) { fprintf(stderr, "dd: %s: %s\n", outf, strerror(errno)); return 1; }

    char *buf = (char *)malloc((size_t)bs);
    if (!buf) return 1;
    if (skip > 0) lseek(ifd, skip * bs, SEEK_SET);
    if (seek > 0) lseek(ofd, seek * bs, SEEK_SET);

    long full = 0, partial = 0, bytes = 0;
    for (long n = 0; count < 0 || n < count; n++) {
        ssize_t r = read(ifd, buf, (size_t)bs);
        if (r < 0) { fprintf(stderr, "dd: read error: %s\n", strerror(errno)); break; }
        if (r == 0) break;
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(ofd, buf + off, (size_t)(r - off));
            if (w <= 0) { fprintf(stderr, "dd: write error\n"); goto done; }
            off += w;
        }
        bytes += r;
        if (r == bs) full++; else partial++;
    }
done:
    free(buf);
    if (ifd > 0) close(ifd);
    if (ofd > 1) close(ofd);
    /* dd reports on stderr, which is why `dd ... 2>/dev/null` is idiomatic. */
    fprintf(stderr, "%ld+%ld records in\n%ld+%ld records out\n%ld bytes copied\n",
            full, partial, full, partial, bytes);
    return 0;
}
