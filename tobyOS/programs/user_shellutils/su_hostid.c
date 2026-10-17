/* su_hostid.c -- hostid, as GNU coreutils': the host's 32-bit identifier
 * in eight hex digits (logs/utilgate/hostid.sh). As glibc's gethostid:
 * /etc/hostid's four bytes when it has them, else the primary IPv4 address
 * with its 16-bit halves swapped. No operands. It looked at argv[1] alone,
 * so every option was an "extra operand" with no Try line. */
static unsigned long hi_hostid(void) {
#ifdef SU_HOST
    return (unsigned long)gethostid() & 0xFFFFFFFFul;
#else
    unsigned long id = 0;
    FILE *f = fopen("/etc/hostid", "rb");
    if (f) {
        unsigned char b[4];
        if (fread(b, 1, 4, f) == 4)
            id = (unsigned long)b[0] | (unsigned long)b[1] << 8 |
                 (unsigned long)b[2] << 16 | (unsigned long)b[3] << 24;
        fclose(f);
    }
    if (!id) {
        struct abi_system_metrics m;
        memset(&m, 0, sizeof m);
        long r;
        __asm__ volatile("syscall" : "=a"(r)
                         : "0"((long)ABI_SYS_SYSTEM_METRICS), "D"((long)(uintptr_t)&m),
                           "S"((long)sizeof m)
                         : "rcx", "r11", "memory");
        if (r >= 0 && m.net_ip_be) {
            unsigned long a = m.net_ip_be;
            id = ((a << 16) | (a >> 16)) & 0xFFFFFFFFul;
        }
    }
    return id;
#endif
}

static int do_hostid(int argc, char **argv) {
    static const struct su_option longs[] = { SU_LONG_HV, { 0, 0, 0, 0 } };
    int c;
    while ((c = su_getopt_long(argc, argv, "", longs, 0)) != -1) {
        if (c == SU_HELP_OPT) {
            printf("Usage: %s [OPTION]\n"
                   "Print this host's numeric identifier, in hexadecimal.\n"
                   "\n"
                   "      --help     show this help and exit\n"
                   "      --version  show the version and exit\n", su_name);
            return 0;
        }
        if (c == SU_VERSION_OPT) return su_version();
        su_try_help(1);
    }
    if (su_optind < argc) {
        su_error(0, 0, "extra operand %s", su_quote(argv[su_optind]));
        su_try_help(1);
    }
    printf("%08lx\n", hi_hostid());
    return 0;
}
