/* su_dd.c -- dd, as GNU coreutils 9.7's (logs/utilgate/dd.sh).
 *
 *   dd [OPERAND]...   if= of= ibs= obs= bs= cbs= skip= (iseek=) seek=
 *                     (oseek=) count= conv= iflag= oflag= status=
 *
 *   - Numbers as GNU reads them: a decimal with c w b k K M G T P E Z Y R
 *     Q (kB = 1000, K = KiB = 1024), products with x ("2x3", warning on a
 *     "0x" lead), "invalid number: 'N'" (": Value too large for defined
 *     data type" past 2^63-1); the block sizes must be positive.
 *   - bs= alone, with no conversion that changes the data, writes each
 *     read as one record (2+1 in, 2+1 out); anything else goes through an
 *     obs-sized output buffer -- GNU's two buffers.
 *   - conv=: ascii ebcdic ibm (GNU's tables, with block/unblock when cbs=
 *     is set), block unblock (spaces, newlines, "N truncated records"),
 *     lcase ucase, swab (an odd byte carried into the next read), sync
 *     (NULs, spaces when blocking), sparse, excl nocreat notrunc noerror
 *     fsync fdatasync; GNU's "cannot combine" checks in GNU's order.
 *   - iflag/oflag: append, fullblock (input only), count_bytes skip_bytes
 *     seek_bytes, and GNU's others taken (direct nocache dsync sync
 *     noatime noctty nofollow nonblock directory).
 *   - skip= seeks a seekable input and reads past an unseekable one ("X:
 *     cannot skip to specified offset"); seek= truncates the output there
 *     (but notrunc) and seeks, or writes NULs where it cannot.
 *   - The report on stderr: records in/out, truncated records, "N bytes
 *     (1.5 kB, 1.5 KiB) copied, T s, R/s" -- GNU's rounding (to nearest,
 *     ties to even); status=none, noxfer, progress. SIGUSR1 asks for it;
 *     SIGINT gives it, then the signal.
 *
 * It took if of bs count skip seek -- bs only as both block sizes, "k" and
 * "M" only, every other operand as nothing -- and said "N+M records in"
 * with no truncation, no conversions and no errors for bad operands. */

#define DC_ASCII     0x0001
#define DC_EBCDIC    0x0002
#define DC_IBM       0x0004
#define DC_BLOCK     0x0008
#define DC_UNBLOCK   0x0010
#define DC_LCASE     0x0020
#define DC_UCASE     0x0040
#define DC_SWAB      0x0080
#define DC_SYNC      0x0100
#define DC_NOERROR   0x0200
#define DC_NOTRUNC   0x0400
#define DC_SPARSE    0x0800
#define DC_EXCL      0x1000
#define DC_NOCREAT   0x2000
#define DC_FSYNC     0x4000
#define DC_FDATASYNC 0x8000

#define DF_APPEND      0x0001
#define DF_DIRECT      0x0002
#define DF_DIRECTORY   0x0004
#define DF_DSYNC       0x0008
#define DF_SYNC        0x0010
#define DF_FULLBLOCK   0x0020
#define DF_NOCACHE     0x0040
#define DF_NOATIME     0x0080
#define DF_NOCTTY      0x0100
#define DF_NOFOLLOW    0x0200
#define DF_NONBLOCK    0x0400
#define DF_COUNT_BYTES 0x0800
#define DF_SKIP_BYTES  0x1000
#define DF_SEEK_BYTES  0x2000

struct dd_sym { const char *name; int bits; };

static const struct dd_sym dd_convs[] = {
    { "ascii", DC_ASCII | DC_UNBLOCK }, { "ebcdic", DC_EBCDIC | DC_BLOCK },
    { "ibm", DC_IBM | DC_BLOCK }, { "block", DC_BLOCK }, { "unblock", DC_UNBLOCK },
    { "lcase", DC_LCASE }, { "ucase", DC_UCASE }, { "sparse", DC_SPARSE },
    { "swab", DC_SWAB }, { "sync", DC_SYNC }, { "excl", DC_EXCL },
    { "nocreat", DC_NOCREAT }, { "notrunc", DC_NOTRUNC }, { "noerror", DC_NOERROR },
    { "fdatasync", DC_FDATASYNC }, { "fsync", DC_FSYNC }, { 0, 0 }
};

static const struct dd_sym dd_flags[] = {
    { "append", DF_APPEND }, { "direct", DF_DIRECT }, { "directory", DF_DIRECTORY },
    { "dsync", DF_DSYNC }, { "sync", DF_SYNC }, { "fullblock", DF_FULLBLOCK },
    { "nocache", DF_NOCACHE }, { "noatime", DF_NOATIME }, { "noctty", DF_NOCTTY },
    { "nofollow", DF_NOFOLLOW }, { "nonblock", DF_NONBLOCK },
    { "count_bytes", DF_COUNT_BYTES }, { "skip_bytes", DF_SKIP_BYTES },
    { "seek_bytes", DF_SEEK_BYTES }, { 0, 0 }
};

enum { DS_DEFAULT, DS_NONE, DS_NOXFER, DS_PROGRESS };

/* POSIX's tables, as GNU dd's conv=ascii, conv=ebcdic and conv=ibm give
 * them (measured: every byte through each). */
static const unsigned char dd_ebcdic_to_ascii[256] = {
    0x00, 0x01, 0x02, 0x03, 0x9c, 0x09, 0x86, 0x7f, 0x97, 0x8d, 0x8e, 0x0b,
    0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x9d, 0x85, 0x08, 0x87,
    0x18, 0x19, 0x92, 0x8f, 0x1c, 0x1d, 0x1e, 0x1f, 0x80, 0x81, 0x82, 0x83,
    0x84, 0x0a, 0x17, 0x1b, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x05, 0x06, 0x07,
    0x90, 0x91, 0x16, 0x93, 0x94, 0x95, 0x96, 0x04, 0x98, 0x99, 0x9a, 0x9b,
    0x14, 0x15, 0x9e, 0x1a, 0x20, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6,
    0xa7, 0xa8, 0xd5, 0x2e, 0x3c, 0x28, 0x2b, 0x7c, 0x26, 0xa9, 0xaa, 0xab,
    0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0x21, 0x24, 0x2a, 0x29, 0x3b, 0x7e,
    0x2d, 0x2f, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xcb, 0x2c,
    0x25, 0x5f, 0x3e, 0x3f, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf, 0xc0, 0xc1,
    0xc2, 0x60, 0x3a, 0x23, 0x40, 0x27, 0x3d, 0x22, 0xc3, 0x61, 0x62, 0x63,
    0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
    0xca, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x5e, 0xcc,
    0xcd, 0xce, 0xcf, 0xd0, 0xd1, 0xe5, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7a, 0xd2, 0xd3, 0xd4, 0x5b, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xdb,
    0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0x5d, 0xe6, 0xe7,
    0x7b, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0xe8, 0xe9,
    0xea, 0xeb, 0xec, 0xed, 0x7d, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50,
    0x51, 0x52, 0xee, 0xef, 0xf0, 0xf1, 0xf2, 0xf3, 0x5c, 0x9f, 0x53, 0x54,
    0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0xfa, 0xfb,
    0xfc, 0xfd, 0xfe, 0xff,
};
static const unsigned char dd_ascii_to_ebcdic[256] = {
    0x00, 0x01, 0x02, 0x03, 0x37, 0x2d, 0x2e, 0x2f, 0x16, 0x05, 0x25, 0x0b,
    0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x3c, 0x3d, 0x32, 0x26,
    0x18, 0x19, 0x3f, 0x27, 0x1c, 0x1d, 0x1e, 0x1f, 0x40, 0x5a, 0x7f, 0x7b,
    0x5b, 0x6c, 0x50, 0x7d, 0x4d, 0x5d, 0x5c, 0x4e, 0x6b, 0x60, 0x4b, 0x61,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0x7a, 0x5e,
    0x4c, 0x7e, 0x6e, 0x6f, 0x7c, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xad, 0xe0, 0xbd, 0x9a, 0x6d,
    0x79, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x91, 0x92,
    0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6,
    0xa7, 0xa8, 0xa9, 0xc0, 0x4f, 0xd0, 0x5f, 0x07, 0x20, 0x21, 0x22, 0x23,
    0x24, 0x15, 0x06, 0x17, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x09, 0x0a, 0x1b,
    0x30, 0x31, 0x1a, 0x33, 0x34, 0x35, 0x36, 0x08, 0x38, 0x39, 0x3a, 0x3b,
    0x04, 0x14, 0x3e, 0xe1, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x62, 0x63,
    0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x80, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x6a,
    0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa0, 0xaa, 0xab, 0xac, 0x4a, 0xae, 0xaf,
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb,
    0xbc, 0xa1, 0xbe, 0xbf, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xda, 0xdb,
    0xdc, 0xdd, 0xde, 0xdf, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xfa, 0xfb,
    0xfc, 0xfd, 0xfe, 0xff,
};
static const unsigned char dd_ascii_to_ibm[256] = {
    0x00, 0x01, 0x02, 0x03, 0x37, 0x2d, 0x2e, 0x2f, 0x16, 0x05, 0x25, 0x0b,
    0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x3c, 0x3d, 0x32, 0x26,
    0x18, 0x19, 0x3f, 0x27, 0x1c, 0x1d, 0x1e, 0x1f, 0x40, 0x5a, 0x7f, 0x7b,
    0x5b, 0x6c, 0x50, 0x7d, 0x4d, 0x5d, 0x5c, 0x4e, 0x6b, 0x60, 0x4b, 0x61,
    0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0x7a, 0x5e,
    0x4c, 0x7e, 0x6e, 0x6f, 0x7c, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xad, 0xe0, 0xbd, 0x5f, 0x6d,
    0x79, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x91, 0x92,
    0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6,
    0xa7, 0xa8, 0xa9, 0xc0, 0x4f, 0xd0, 0xa1, 0x07, 0x20, 0x21, 0x22, 0x23,
    0x24, 0x15, 0x06, 0x17, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x09, 0x0a, 0x1b,
    0x30, 0x31, 0x1a, 0x33, 0x34, 0x35, 0x36, 0x08, 0x38, 0x39, 0x3a, 0x3b,
    0x04, 0x14, 0x3e, 0xe1, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x62, 0x63,
    0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x80, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x9a,
    0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa0, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
    0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb,
    0xbc, 0xbd, 0xbe, 0xbf, 0xca, 0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xda, 0xdb,
    0xdc, 0xdd, 0xde, 0xdf, 0xea, 0xeb, 0xec, 0xed, 0xee, 0xef, 0xfa, 0xfb,
    0xfc, 0xfd, 0xfe, 0xff,
};

static const char *dd_in_name = "standard input", *dd_out_name = "standard output";
static int dd_ifd = 0, dd_ofd = 1, dd_in_isdir;
static size_t dd_ibs = 512, dd_obs = 512, dd_cbs;
static int dd_conv, dd_iflag, dd_oflag, dd_status = DS_DEFAULT;
static uint64_t dd_r_full, dd_r_partial, dd_w_full, dd_w_partial, dd_r_trunc, dd_w_bytes;
static unsigned char dd_tr[256], dd_nl = '\n', dd_sp = ' ';
static int dd_translate, dd_final_seek, dd_progress_len;
static unsigned char *dd_obuf;
static size_t dd_ofill, dd_col, dd_pending;
static struct timespec dd_t0, dd_tlast;
static volatile int dd_sig_info, dd_sig_int;

/* N in units of BASE (1000: "1.5 kB"; 1024: "1.5 KiB"), GNU's way: one
 * decimal under 10, rounded to nearest with ties to even, carried to the
 * next unit at BASE. 0 (OUT untouched) when N is under BASE: no prefix. */
static int dd_human(uint64_t n, unsigned base, char *out, size_t cap) {
    static const char units[] = "KMGTPE";
    uint64_t b = 1;
    int e = 0;
    while (e < 6 && n / b >= base) { b *= base; e++; }
    if (!e) return 0;
    uint64_t q = n / b, r = n % b;
    if (q < 10) {
        uint64_t t = q * 10 + r * 10 / b, rem = r * 10 % b;
        if (2 * rem > b || (2 * rem == b && (t & 1))) t++;
        if (t < 100) {
            char u = base == 1000 && e == 1 ? 'k' : units[e - 1];
            snprintf(out, cap, "%d.%d %c%s", (int)(t / 10), (int)(t % 10), u,
                     base == 1000 ? "B" : "iB");
            return 1;
        }
        q = 10;
    } else {
        if (2 * r > b || (2 * r == b && (q & 1))) q++;
        if (q >= base && e < 6) {
            e++;
            snprintf(out, cap, "1.0 %c%s", base == 1000 && e == 1 ? 'k' : units[e - 1],
                     base == 1000 ? "B" : "iB");
            return 1;
        }
    }
    snprintf(out, cap, "%" PRIu64 " %c%s", q, base == 1000 && e == 1 ? 'k' : units[e - 1],
             base == 1000 ? "B" : "iB");
    return 1;
}

static uint64_t dd_elapsed_ns(const struct timespec *from) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t ns = (int64_t)(now.tv_sec - from->tv_sec) * 1000000000 + (now.tv_nsec - from->tv_nsec);
    return ns > 0 ? (uint64_t)ns : 0;
}

/* "N bytes (SI, IEC) copied, T s, R/s" -- the line, or the progress one */
static void dd_xfer_line(int progress) {
    char si[32], iec[32], tm[64], rate[48];
    uint64_t ns = dd_elapsed_ns(&dd_t0);
    int hs = dd_human(dd_w_bytes, 1000, si, sizeof si);
    int hi = dd_human(dd_w_bytes, 1024, iec, sizeof iec);
    struct sn_sb b = { 0, 0, 0 };
    struct su_fmtspec fs = { 0, 0, 0, 0, 0, -1, progress ? 0 : -1, progress ? 'f' : 'g' };
    su_ldfmt(&b, (long double)ns / 1e9L, &fs);
    snprintf(tm, sizeof tm, "%s s", b.s ? b.s : "0");
    free(b.s);
    if (!ns) snprintf(rate, sizeof rate, "Infinity B");
    else {
        long double v = (long double)dd_w_bytes * 1e9L / (long double)ns;
        if (v < 1000 && v < 18446744073709551615.0L) {
            struct sn_sb rb = { 0, 0, 0 };
            struct su_fmtspec rf = { 0, 0, 0, 0, 0, -1, v < 10 ? 1 : 0, 'f' };
            su_ldfmt(&rb, v, &rf);
            snprintf(rate, sizeof rate, "%s B", rb.s ? rb.s : "0");
            free(rb.s);
        } else if (!dd_human((uint64_t)(v + 0.5L), 1000, rate, sizeof rate))
            snprintf(rate, sizeof rate, "%" PRIu64 " B", (uint64_t)(v + 0.5L));
    }
    char line[256];
    int len;
    if (!hs)
        len = snprintf(line, sizeof line, "%" PRIu64 " byte%s copied, %s, %s/s", dd_w_bytes,
                       dd_w_bytes == 1 ? "" : "s", tm, rate);
    else if (!hi)
        len = snprintf(line, sizeof line, "%" PRIu64 " bytes (%s) copied, %s, %s/s",
                       dd_w_bytes, si, tm, rate);
    else
        len = snprintf(line, sizeof line, "%" PRIu64 " bytes (%s, %s) copied, %s, %s/s",
                       dd_w_bytes, si, iec, tm, rate);
    if (progress) {
        fprintf(stderr, "\r%s", line);
        for (int i = len; i < dd_progress_len; i++) fputc(' ', stderr);
        dd_progress_len = len;
    } else
        fprintf(stderr, "%s\n", line);
    fflush(stderr);
}

static void dd_stats(void) {
    if (dd_status == DS_NONE) return;
    if (dd_progress_len) { fputc('\n', stderr); dd_progress_len = 0; }
    fprintf(stderr, "%" PRIu64 "+%" PRIu64 " records in\n%" PRIu64 "+%" PRIu64 " records out\n",
            dd_r_full, dd_r_partial, dd_w_full, dd_w_partial);
    if (dd_r_trunc)
        fprintf(stderr, "%" PRIu64 " truncated record%s\n", dd_r_trunc, dd_r_trunc == 1 ? "" : "s");
    if (dd_status == DS_NOXFER) { fflush(stderr); return; }
    dd_xfer_line(0);
}

static void dd_on_info(int s) { (void)s; dd_sig_info = 1; }
static void dd_on_int(int s) { (void)s; dd_sig_int = 1; }

static void dd_signals(void) {
    if (dd_sig_int) {
        dd_stats();
        signal(SIGINT, SIG_DFL);
        raise(SIGINT);
        _exit(130);
    }
    if (dd_sig_info) {
        dd_sig_info = 0;
        dd_stats();
    }
}

static void dd_catch(int sig, void (*fn)(int)) {
    struct sigaction sa, old;
    if (sigaction(sig, 0, &old) == 0 && old.sa_handler == SIG_IGN) return;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, 0);
}

static void dd_quit(int status) {
    dd_stats();
    exit(status);
}

/* A number as GNU dd reads one: 0, 1 (invalid) or 2 (too large). */
static int dd_num(const char *s, uint64_t *out) {
    char *end;
    uintmax_t n = 0;
    int e = su_strtoumax(s, &end, &n, "bcEGkKMPQRTwYZ0");
    if ((e == SU_NUM_BADSUFFIX || e == SU_NUM_OVERFLOW) && *end == 'x') {
        uint64_t m;
        int e2 = dd_num(end + 1, &m);
        if (e2 == 1) return 1;
        if (n == 0 && !strncmp(s, "0x", 2))
            su_error(0, 0, "warning: %s is a zero multiplier; use %s if that is intended",
                     su_quote("0x"), su_quote("00x"));
        if (e == SU_NUM_OVERFLOW || e2 == 2 || (m && n > INT64_MAX / m)) return 2;
        *out = n * m;
        return 0;
    }
    if (e == SU_NUM_INVALID || e == SU_NUM_BADSUFFIX) return 1;
    if (e == SU_NUM_OVERFLOW || n > INT64_MAX) return 2;
    *out = n;
    return 0;
}

static uint64_t dd_number(const char *s, int positive) {
    uint64_t v = 0;
    int e = dd_num(s, &v);
    if (e == 0 && v > INT64_MAX) e = 2;
    if (e == 0 && positive && v == 0) e = 1;
    if (e == 1) su_error(1, 0, "invalid number: %s", su_quote(s));
    if (e == 2) su_error(1, EOVERFLOW, "invalid number: %s", su_quote(s));
    return v;
}

/* a comma list of TABLE's names, OR'd; status= keeps the last */
static int dd_symbols(const char *s, const struct dd_sym *table, const char *what, int last) {
    int bits = 0;
    for (;;) {
        const char *c = strchr(s, ',');
        size_t n = c ? (size_t)(c - s) : strlen(s);
        int found = -1;
        for (int i = 0; table[i].name; i++)
            if (strlen(table[i].name) == n && !strncmp(table[i].name, s, n)) { found = i; break; }
        if (found < 0) {
            char tmp[256];
            snprintf(tmp, sizeof tmp, "%.*s", (int)(n < 255 ? n : 255), s);
            su_error(0, 0, "%s: %s", what, su_quote(tmp));
            su_try_help(1);
        }
        bits = last ? table[found].bits : bits | table[found].bits;
        if (!c) break;
        s = c + 1;
    }
    return bits;
}

static void dd_write(const unsigned char *p, size_t n) {
    if (n == dd_obs) dd_w_full++; else dd_w_partial++;
    if (dd_conv & DC_SPARSE) {
        size_t i = 0;
        while (i < n && !p[i]) i++;
        if (i == n) {
            if (lseek(dd_ofd, (off_t)n, SEEK_CUR) >= 0) {
                dd_final_seek = 1;
                dd_w_bytes += n;
                return;
            }
            dd_conv &= ~DC_SPARSE;
        }
    }
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(dd_ofd, p + off, n - off);
        if (w < 0 && errno == EINTR) { dd_signals(); continue; }
        if (w <= 0) {
            su_error(0, w < 0 ? errno : ENOSPC, "error writing %s", su_quoteaf(dd_out_name));
            dd_w_bytes += off;
            dd_quit(1);
        }
        off += (size_t)w;
    }
    dd_w_bytes += n;
    dd_final_seek = 0;
}

static void dd_outc(unsigned char c) {
    dd_obuf[dd_ofill++] = c;
    if (dd_ofill == dd_obs) { dd_write(dd_obuf, dd_obs); dd_ofill = 0; }
}

static void dd_out(const unsigned char *p, size_t n) {
    while (n) {
        size_t k = dd_obs - dd_ofill;
        if (k > n) k = n;
        memcpy(dd_obuf + dd_ofill, p, k);
        dd_ofill += k;
        p += k;
        n -= k;
        if (dd_ofill == dd_obs) { dd_write(dd_obuf, dd_obs); dd_ofill = 0; }
    }
}

/* conv=block: newline-ended lines into cbs-wide records, space-padded */
static void dd_block(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] == dd_nl) {
            for (; dd_col < dd_cbs; dd_col++) dd_outc(dd_sp);
            dd_col = 0;
        } else {
            if (dd_col == dd_cbs) dd_r_trunc++;
            else if (dd_col < dd_cbs) dd_outc(p[i]);
            dd_col++;
        }
    }
}

/* conv=unblock: cbs-wide records into lines, trailing spaces dropped */
static void dd_unblock(const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (dd_col++ >= dd_cbs) {
            dd_col = dd_pending = 0;
            dd_outc(dd_nl);
            i--;
        } else if (p[i] == dd_sp) {
            dd_pending++;
        } else {
            for (; dd_pending; dd_pending--) dd_outc(dd_sp);
            dd_outc(p[i]);
        }
    }
}

/* one read of up to WANT bytes; fullblock reads until it has them */
static ssize_t dd_read(unsigned char *buf, size_t want) {
    if (dd_in_isdir) { errno = EISDIR; return -1; }
    size_t got = 0;
    while (got < want) {
        ssize_t r = read(dd_ifd, buf + got, want - got);
        if (r < 0 && errno == EINTR) { dd_signals(); continue; }
        if (r < 0) return got ? (ssize_t)got : -1;
        if (r == 0) break;
        got += (size_t)r;
        if (!(dd_iflag & DF_FULLBLOCK)) break;
    }
    return (ssize_t)got;
}

static void dd_progress(void) {
    if (dd_status != DS_PROGRESS) return;
    if (dd_elapsed_ns(&dd_tlast) < 1000000000ull) return;
    clock_gettime(CLOCK_MONOTONIC, &dd_tlast);
    dd_xfer_line(1);
}

/* skip RECORDS of IBS then BYTES of input: by seeking, or by reading */
static void dd_skip(uint64_t records, uint64_t bytes, unsigned char *buf) {
    if (dd_in_isdir) return;
    struct stat st;
    uint64_t off = records * dd_ibs + bytes;
    off_t cur = lseek(dd_ifd, 0, SEEK_CUR);
    if (cur >= 0) {
        if (fstat(dd_ifd, &st) == 0 && S_ISREG(st.st_mode) && (uint64_t)(st.st_size - cur) < off) {
            lseek(dd_ifd, 0, SEEK_END);
            su_error(0, 0, "%s: cannot skip to specified offset", su_quotef(dd_in_name));
            return;
        }
        if (lseek(dd_ifd, (off_t)off, SEEK_CUR) >= 0) return;
    }
    while (records || bytes) {
        ssize_t r = dd_read(buf, records ? dd_ibs : bytes);
        if (r < 0) {
            su_error(0, errno, "error reading %s", su_quoteaf(dd_in_name));
            if (dd_conv & DC_NOERROR) dd_stats();
            exit(1);
        }
        if (r == 0) {
            su_error(0, 0, "%s: cannot skip to specified offset", su_quotef(dd_in_name));
            return;
        }
        if (records) records--; else bytes = 0;
    }
}

static int do_dd(int argc, char **argv) {
    static const struct su_option longs[] = { SU_LONG_HV, { 0, 0, 0, 0 } };
    int c;
    while ((c = su_getopt_long(argc, argv, "", longs, 0)) != -1) {
        switch (c) {
        case SU_HELP_OPT:
            printf("Usage: %s [OPERAND]...\n"
                   "  or:  %s OPTION\n"
                   "Copy a file in blocks, converting it as the operands ask.\n"
                   "\n"
                   "  bs=BYTES        read and write BYTES at a time (both sizes)\n"
                   "  cbs=BYTES       the record size for block and unblock\n"
                   "  conv=CONVS      convert the data as the comma list says\n"
                   "  count=N         copy N input blocks only\n"
                   "  ibs=BYTES       read BYTES at a time (512)\n"
                   "  if=FILE         read FILE, not standard input\n"
                   "  iflag=FLAGS     input flags, a comma list\n"
                   "  obs=BYTES       write BYTES at a time (512)\n"
                   "  of=FILE         write FILE, not standard output\n"
                   "  oflag=FLAGS     output flags, a comma list\n"
                   "  seek=N, oseek=N skip N obs-sized blocks of output first\n"
                   "  skip=N, iseek=N skip N ibs-sized blocks of input first\n"
                   "  status=LEVEL    none, noxfer or progress\n"
                   "\n"
                   "N and BYTES may carry c 1, w 2, b 512, kB 1000, K 1024, MB, M,\n"
                   "and so on through Q, and be multiplied with x (2x3).\n"
                   "\n"
                   "CONVS: ascii ebcdic ibm block unblock lcase ucase sparse swab\n"
                   "sync excl nocreat notrunc noerror fdatasync fsync.\n"
                   "FLAGS: append direct directory dsync sync fullblock nocache\n"
                   "noatime noctty nofollow nonblock count_bytes skip_bytes\n"
                   "seek_bytes.\n"
                   "\n"
                   "SIGUSR1 prints the counts so far and carries on.\n"
                   "\n"
                   "      --help      show this help and exit\n"
                   "      --version   show the version and exit\n",
                   su_name, su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    uint64_t bs = 0, skip = 0, seek = 0, count = UINT64_MAX;
    int have_count = 0;
    const char *in_file = 0, *out_file = 0;
    for (int i = su_optind; i < argc; i++) {
        const char *a = argv[i], *eq = strchr(a, '=');
        if (!eq) {
            su_error(0, 0, "unrecognized operand %s", su_quote(a));
            su_try_help(1);
        }
        size_t kn = (size_t)(eq - a);
        const char *v = eq + 1;
#define DD_IS(k) (kn == strlen(k) && !strncmp(a, k, kn))
        if (DD_IS("if")) in_file = v;
        else if (DD_IS("of")) out_file = v;
        else if (DD_IS("conv")) dd_conv |= dd_symbols(v, dd_convs, "invalid conversion", 0);
        else if (DD_IS("iflag")) dd_iflag |= dd_symbols(v, dd_flags, "invalid input flag", 0);
        else if (DD_IS("oflag")) dd_oflag |= dd_symbols(v, dd_flags, "invalid output flag", 0);
        else if (DD_IS("status")) {
            static const struct dd_sym lv[] = {
                { "none", DS_NONE }, { "noxfer", DS_NOXFER }, { "progress", DS_PROGRESS }, { 0, 0 }
            };
            dd_status = dd_symbols(v, lv, "invalid status level", 1);
        }
        else if (DD_IS("ibs")) dd_ibs = (size_t)dd_number(v, 1);
        else if (DD_IS("obs")) dd_obs = (size_t)dd_number(v, 1);
        else if (DD_IS("bs")) bs = dd_number(v, 1);
        else if (DD_IS("cbs")) dd_cbs = (size_t)dd_number(v, 1);
        else if (DD_IS("skip") || DD_IS("iseek")) skip = dd_number(v, 0);
        else if (DD_IS("seek") || DD_IS("oseek")) seek = dd_number(v, 0);
        else if (DD_IS("count")) { count = dd_number(v, 0); have_count = 1; }
        else {
            su_error(0, 0, "unrecognized operand %s", su_quote(a));
            su_try_help(1);
        }
#undef DD_IS
    }
    if (dd_oflag & DF_FULLBLOCK) {
        su_error(0, 0, "invalid output flag: %s", su_quote("fullblock"));
        su_try_help(1);
    }
    if (!dd_cbs) dd_conv &= ~(DC_BLOCK | DC_UNBLOCK);
    int cs = dd_conv & (DC_ASCII | DC_EBCDIC | DC_IBM);
    if (cs & (cs - 1)) su_error(1, 0, "cannot combine any two of {ascii,ebcdic,ibm}");
    if ((dd_conv & DC_BLOCK) && (dd_conv & DC_UNBLOCK)) su_error(1, 0, "cannot combine block and unblock");
    if ((dd_conv & DC_LCASE) && (dd_conv & DC_UCASE)) su_error(1, 0, "cannot combine lcase and ucase");
    if ((dd_conv & DC_EXCL) && (dd_conv & DC_NOCREAT)) su_error(1, 0, "cannot combine excl and nocreat");
    if (((dd_iflag & DF_DIRECT) && (dd_iflag & DF_NOCACHE)) ||
        ((dd_oflag & DF_DIRECT) && (dd_oflag & DF_NOCACHE)))
        su_error(1, 0, "cannot combine direct and nocache");
    if (bs) dd_ibs = dd_obs = (size_t)bs;
    int twobufs = !bs || (dd_conv & (DC_ASCII | DC_EBCDIC | DC_IBM | DC_BLOCK | DC_UNBLOCK |
                                     DC_LCASE | DC_UCASE | DC_SWAB));

    for (int i = 0; i < 256; i++) dd_tr[i] = (unsigned char)i;
    if (dd_conv & DC_ASCII)
        for (int i = 0; i < 256; i++) dd_tr[i] = dd_ebcdic_to_ascii[dd_tr[i]];
    if (dd_conv & DC_UCASE)
        for (int i = 0; i < 256; i++) dd_tr[i] = (unsigned char)toupper(dd_tr[i]);
    else if (dd_conv & DC_LCASE)
        for (int i = 0; i < 256; i++) dd_tr[i] = (unsigned char)tolower(dd_tr[i]);
    if (dd_conv & DC_EBCDIC) {
        for (int i = 0; i < 256; i++) dd_tr[i] = dd_ascii_to_ebcdic[dd_tr[i]];
        dd_nl = dd_ascii_to_ebcdic['\n'];
        dd_sp = dd_ascii_to_ebcdic[' '];
    } else if (dd_conv & DC_IBM) {
        for (int i = 0; i < 256; i++) dd_tr[i] = dd_ascii_to_ibm[dd_tr[i]];
        dd_nl = dd_ascii_to_ibm['\n'];
        dd_sp = dd_ascii_to_ibm[' '];
    }
    dd_translate = (dd_conv & (DC_ASCII | DC_EBCDIC | DC_IBM | DC_LCASE | DC_UCASE)) != 0;

    if (in_file) {
        dd_in_name = in_file;
        struct stat st;
        int fd = open(in_file, O_RDONLY);
        if (fd < 0 && stat(in_file, &st) == 0 && S_ISDIR(st.st_mode)) dd_in_isdir = 1;
        else if (fd < 0) su_error(1, errno, "failed to open %s", su_quoteaf(in_file));
        dd_ifd = fd;
    }
    if (out_file) {
        dd_out_name = out_file;
        struct stat st;
        if (stat(out_file, &st) == 0 && S_ISDIR(st.st_mode))
            su_error(1, EISDIR, "failed to open %s", su_quoteaf(out_file));
        int fl = O_WRONLY | ((dd_conv & DC_NOCREAT) ? 0 : O_CREAT) |
                 ((dd_conv & DC_EXCL) ? O_EXCL : 0) |
                 ((seek || (dd_conv & DC_NOTRUNC)) ? 0 : O_TRUNC) |
                 ((dd_oflag & DF_APPEND) ? O_APPEND : 0);
        int fd = open(out_file, fl, 0666);
        if (fd < 0) su_error(1, errno, "failed to open %s", su_quoteaf(out_file));
        dd_ofd = fd;
        uint64_t size = (dd_oflag & DF_SEEK_BYTES) ? seek : seek * dd_obs;
        if (seek && !(dd_conv & DC_NOTRUNC) && fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
            ftruncate(fd, (off_t)size) != 0)
            su_error(1, errno, "failed to truncate to %" PRIu64 " bytes in output file %s",
                     size, su_quoteaf(out_file));
    }

    unsigned char *ibase = (unsigned char *)malloc(dd_ibs + 1);
    if (!ibase) {
        char h[32];
        if (!dd_human(dd_ibs, 1024, h, sizeof h)) snprintf(h, sizeof h, "%zu B", dd_ibs);
        su_error(1, 0, "memory exhausted by input buffer of size %zu bytes (%s)", dd_ibs, h);
    }
    unsigned char *ibuf = ibase + 1;
    if (twobufs) {
        dd_obuf = (unsigned char *)malloc(dd_obs);
        if (!dd_obuf) {
            char h[32];
            if (!dd_human(dd_obs, 1024, h, sizeof h)) snprintf(h, sizeof h, "%zu B", dd_obs);
            su_error(1, 0, "memory exhausted by output buffer of size %zu bytes (%s)", dd_obs, h);
        }
    }

    dd_catch(SIGINT, dd_on_int);
    dd_catch(SIGUSR1, dd_on_info);
    clock_gettime(CLOCK_MONOTONIC, &dd_t0);
    dd_tlast = dd_t0;

    if (skip) {
        if (dd_iflag & DF_SKIP_BYTES) dd_skip(skip / dd_ibs, skip % dd_ibs, ibuf);
        else dd_skip(skip, 0, ibuf);
    }
    if (seek) {
        uint64_t bytes = (dd_oflag & DF_SEEK_BYTES) ? seek : seek * dd_obs;
        if (lseek(dd_ofd, (off_t)bytes, SEEK_CUR) < 0) {
            static const unsigned char zero[4096];
            while (bytes) {
                size_t k = bytes < sizeof zero ? (size_t)bytes : sizeof zero;
                ssize_t w = write(dd_ofd, zero, k);
                if (w <= 0) {
                    su_error(0, errno, "writing to %s", su_quoteaf(dd_out_name));
                    dd_quit(1);
                }
                bytes -= (uint64_t)w;
            }
        }
    }

    uint64_t max_records = count, max_bytes = 0;
    if (have_count && (dd_iflag & DF_COUNT_BYTES)) {
        max_records = count / dd_ibs;
        max_bytes = count % dd_ibs;
    }
    int rc = 0, saved = -1;
    unsigned char pad = (dd_conv & (DC_BLOCK | DC_UNBLOCK)) ? ' ' : 0;
    for (;;) {
        uint64_t done = dd_r_full + dd_r_partial;
        if (have_count && done >= max_records + (max_bytes ? 1 : 0)) break;
        dd_signals();
        dd_progress();
        size_t want = (have_count && done >= max_records) ? (size_t)max_bytes : dd_ibs;
        ssize_t n = dd_read(ibuf, want);
        if (n < 0) {
            su_error(0, errno, "error reading %s", su_quoteaf(dd_in_name));
            if (!(dd_conv & DC_NOERROR) || dd_in_isdir || lseek(dd_ifd, (off_t)dd_ibs, SEEK_CUR) < 0) {
                rc = 1;
                break;
            }
            dd_stats();
            if (!(dd_conv & DC_SYNC)) continue;
            n = 0;
        } else if (n == 0)
            break;
        if ((size_t)n == dd_ibs) dd_r_full++; else dd_r_partial++;
        if ((size_t)n < dd_ibs && (dd_conv & DC_SYNC)) {
            memset(ibuf + n, pad, dd_ibs - (size_t)n);
            n = (ssize_t)dd_ibs;
        }
        if (!twobufs) { dd_write(ibuf, (size_t)n); continue; }
        unsigned char *p = ibuf;
        size_t len = (size_t)n;
        if (dd_conv & DC_SWAB) {
            if (saved >= 0) { p = ibase; p[0] = (unsigned char)saved; len++; saved = -1; }
            if (len & 1) saved = p[--len];
            for (size_t i = 0; i + 1 < len; i += 2) {
                unsigned char t = p[i];
                p[i] = p[i + 1];
                p[i + 1] = t;
            }
        }
        if (dd_translate)
            for (size_t i = 0; i < len; i++) p[i] = dd_tr[p[i]];
        if (dd_conv & DC_BLOCK) dd_block(p, len);
        else if (dd_conv & DC_UNBLOCK) dd_unblock(p, len);
        else dd_out(p, len);
    }
    if (twobufs) {
        if (saved >= 0) {
            unsigned char ch = (unsigned char)saved;
            if (dd_conv & DC_BLOCK) dd_block(&ch, 1);
            else if (dd_conv & DC_UNBLOCK) dd_unblock(&ch, 1);
            else dd_outc(ch);
        }
        if ((dd_conv & DC_BLOCK) && dd_col > 0)
            for (; dd_col < dd_cbs; dd_col++) dd_outc(dd_sp);
        if ((dd_conv & DC_UNBLOCK) && dd_col > 0) dd_outc(dd_nl);
        if (dd_ofill) { dd_write(dd_obuf, dd_ofill); dd_ofill = 0; }
    }
    if (dd_final_seek) {
        off_t end = lseek(dd_ofd, 0, SEEK_CUR);
        if (end >= 0 && ftruncate(dd_ofd, end) != 0) {
            su_error(0, errno, "failed to truncate to %" PRIu64 " bytes in output file %s",
                     (uint64_t)end, su_quoteaf(dd_out_name));
            rc = 1;
        }
    }
    if ((dd_conv & (DC_FSYNC | DC_FDATASYNC)) && sync_all() != 0) {
        su_error(0, errno, "fsync failed for %s", su_quoteaf(dd_out_name));
        rc = 1;
    }
    if (out_file && close(dd_ofd) != 0) {
        su_error(0, errno, "closing output file %s", su_quoteaf(dd_out_name));
        rc = 1;
    }
    dd_stats();
    return rc;
}
