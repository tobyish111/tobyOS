/* su_hostname.c -- hostname and domainname, as the hostname package's
 * 3.25 (Debian, Ubuntu) (logs/utilgate/hostname.sh). One program: run as
 * domainname, nisdomainname or ypdomainname it is hostname -y, as
 * dnsdomainname hostname -d. NEEDS su_getent.c INCLUDED BEFORE IT: the
 * names it resolves go through the same hosts database getent reads.
 *
 * SHOWING: the name is the kernel's (uname's nodename, read from
 * /proc/sys/kernel/hostname); -s cuts it at the first dot, no lookup.
 * -f is getaddrinfo's canonical name for it and -d the part of that after
 * the first dot -- nothing at all, not even a newline, when there is no
 * dot; -a the aliases gethostbyname gives it, -i the addresses
 * getaddrinfo gives (both space-separated); a name that does not resolve
 * is "Name or service not known", status 1. -I is every address of the
 * machine's interfaces but loopback and link-local, -A the name each of
 * those resolves back to, each followed by a blank. -y is the NIS domain,
 * "(none)" being "Local domain name not set", status 1 -- but domainname
 * with no option shows "(none)" as it is, status 0. The last of
 * -a -A -d -f -i -I -s -y wins.
 *
 * SETTING: a NAME operand, or -F FILE's first line that is neither empty
 * nor begins with '#' (a file with no such line leaves its last line,
 * newline and all -- measured: "\n\n" makes the NIS domain "\n"). -b
 * with -F and nothing read keeps the current host name, or "localhost"
 * when that is empty or "(none)". A host name is then trimmed of blanks
 * (an operand too: `hostname "  foo"` is foo) and must be dot-separated
 * labels of letters, digits and inner hyphens ("the specified hostname
 * is invalid"); a domain name is taken as it is. Not root: "you must be root to change the host name" (or
 * "domain name"), status 1 -- said before a name's length is looked at;
 * over 64 bytes is "name too long". tobyOS has no native sethostname:
 * root's attempt is a write to /proc/sys/kernel/hostname (or domainname),
 * and its failure is reported as the error it is.
 *
 * USAGE: a bad option prints the usage to stdout and getopt's complaint
 * to stderr, status 255; so do -h, --help and -?; a misused operand (two
 * of them, NAME with -F, NAME or -F with a display option) prints it to
 * stderr. -V prints the version, status 0.
 *
 * The old hostname read the name and refused to set one, without
 * options; domainname likewise. */

#define HNM_DEFAULT  0
#define HNM_ALIAS    1
#define HNM_ALLFQDN  2
#define HNM_DNSDOM   3
#define HNM_FQDN     4
#define HNM_IP       5
#define HNM_ALLIP    6
#define HNM_SHORT    7
#define HNM_NIS      8

static void hnm_usage(FILE *to) {
    fprintf(to,
        "Usage: hostname [-b] {NAME|-F FILE}           set the host name (from FILE)\n"
        "       hostname [-a|-A|-d|-f|-i|-I|-s|-y]     show one form of the name\n"
        "       hostname                               show the host name\n"
        "\n"
        "       {yp,nis,}domainname {NAME|-F FILE}     set the NIS domain name\n"
        "       {yp,nis,}domainname                    show the NIS domain name\n"
        "       dnsdomainname                          show the DNS domain\n"
        "\n"
        "       hostname -V|--version|-h|--help        show the version or this help\n"
        "\n"
        "  -a, --alias             the name's aliases\n"
        "  -A, --all-fqdns         the full name of every interface address\n"
        "  -b, --boot              with -F, set a default when FILE gives no name\n"
        "  -d, --domain            the DNS domain\n"
        "  -f, --fqdn, --long      the fully qualified name\n"
        "  -F, --file=FILE         take the name to set from FILE\n"
        "  -i, --ip-address        the addresses the name resolves to\n"
        "  -I, --all-ip-addresses  every address of the machine's interfaces\n"
        "  -s, --short             the name up to its first dot\n"
        "  -y, --yp, --nis         the NIS domain name\n"
        "\n"
        "On tobyOS the name is uname's nodename (/proc/sys/kernel/hostname), and\n"
        "names resolve through /etc/hosts and the DNS resolver.\n");
}

/* the first line of a /proc/sys/kernel file, or 0 */
static char *hnm_proc(const char *leaf) {
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/sys/kernel/%s", leaf);
    if (slurp(path, buf, sizeof buf) < 0) {
#ifdef SU_HOST
        if (!strcmp(leaf, "hostname") && gethostname(buf, sizeof buf) == 0) return nsf_strdup(buf);
#endif
        return 0;
    }
    return nsf_strdup(first_line(buf));
}

/* labels of [A-Za-z0-9-], none empty, none starting or ending with '-' */
static int hnm_valid(const char *s) {
    if (!*s) return 0;
    for (;;) {
        const char *e = s;
        while (isalnum((unsigned char)*e) || *e == '-') e++;
        if (e == s || *s == '-' || e[-1] == '-') return 0;
        if (!*e) return 1;
        if (*e != '.') return 0;
        s = e + 1;
    }
}

/* -F FILE as the hostname package reads it (see the header); 0, or -1
 * with errno */
static int hnm_read_file(const char *path, char **out) {
    int fd = su_open_read(path, 0);
    if (fd < 0) return -1;
    /* the whole file, then fgets' lines over it */
    size_t cap = 4096, n = 0;
    char *all = (char *)nsf_alloc(cap);
    for (;;) {
        if (n + 1024 > cap) {
            cap *= 2;
            all = (char *)realloc(all, cap);
            if (!all) su_error(1, ENOMEM, "memory exhausted");
        }
        ssize_t r = read(fd, all + n, cap - n - 1);
        if (r <= 0) break;                  /* a directory reads as nothing */
        n += (size_t)r;
    }
    close(fd);
    all[n] = 0;
    char *last = nsf_strdup("");
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && all[j] != '\n') j++;
        if (j < n) j++;                     /* keep the newline, as fgets does */
        free(last);
        last = (char *)nsf_alloc(j - i + 1);
        memcpy(last, all + i, j - i);
        last[j - i] = 0;
        i = j;
        if (last[0] == '#' || last[0] == '\n') continue;
        size_t l = strlen(last);
        if (l && last[l - 1] == '\n') last[l - 1] = 0;
        break;
    }
    free(all);
    *out = last;
    return 0;
}

/* set the host (or with NIS the domain) name; the exit status */
static int hnm_set(const char *name, int nis) {
    const char *what = nis ? "domain" : "host";
    size_t len = strlen(name);
    int rc;
#ifdef SU_HOST
    rc = nis ? setdomainname(name, len) : sethostname(name, len);
#else
    /* no native sethostname: root writes the sysctl file, as Linux allows */
    if (geteuid() != 0) { errno = EPERM; rc = -1; }
    else if (len > 64) { errno = EINVAL; rc = -1; }
    else {
        int fd = open(nis ? "/proc/sys/kernel/domainname" : "/proc/sys/kernel/hostname", O_WRONLY);
        rc = -1;
        if (fd >= 0) {
            ssize_t w = write(fd, name, len);
            int e = errno;
            close(fd);
            if (w == (ssize_t)len) rc = 0;
            else errno = w < 0 ? e : EIO;
        }
    }
#endif
    if (rc == 0) return 0;
    int e = errno;
    fflush(stdout);
    if (e == EPERM) fprintf(stderr, "%s: you must be root to change the %s name\n", su_short, what);
    else if (e == EINVAL) fprintf(stderr, "%s: name too long\n", su_short);
    else fprintf(stderr, "%s: %s\n", su_short, strerror(e));
    return 1;
}

/* the machine's interface addresses, loopback and link-local left out */
static int hnm_ifaddrs(struct nsf_addr *out, int max) {
    int n = 0;
#ifdef SU_HOST
    struct ifaddrs *ifa, *p;
    if (getifaddrs(&ifa) != 0) return 0;
    for (p = ifa; p && n < max; p = p->ifa_next) {
        if (!p->ifa_addr || (p->ifa_flags & 0x8 /* IFF_LOOPBACK */)) continue;
        if (p->ifa_addr->sa_family == AF_INET) {
            out[n].af = NSF_AF4;
            memcpy(out[n].a, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, 4);
            n++;
        } else if (p->ifa_addr->sa_family == AF_INET6) {
            const unsigned char *a = (const unsigned char *)&((struct sockaddr_in6 *)p->ifa_addr)->sin6_addr;
            if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) continue;    /* fe80::/10 */
            out[n].af = NSF_AF6;
            memcpy(out[n].a, a, 16);
            n++;
        }
    }
    freeifaddrs(ifa);
#else
    /* the one interface the native ABI reports: its IPv4 address */
    struct abi_system_metrics m;
    memset(&m, 0, sizeof m);
    if (max > 0 && nsf_sc2(ABI_SYS_SYSTEM_METRICS, (long)(uintptr_t)&m, (long)sizeof m) >= 0 &&
        m.net_up && m.net_ip_be) {
        out[0].af = NSF_AF4;
        memcpy(out[0].a, &m.net_ip_be, 4);
        n = 1;
    }
#endif
    return n;
}

static int do_hostname(int argc, char **argv) {
    static const struct su_option longs[] = {
        { "all-fqdns", SU_NO_ARG, 0, 'A' },
        { "alias", SU_NO_ARG, 0, 'a' },
        { "all-ip-addresses", SU_NO_ARG, 0, 'I' },
        { "boot", SU_NO_ARG, 0, 'b' },
        { "domain", SU_NO_ARG, 0, 'd' },
        { "file", SU_REQ_ARG, 0, 'F' },
        { "fqdn", SU_NO_ARG, 0, 'f' },
        { "help", SU_NO_ARG, 0, 'h' },
        { "ip-address", SU_NO_ARG, 0, 'i' },
        { "long", SU_NO_ARG, 0, 'f' },
        { "nis", SU_NO_ARG, 0, 'y' },
        { "short", SU_NO_ARG, 0, 's' },
        { "version", SU_NO_ARG, 0, 'V' },
        { "yp", SU_NO_ARG, 0, 'y' },
        { 0, 0, 0, 0 }
    };
    int type = HNM_DEFAULT, boot = 0, c;
    const char *file = 0;
    /* domainname alone shows the name as the kernel has it, "(none)"
     * included, status 0; -y, nisdomainname and ypdomainname call "(none)"
     * unset (measured, hostname 3.25 in a WSL UTS namespace) */
    int raw_dom = !strcmp(su_short, "domainname");
    if (!strcmp(su_short, "domainname") || !strcmp(su_short, "nisdomainname") ||
        !strcmp(su_short, "ypdomainname"))
        type = HNM_NIS;
    else if (!strcmp(su_short, "dnsdomainname")) type = HNM_DNSDOM;
    for (;;) {
        su_optopt = -1;
        c = su_getopt_long(argc, argv, "aAbdfF:hiIsVy?", longs, 0);
        if (c == -1) break;
        switch (c) {
        case 'a': type = HNM_ALIAS; break;
        case 'A': type = HNM_ALLFQDN; break;
        case 'b': boot = 1; break;
        case 'd': type = HNM_DNSDOM; break;
        case 'f': type = HNM_FQDN; break;
        case 'F': file = su_optarg; break;
        case 'i': type = HNM_IP; break;
        case 'I': type = HNM_ALLIP; break;
        case 's': type = HNM_SHORT; break;
        case 'y': type = HNM_NIS; raw_dom = 0; break;
        case 'V': return su_version();
        case 'h': hnm_usage(stdout); return 255;
        default:                 /* '?': -? itself, or a bad option */
            hnm_usage(stdout);
            return 255;
        }
    }
    int nops = argc - su_optind;
    char *name = 0;
    if (file && hnm_read_file(file, &name) != 0) {
        if (!boot) {
            int e = errno;
            fflush(stdout);
            fprintf(stderr, "%s: %s\n", su_short, strerror(e));
            return 1;
        }
        name = nsf_strdup("");
    }
    if (nops > 1 || (file && nops == 1) ||
        ((file || nops == 1) && type != HNM_DEFAULT && type != HNM_NIS)) {
        hnm_usage(stderr);
        return 255;
    }
    if (file || nops == 1) {
        if (!file) name = nsf_strdup(argv[su_optind]);
        /* -b's default is for a file that gave nothing at all: one that
         * gave blanks is trimmed to nothing below, and refused */
        if (file && boot && !*name) {
            char *cur = hnm_proc("hostname");
            free(name);
            name = nsf_strdup(cur && *cur && strcmp(cur, "(none)") ? cur : "localhost");
            free(cur);
        }
        if (type == HNM_DEFAULT) {
            /* a host name is trimmed of blanks, from FILE or not */
            char *s = name;
            while (nsf_space((unsigned char)*s)) s++;
            size_t l = strlen(s);
            while (l && nsf_space((unsigned char)s[l - 1])) l--;
            memmove(name, s, l);
            name[l] = 0;
        }
        if (type == HNM_DEFAULT && !hnm_valid(name)) {
            fflush(stdout);
            fprintf(stderr, "%s: the specified hostname is invalid\n", su_short);
            return 1;
        }
        int rc = hnm_set(name, type == HNM_NIS);
        free(name);
        return rc;
    }

    if (type == HNM_NIS) {
        char *d = hnm_proc("domainname");
        if (!d) { fprintf(stderr, "%s: %s\n", su_short, strerror(errno)); return 1; }
        if (!raw_dom && !strcmp(d, "(none)")) {
            fflush(stdout);
            fprintf(stderr, "%s: Local domain name not set\n", su_short);
            return 1;
        }
        puts(d);
        free(d);
        return 0;
    }
    char *host = hnm_proc("hostname");
    if (!host) { fprintf(stderr, "%s: %s\n", su_short, strerror(errno)); return 1; }
    int rc = 0;
    switch (type) {
    case HNM_DEFAULT: puts(host); break;
    case HNM_SHORT: {
        char *dot = strchr(host, '.');
        if (dot) *dot = 0;
        puts(host);
        break;
    }
    case HNM_FQDN: case HNM_DNSDOM: case HNM_ALIAS: case HNM_IP: {
        struct nsf_ai r;
        if (!nsf_getaddrinfo(host, 0, &r)) {
            fflush(stdout);
            fprintf(stderr, "%s: Name or service not known\n", su_short);
            rc = 1;
            break;
        }
        if (type == HNM_FQDN) puts(r.canon);
        else if (type == HNM_DNSDOM) {
            const char *dot = strchr(r.canon, '.');
            if (dot) puts(dot + 1);
        } else if (type == HNM_IP) {
            for (int i = 0; i < r.n; i++) {
                char b[64];
                nsf_ntop(&r.ad[i], b);
                printf("%s%s", i ? " " : "", b);
            }
            putchar('\n');
        } else {
            struct nsf_hent h;
            if (nsf_gethostbyname(host, NSF_AF4, &h)) {
                for (int i = 0; i < h.nal; i++) printf("%s%s", i ? " " : "", h.al[i]);
                nsf_hent_free(&h);
            }
            putchar('\n');
        }
        nsf_ai_free(&r);
        break;
    }
    case HNM_ALLIP: case HNM_ALLFQDN: {
        struct nsf_addr ifs[64];
        int n = hnm_ifaddrs(ifs, 64);
        for (int i = 0; i < n; i++) {
            if (type == HNM_ALLIP) {
                char b[64];
                nsf_ntop(&ifs[i], b);
                printf("%s ", b);
            } else {
                struct nsf_hent h;
                if (nsf_gethostbyaddr(&ifs[i], &h)) {
                    printf("%s ", h.name);
                    nsf_hent_free(&h);
                }
            }
        }
        putchar('\n');
        break;
    }
    }
    free(host);
    if (fflush(stdout) != 0 || ferror(stdout)) return 1;
    return rc;
}

/* domainname: the same program, which knows by its name to be -y */
static int do_domainname(int argc, char **argv) {
    return do_hostname(argc, argv);
}
