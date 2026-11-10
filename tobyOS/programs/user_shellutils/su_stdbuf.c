/* su_stdbuf.c -- stdbuf, as GNU coreutils 9.7's (logs/utilgate/staging/
 * stdbuf.sh): stdbuf OPTION... COMMAND [ARG]...
 *
 * -i/--input, -o/--output, -e/--error MODE: "L" for line buffering (not
 * for stdin: "line buffering stdin is meaningless"), or a size -- digits
 * (blanks and '+' first allowed) and an optional K M G T P E Z Y R Q
 * (k too), powers of 1024, or with B (KB, kB) powers of 1000, or with iB
 * the same as without; 0 and 0K are unbuffered. Anything else is
 * "invalid mode 'X'", and a size past what fits is "invalid mode 'X':
 * EOVERFLOW". The last option for a stream wins. Options end at the
 * first operand. No COMMAND: "missing operand"; no MODE: "you must
 * specify a buffering mode option". All of these exit 125.
 *
 * Each MODE is put in the environment as GNU's does -- _STDBUF_I, _STDBUF_O,
 * _STDBUF_E = L, or the size in bytes -- and COMMAND is run with execvp:
 * "failed to run command 'X': strerror", 127 when it does not exist, 126
 * when it cannot be run.
 *
 * HOW IT TAKES: GNU's stdbuf makes the buffering happen by adding its
 * libstdbuf.so to LD_PRELOAD, so the command's C library calls setvbuf
 * before main. A native tobyOS program is linked statically with libtoby
 * and loads nothing, so libtoby reads _STDBUF_* itself at startup, as
 * libstdbuf would, its words included ("invalid buffering mode X for
 * stdout", "failed to allocate a N byte stdio buffer"; finding 215 --
 * until then the buffering of COMMAND never changed). LD_PRELOAD is left
 * alone: a library that is not there would only make every dynamically
 * linked Linux program warn, so a Linux COMMAND keeps its own buffering.
 *
 * The old stdbuf skipped its options, said buffering could not be changed,
 * and ran the command. */

/* MODE: 0 and *OUT ("L" or a number), 1 invalid, 2 too large */
static int sy_sbmode(const char *s, char *out, size_t cap) {
    if (!strcmp(s, "L")) { snprintf(out, cap, "L"); return 0; }
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    if (*p == '+') p++;
    uintmax_t v = 1;
    if (isdigit((unsigned char)*p)) {
        v = 0;
        for (; isdigit((unsigned char)*p); p++) {
            unsigned d = (unsigned)(*p - '0');
            if (v > (UINTMAX_MAX - d) / 10) {
                /* the digits alone are too many: a suffix after them is
                 * not looked at */
                return 2;
            }
            v = v * 10 + d;
        }
    } else if (!*p || !strchr("kKMGTPEZYRQ", *p)) {
        return 1;
    }
    if (*p) {
        static const char letters[] = "KMGTPEZYRQ";
        char L = *p == 'k' ? 'K' : *p;
        const char *at = strchr(letters, L);
        if (!at) return 1;
        int power = (int)(at - letters) + 1;
        unsigned base = 1024;
        const char *rest = p + 1;
        if (rest[0] == 'B') { base = 1000; rest++; }
        else if (rest[0] == 'i' && rest[1] == 'B') rest += 2;
        if (*rest) return 1;
        int over = 0;
        for (int i = 0; i < power; i++) {
            if (v && v > UINTMAX_MAX / base) over = 1;
            else v *= base;
        }
        if (over) return 2;
    }
    snprintf(out, cap, "%ju", v);
    return 0;
}

static int do_stdbuf(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "input", SU_REQ_ARG, 0, 'i' },
        { "output", SU_REQ_ARG, 0, 'o' },
        { "error", SU_REQ_ARG, 0, 'e' },
        SU_LONG_HV, { 0, 0, 0, 0 }
    };
    char mode[3][32] = { "", "", "" };
    int c;
    while ((c = su_getopt_long(argc, argv, "+i:o:e:", longs, 0)) != -1) {
        int k = c == 'i' ? 0 : c == 'o' ? 1 : c == 'e' ? 2 : -1;
        if (k >= 0) {
            if (k == 0 && !strcmp(su_optarg, "L")) {
                su_error(0, 0, "line buffering stdin is meaningless");
                su_try_help(125);
            }
            int r = sy_sbmode(su_optarg, mode[k], sizeof mode[k]);
            if (r) su_error(125, r == 2 ? EOVERFLOW : 0, "invalid mode %s", su_quote(su_optarg));
            continue;
        }
        switch (c) {
        case SU_HELP_OPT:
            printf("Usage: %s OPTION... COMMAND\n"
                   "Run COMMAND with the buffering of its standard streams changed.\n"
                   "\n"
                   "An argument a long option needs, its short form needs too.\n"
                   "  -i, --input=MODE   the buffering of standard input\n"
                   "  -o, --output=MODE  the buffering of standard output\n"
                   "  -e, --error=MODE   the buffering of standard error\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n"
                   "\n"
                   "MODE 'L' is line buffering (not for standard input); MODE '0' is no\n"
                   "buffering; any other MODE is the size of a full buffer, in bytes,\n"
                   "with an optional unit: K M G T P E Z Y R Q for powers of 1024 (KiB,\n"
                   "MiB... the same), KB MB... for powers of 1000.\n"
                   "\n"
                   "Programs that set their own buffering, or do not use stdio at all,\n"
                   "are not changed. On tobyOS the modes reach COMMAND only as the\n"
                   "_STDBUF_I, _STDBUF_O and _STDBUF_E environment variables.\n"
                   "\n"
                   "The exit status is 125 when stdbuf itself fails, 126 when COMMAND\n"
                   "cannot be run, 127 when it is not found, else COMMAND's.\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(125);
        }
    }
    if (su_optind >= argc) {
        su_error(0, 0, "missing operand");
        su_try_help(125);
    }
    if (!mode[0][0] && !mode[1][0] && !mode[2][0]) {
        su_error(0, 0, "you must specify a buffering mode option");
        su_try_help(125);
    }
    static const char *const names[3] = { "_STDBUF_I", "_STDBUF_O", "_STDBUF_E" };
    for (int k = 0; k < 3; k++)
        if (mode[k][0] && setenv(names[k], mode[k], 1) != 0)
            su_error(125, errno, "failed to update the environment with %s",
                     su_quote(names[k]));
    execvp(argv[su_optind], &argv[su_optind]);
    int e = errno;
    su_error(e == ENOENT ? 127 : 126, e, "failed to run command %s", su_quote(argv[su_optind]));
    return 126;
}
