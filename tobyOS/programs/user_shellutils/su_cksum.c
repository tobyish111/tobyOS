/* su_cksum.c -- cksum; included by main.c. */

/* POSIX cksum is a CRC-32 with a DIFFERENT convention from gzip's: the
 * polynomial runs MSB-first and the LENGTH is fed in after the data, which
 * is why the two never agree on the same file. */
static unsigned long cksum_tab[256];
static int cksum_ready;

static unsigned long posix_cksum(const unsigned char *p, size_t n) {
    if (!cksum_ready) {
        for (unsigned long i = 0; i < 256; i++) {
            unsigned long c = i << 24;
            for (int k = 0; k < 8; k++)
                c = (c & 0x80000000UL) ? ((c << 1) ^ 0x04C11DB7UL) : (c << 1);
            cksum_tab[i] = c & 0xFFFFFFFFUL;
        }
        cksum_ready = 1;
    }
    unsigned long c = 0;
    for (size_t i = 0; i < n; i++)
        c = ((c << 8) ^ cksum_tab[((c >> 24) ^ p[i]) & 0xFF]) & 0xFFFFFFFFUL;
    for (size_t len = n; len; len >>= 8)
        c = ((c << 8) ^ cksum_tab[((c >> 24) ^ (len & 0xFF)) & 0xFF]) & 0xFFFFFFFFUL;
    return (~c) & 0xFFFFFFFFUL;
}

static unsigned char *slurp_stream(FILE *f, size_t *outlen) {
    unsigned char *b = NULL;
    size_t cap = 0, len = 0;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 65536;
            b = (unsigned char *)realloc(b, cap);
            if (!b) { *outlen = 0; return NULL; }
        }
        size_t got = fread(b + len, 1, cap - len, f);
        if (!got) break;
        len += got;
    }
    *outlen = len;
    return b;
}

static int do_cksum(int argc, char **argv) {
    int rc = 0, any = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) continue;
        any = 1;
        FILE *f = fopen(argv[i], "rb");
        if (!f) { fprintf(stderr, "cksum: %s: %s\n", argv[i], strerror(errno)); rc = 1; continue; }
        size_t len = 0;
        unsigned char *b = slurp_stream(f, &len);
        fclose(f);
        printf("%lu %zu %s\n", posix_cksum(b, len), len, argv[i]);
        free(b);
    }
    if (!any) {
        size_t len = 0;
        unsigned char *b = slurp_stream(stdin, &len);
        printf("%lu %zu\n", posix_cksum(b, len), len);
        free(b);
    }
    return rc;
}
