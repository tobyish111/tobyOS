/* su_uname.c -- uname, as GNU coreutils' on a Debian or Ubuntu system:
 * the names asked for, always in the order kernel name, node name,
 * kernel release, kernel version, machine, processor, hardware platform,
 * operating system, whatever order the options came in; -s alone when
 * none is asked for; -a every one. The processor and the platform are the
 * machine, as Debian's uname says (upstream's says "unknown" and leaves
 * them out of -a). No operands. Measured on coreutils 9.7 (Ubuntu):
 * logs/utilgate/uname.sh.
 *
 * The values are tobyOS's: /proc/sys/kernel/{ostype,hostname,osrelease,
 * version}, x86_64, and "tobyOS" as the operating system. It took -p and
 * -i as the machine but printed it once, -o not at all, the whole first
 * line of /proc/version as the kernel version, and operands as nothing. */

static void uname_read(const char *path, char *out, size_t cap, const char *dflt) {
    char buf[1024];
    if (slurp(path, buf, sizeof buf) > 0) snprintf(out, cap, "%s", first_line(buf));
    else snprintf(out, cap, "%s", dflt);
}

static int do_uname(int argc, char **argv) {
    enum { S = 1, N = 2, R = 4, V = 8, M = 16, P = 32, I = 64, O = 128 };
    static const struct su_option longs[] = {
        { "all", SU_NO_ARG, 0, 'a' },
        { "kernel-name", SU_NO_ARG, 0, 's' },
        { "sysname", SU_NO_ARG, 0, 's' },
        { "nodename", SU_NO_ARG, 0, 'n' },
        { "kernel-release", SU_NO_ARG, 0, 'r' },
        { "release", SU_NO_ARG, 0, 'r' },
        { "kernel-version", SU_NO_ARG, 0, 'v' },
        { "machine", SU_NO_ARG, 0, 'm' },
        { "processor", SU_NO_ARG, 0, 'p' },
        { "hardware-platform", SU_NO_ARG, 0, 'i' },
        { "operating-system", SU_NO_ARG, 0, 'o' },
        SU_LONG_HV,
        { 0, 0, 0, 0 }
    };
    int want = 0, c;
    while ((c = su_getopt_long(argc, argv, "asnrvmpio", longs, 0)) != -1) {
        switch (c) {
        case 'a': want = S | N | R | V | M | P | I | O; break;
        case 's': want |= S; break;
        case 'n': want |= N; break;
        case 'r': want |= R; break;
        case 'v': want |= V; break;
        case 'm': want |= M; break;
        case 'p': want |= P; break;
        case 'i': want |= I; break;
        case 'o': want |= O; break;
        case SU_HELP_OPT:
            printf("Usage: %s [OPTION]...\n"
                   "Print facts about this system; with no OPTION, -s.\n"
                   "\n"
                   "  -a, --all                every fact below, in this order\n"
                   "  -s, --kernel-name        the kernel's name\n"
                   "  -n, --nodename           this machine's network name\n"
                   "  -r, --kernel-release     the kernel's release\n"
                   "  -v, --kernel-version     the kernel's version\n"
                   "  -m, --machine            the machine's hardware name\n"
                   "  -p, --processor          the processor type\n"
                   "  -i, --hardware-platform  the hardware platform\n"
                   "  -o, --operating-system   the operating system\n"
                   "      --help               show this help and exit\n"
                   "      --version            show the version and exit\n",
                   su_name);
            return 0;
        case SU_VERSION_OPT: return su_version();
        default: su_try_help(1);
        }
    }
    if (su_optind < argc) {
        su_error(0, 0, "extra operand %s", su_quote(argv[su_optind]));
        su_try_help(1);
    }
    if (!want) want = S;
    char sys[256], node[256], rel[256], ver[512], mach[64] = "x86_64";
#ifdef SU_HOST
    uname_read("/proc/sys/kernel/ostype", sys, sizeof sys, "Linux");
    const char *os = "GNU/Linux";
#else
    uname_read("/proc/sys/kernel/ostype", sys, sizeof sys, "tobyOS");
    const char *os = "tobyOS";
#endif
    uname_read("/proc/sys/kernel/hostname", node, sizeof node, "");
    uname_read("/proc/sys/kernel/osrelease", rel, sizeof rel, "");
    uname_read("/proc/sys/kernel/version", ver, sizeof ver, "");
    const char *field[8] = { sys, node, rel, ver, mach, mach, mach, os };
    int first = 1;
    for (int k = 0; k < 8; k++) {
        if (!(want & (1 << k))) continue;
        if (!first) putchar(' ');
        fputs(field[k], stdout);
        first = 0;
    }
    putchar('\n');
    return 0;
}
