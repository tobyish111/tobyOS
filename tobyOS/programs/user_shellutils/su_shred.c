/* su_shred.c -- shred, as GNU coreutils' (logs/utilgate/shred.sh):
 * overwrite each FILE (- is standard output) so its old contents are
 * hard to recover, and with -u remove it.
 *
 *   -n N        N passes (3); -z one more, of zeros, last
 *   -s SIZE     overwrite SIZE bytes (0x.., 0.., suffixes b c B k K kB
 *               KiB M ... as GNU's); else the file's size, rounded up to
 *               its st_blksize unless -x -- a 2-byte file becomes 4096
 *   -f          a file not writable is made writable (mode 0200) first
 *   -u, --remove[=unlink|wipe|wipesync]  remove it after: renamed to
 *               zeros of its name's length, then of each shorter length
 *               (the next free name in 0-9 a-z A-Z _ when one is taken),
 *               then unlinked -- unlink alone with =unlink
 *   -v          "shred: f: pass 1/3 (random)..." and the removal's steps
 *   --random-source=FILE  the random bytes, read as they are, from FILE
 *
 * WHICH PASSES (measured: logs/b30/shred-*): GNU takes its passes from a
 * fixed list of blocks, in order and around again -- 2 random passes;
 * 000000 ffffff; 555555 aaaaaa; 1 random; 249249 492492 6db6db 924924
 * b6db6d db6db6; the twelve 111111 ... eeeeee; 1 random; the first eight
 * again with their top bit flipped (800000 ...); the twelve and 555555
 * aaaaaa so (911111 ...); 1 random -- a whole block while it fits, a random part of
 * one that does not when what is left is at least 2 and a third of the
 * block, else random passes for the rest. The random passes go first,
 * last and evenly between (1-based pass 1 + ceil(i(N-1)/(R-1))); the
 * patterns fill the rest in a random order. With no randomness at all
 * (--random-source=/dev/zero) every choice is the first, and the order is
 * GNU's exactly. */

static const uint32_t sh_blocks[] = {
    0xF0000002,                                     /* 2 random */
    2, 0x000000, 0xffffff,
    2, 0x555555, 0xaaaaaa,
    0xF0000001,                                     /* 1 random */
    6, 0x249249, 0x492492, 0x6db6db, 0x924924, 0xb6db6d, 0xdb6db6,
    12, 0x111111, 0x222222, 0x333333, 0x444444, 0x666666, 0x777777,
        0x888888, 0x999999, 0xbbbbbb, 0xcccccc, 0xdddddd, 0xeeeeee,
    0xF0000001,
    8, 0x800000, 0xa49249, 0xc92492, 0xedb6db, 0x124924, 0x36db6d, 0x5b6db6, 0x7fffff,
    14, 0x911111, 0xa22222, 0xb33333, 0xc44444, 0xd55555, 0xe66666, 0xf77777,
        0x088888, 0x199999, 0x2aaaaa, 0x3bbbbb, 0x4ccccc, 0x5ddddd, 0x6eeeee,
    0xF0000001,
};
#define SH_RANDOM 0xFFFFFFFFu

/* ---- randomness ---------------------------------------------------------
 * From --random-source FILE, its bytes as they are ("'FILE': end of file"
 * when it runs out); else xoshiro256** seeded from /dev/urandom. */
static const char *sh_srcname;
static int sh_srcfd = -1;
static uint64_t sh_x[4];

static uint64_t sh_rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static uint64_t sh_next(void) {
    uint64_t r = sh_rotl(sh_x[1] * 5, 7) * 9, t = sh_x[1] << 17;
    sh_x[2] ^= sh_x[0]; sh_x[3] ^= sh_x[1]; sh_x[1] ^= sh_x[2]; sh_x[0] ^= sh_x[3];
    sh_x[2] ^= t; sh_x[3] = sh_rotl(sh_x[3], 45);
    return r;
}

static void sh_random_init(void) {
    if (sh_srcname) {
        sh_srcfd = open(sh_srcname, O_RDONLY);
        if (sh_srcfd < 0) su_error(1, errno, "%s", su_quotef(sh_srcname));
        return;
    }
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) { if (read(fd, sh_x, sizeof sh_x) != (ssize_t)sizeof sh_x) memset(sh_x, 0, sizeof sh_x); close(fd); }
    if (!(sh_x[0] | sh_x[1] | sh_x[2] | sh_x[3])) {
        sh_x[0] = (uint64_t)time(0) ^ 0x9e3779b97f4a7c15ULL;
        sh_x[1] = (uint64_t)getpid() * 0xbf58476d1ce4e5b9ULL;
        sh_x[2] = 0x94d049bb133111ebULL; sh_x[3] = 1;
    }
}

static void sh_random_bytes(unsigned char *b, size_t n) {
    if (sh_srcfd >= 0) {
        while (n) {
            ssize_t r = read(sh_srcfd, b, n);
            if (r < 0) su_error(1, errno, "%s: read error", su_quotef(sh_srcname));
            if (r == 0) su_error(1, 0, "%s: end of file", su_quote(sh_srcname));
            b += r;
            n -= (size_t)r;
        }
        return;
    }
    while (n >= 8) { uint64_t v = sh_next(); memcpy(b, &v, 8); b += 8; n -= 8; }
    if (n) { uint64_t v = sh_next(); memcpy(b, &v, n); }
}

/* a number in [0, k) */
static size_t sh_choose(size_t k) {
    if (k <= 1) return 0;
    unsigned char b[8];
    size_t nb = 0;
    for (size_t m = k - 1; m; m >>= 8) nb++;
    for (;;) {
        sh_random_bytes(b, nb);
        uint64_t v = 0;
        for (size_t i = 0; i < nb; i++) v = (v << 8) | b[i];
        uint64_t span = nb >= 8 ? 0 : (uint64_t)1 << (8 * nb);
        if (!span) return (size_t)(v % k);
        if (v < span - span % k) return (size_t)(v % k);
    }
}

/* the N passes, as GNU picks and orders them, into P */
static void sh_passes(uint32_t *p, size_t n) {
    size_t top = 0, rnd = 0, left = n;
    size_t nb = sizeof sh_blocks / sizeof sh_blocks[0];
    for (size_t i = 0; left;) {
        if (i >= nb) i = 0;
        uint32_t w = sh_blocks[i++];
        if (w >= 0xF0000000u) {
            size_t k = w & 0xff;
            if (k >= left) { rnd += left; break; }
            rnd += k;
            left -= k;
            continue;
        }
        size_t k = w;
        if (k <= left) {
            for (size_t j = 0; j < k; j++) p[top++] = sh_blocks[i + j];
            left -= k;
        } else if (left < 2 || 3 * left < k) {
            rnd += left;
            break;
        } else {
            /* LEFT of the K, each in turn taken with the chance it must
             * have for the count to come out (selection sampling) */
            for (size_t j = 0; left; j++) {
                if (left == k - j || sh_choose(k - j) < left) { p[top++] = sh_blocks[i + j]; left--; }
            }
            break;
        }
        i += k;
    }
    /* the random passes at their places; a pattern there moves to the end */
    size_t r = 0;
    for (size_t pos = 0; pos < n; pos++) {
        size_t want = r < rnd ? (rnd == 1 ? 0 : (r * (n - 1) + (rnd - 2)) / (rnd - 1)) : (size_t)-1;
        if (pos == want) {
            if (pos < top) p[top] = p[pos];
            top++;
            p[pos] = SH_RANDOM;
            r++;
        } else {
            size_t j = pos + sh_choose(top - pos);
            uint32_t t = p[pos]; p[pos] = p[j]; p[j] = t;
        }
    }
}

/* ---- one file ----------------------------------------------------------- */

static struct {
    size_t npass;
    int zero, exact, force, verbose;
    int remove;             /* 0, 1 unlink, 2 wipe, 3 wipesync */
    int have_size;
    uintmax_t size;
} sh;

static int sh_write_pass(int fd, const char *name, uint32_t pat, uintmax_t size, int known) {
    enum { BUF = 3 * 4096 * 4 };
    static unsigned char buf[BUF];
    if (pat != SH_RANDOM)
        for (size_t i = 0; i < BUF; i += 3) {
            buf[i] = (unsigned char)(pat >> 16);
            buf[i + 1] = (unsigned char)(pat >> 8);
            buf[i + 2] = (unsigned char)pat;
        }
    if (lseek(fd, 0, SEEK_SET) < 0 && known) {
        su_error(0, errno, "%s: cannot rewind", su_quotef(name));
        return 1;
    }
    uintmax_t done = 0;
    while (!known || done < size) {
        size_t n = BUF;
        if (known && size - done < n) n = (size_t)(size - done);
        if (pat == SH_RANDOM) sh_random_bytes(buf, n);
        ssize_t w = write(fd, buf, n);
        if (w <= 0) {
            if (!known && (w == 0 || errno == ENOSPC)) break;
            su_error(0, errno, "%s: error writing at offset %ju", su_quotef(name), done);
            return 1;
        }
        done += (uintmax_t)w;
    }
#ifdef SU_HOST
    fsync(fd);
#else
    (void)sc0(ABI_SYS_SYNC);  /* libtoby has no fsync: the whole cache */
#endif
    return 0;
}

/* NAME's last component renamed through ever shorter names of zeros,
 * then unlinked; 0, or 1 */
static int sh_wipename(const char *name) {
    static const char set[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_.";
    char cur[4096], cand[4096];
    snprintf(cur, sizeof cur, "%s", name);
    if (sh.verbose) su_error(0, 0, "%s: removing", su_quotef(name));
    if (sh.remove >= 2) {
        const char *slash = strrchr(cur, '/');
        size_t dl = slash ? (size_t)(slash - cur + 1) : 0;
        size_t len = strlen(cur + dl);
        for (; len > 0; len--) {
            memcpy(cand, cur, dl);
            memset(cand + dl, '0', len);
            cand[dl + len] = 0;
            int moved = 0, failed = 0;
            for (;;) {
                struct stat st;
                if (strcmp(cand + dl, ".") && lstat(cand, &st) != 0 && errno == ENOENT) {
                    if (rename(cur, cand) == 0) moved = 1;
                    else failed = 1;
                    break;
                }
                /* the next name: the last character up, carrying */
                size_t k = len;
                while (k > 0) {
                    char *c = &cand[dl + k - 1];
                    const char *at = strchr(set, *c);
                    if (at && at[1]) { *c = at[1]; break; }
                    *c = set[0];
                    k--;
                }
                if (k == 0) break;          /* every name of this length is taken */
            }
            if (failed) break;
            if (moved) {
                if (sh.verbose) su_error(0, 0, "%s: renamed to %s", su_quotef(cur), su_quotef(cand));
                snprintf(cur, sizeof cur, "%s", cand);
            }
        }
    }
    if (unlink(cur) != 0) {
        su_error(0, errno, "%s: failed to remove", su_quotef(name));
        return 1;
    }
    if (sh.verbose) su_error(0, 0, "%s: removed", su_quotef(name));
    return 0;
}

static int sh_file(const char *name) {
    int fd;
    int is_stdout = !strcmp(name, "-");
    if (is_stdout) {
        fd = 1;
        /* (natively too since finding 235: libtoby's fcntl was a stub) */
        int fl = fcntl(1, F_GETFL);
        if (fl >= 0 && (fl & O_APPEND)) {
            su_error(0, 0, "%s: cannot shred append-only file descriptor", su_quotef(name));
            return 1;
        }
    } else {
        fd = open(name, O_WRONLY | O_NOCTTY);
        if (fd < 0 && errno == EACCES && sh.force && chmod(name, S_IWUSR) == 0)
            fd = open(name, O_WRONLY | O_NOCTTY);
        if (fd < 0) {
            su_error(0, errno, "%s: failed to open for writing", su_quotef(name));
            return 1;
        }
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        su_error(0, errno, "%s: fstat failed", su_quotef(name));
        if (!is_stdout) close(fd);
        return 1;
    }
    if (!S_ISREG(st.st_mode) && !S_ISCHR(st.st_mode) && !S_ISBLK(st.st_mode)) {
        su_error(0, 0, "%s: invalid file type", su_quotef(name));
        if (!is_stdout) close(fd);
        return 1;
    }
    uintmax_t size = 0;
    int known = 1;
    if (sh.have_size) size = sh.size;
    else if (S_ISREG(st.st_mode)) {
        size = (uintmax_t)st.st_size;
        uintmax_t bs = st.st_blksize > 0 ? (uintmax_t)st.st_blksize : 512;
        if (!sh.exact && size % bs) size += bs - size % bs;
    } else {
        off_t e = lseek(fd, 0, SEEK_END);
        if (e > 0) size = (uintmax_t)e;
        else known = 0;
    }
    int rc = 0;
    if (size > 0 || !known) {
        size_t total = sh.npass + (sh.zero ? 1 : 0);
        uint32_t *p = (uint32_t *)malloc((total ? total : 1) * sizeof *p);
        if (!p) su_error(1, ENOMEM, "%s", su_quotef(name));
        sh_passes(p, sh.npass);
        if (sh.zero) p[sh.npass] = 0;
        for (size_t i = 0; i < total && !rc; i++) {
            if (sh.verbose) {
                char what[16];
                if (p[i] == SH_RANDOM) snprintf(what, sizeof what, "random");
                else snprintf(what, sizeof what, "%06x", (unsigned)p[i]);
                su_error(0, 0, "%s: pass %zu/%zu (%s)...", su_quotef(name), i + 1, total, what);
            }
            rc = sh_write_pass(fd, name, p[i], size, known);
        }
        free(p);
    }
    if (sh.remove && !is_stdout && !rc && S_ISREG(st.st_mode) && ftruncate(fd, 0) != 0) {
        /* its bytes are overwritten already; the unlink still happens */
    }
    if (!is_stdout) close(fd);
    if (sh.remove && !is_stdout && !rc) rc = sh_wipename(name);
    return rc;
}

/* SIZE as GNU shred reads it: base 0, then one of its suffixes */
static int sh_parse_size(const char *s, uintmax_t *out, int *over) {
    const char *end;
    int neg;
    uintmax_t v = pf_mag(s, &end, &neg, over);
    if (end == s || neg) return -1;
    if (*over) return -1;
    if (!*end) { *out = v; return 0; }
    uintmax_t mul = 1, base = 1024;
    int power = 0;
    const char *p = end;
    switch (*p) {
    case 'b': mul = 512; break;
    case 'c': break;
    case 'B': mul = 1024; break;
    case 'k': case 'K': power = 1; break;
    case 'M': power = 2; break; case 'G': power = 3; break; case 'T': power = 4; break;
    case 'P': power = 5; break; case 'E': power = 6; break; case 'Z': power = 7; break;
    case 'Y': power = 8; break; case 'R': power = 9; break; case 'Q': power = 10; break;
    default: return -1;
    }
    p++;
    if (power) {
        if (p[0] == 'B') { base = 1000; p++; }
        else if (p[0] == 'i' && p[1] == 'B') p += 2;
    }
    if (*p) return -1;
    for (int i = 0; i < power; i++) mul = mul > UINTMAX_MAX / base ? (*over = 1, UINTMAX_MAX) : mul * base;
    if (*over || (mul && v > UINTMAX_MAX / mul)) { *over = 1; return -1; }
    *out = v * mul;
    return 0;
}

static int do_shred(int argc, char **argv) {
    enum { RANDOM_SOURCE_OPT = 0x100, REMOVE_OPT };
    static const struct su_option longs[] = {
        { "exact", SU_NO_ARG, 0, 'x' },
        { "force", SU_NO_ARG, 0, 'f' },
        { "iterations", SU_REQ_ARG, 0, 'n' },
        { "size", SU_REQ_ARG, 0, 's' },
        { "random-source", SU_REQ_ARG, 0, RANDOM_SOURCE_OPT },
        { "remove", SU_OPT_ARG, 0, REMOVE_OPT },
        { "verbose", SU_NO_ARG, 0, 'v' },
        { "zero", SU_NO_ARG, 0, 'z' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    memset(&sh, 0, sizeof sh);
    sh.npass = 3;
    int c;
    while ((c = su_getopt_long(argc, argv, "fn:s:uvxz", longs, 0)) != -1) {
        switch (c) {
        case 'f': sh.force = 1; break;
        case 'n': {
            const char *p = su_optarg;
            while (isspace((unsigned char)*p)) p++;
            if (*p == '+') p++;
            uintmax_t v = 0;
            int over = 0;
            if (!isdigit((unsigned char)*p)) su_error(1, 0, "invalid number of passes: %s", su_quote(su_optarg));
            for (; isdigit((unsigned char)*p); p++) {
                if (v > (UINTMAX_MAX - (uintmax_t)(*p - '0')) / 10) over = 1;
                else v = v * 10 + (uintmax_t)(*p - '0');
            }
            if (*p) su_error(1, 0, "invalid number of passes: %s", su_quote(su_optarg));
            if (over || v > (uintmax_t)SIZE_MAX / sizeof(uint32_t) - 1)
                su_error(1, EOVERFLOW, "invalid number of passes: %s", su_quote(su_optarg));
            sh.npass = (size_t)v;
            break;
        }
        case 's': {
            int over = 0;
            if (sh_parse_size(su_optarg, &sh.size, &over) != 0 || (!over && sh.size > (uintmax_t)INTMAX_MAX)) {
                if (over || sh.size > (uintmax_t)INTMAX_MAX)
                    su_error(1, EOVERFLOW, "invalid file size: %s", su_quote(su_optarg));
                su_error(1, 0, "invalid file size: %s", su_quote(su_optarg));
            }
            sh.have_size = 1;
            break;
        }
        case 'u': sh.remove = 3; break;
        case REMOVE_OPT:
            if (!su_optarg || !strcmp(su_optarg, "wipesync")) sh.remove = 3;
            else if (!strcmp(su_optarg, "wipe")) sh.remove = 2;
            else if (!strcmp(su_optarg, "unlink")) sh.remove = 1;
            else {
                fflush(stdout);
                fprintf(stderr, "%s: invalid argument %s for %s\n"
                        "Valid arguments are:\n  - 'unlink'\n  - 'wipe'\n  - 'wipesync'\n",
                        su_short, su_quote(su_optarg), su_quote("--remove"));
                su_try_help(1);
            }
            break;
        case 'v': sh.verbose = 1; break;
        case 'x': sh.exact = 1; break;
        case 'z': sh.zero = 1; break;
        case RANDOM_SOURCE_OPT: sh_srcname = su_optarg; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]... FILE...\n"
                   "Overwrite each FILE again and again, so that what was in it is hard to\n"
                   "recover even with very expensive hardware probing.\n"
                   "\n"
                   "A FILE of - is standard output.\n"
                   "\n"
                   "  -f, --force           make a FILE writable first if it is not\n"
                   "  -n, --iterations=N    overwrite N times (3)\n"
                   "      --random-source=FILE  take the random bytes from FILE\n"
                   "  -s, --size=N          overwrite N bytes (K, M, G ... allowed)\n"
                   "  -u                    remove each FILE after overwriting it\n"
                   "      --remove[=HOW]    like -u; HOW is unlink (just unlink), wipe (rename\n"
                   "                        the name away first) or wipesync (wipe, syncing\n"
                   "                        each step: the default)\n"
                   "  -v, --verbose         report each pass\n"
                   "  -x, --exact           do not round the size up to a whole block (the\n"
                   "                        default for files that are not regular)\n"
                   "  -z, --zero            end with a pass of zeros, to hide the shredding\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "Nothing is removed without -u or --remove: shred is often pointed at a\n"
                   "device, which should stay. It assumes the filesystem and the hardware\n"
                   "overwrite in place, which many do not; a journal, a snapshot or a backup\n"
                   "can keep an old copy.\n", su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (su_optind >= argc) {
        su_error(0, 0, "missing file operand");
        su_try_help(1);
    }
    sh_random_init();
    int rc = 0;
    for (int i = su_optind; i < argc; i++) rc |= sh_file(argv[i]);
    return rc;
}
