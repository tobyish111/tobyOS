/* su_getent.c -- getent, as glibc 2.43's, and groups, as GNU coreutils
 * 9.7's (logs/utilgate/getent.sh, groups.sh); and the name-service code
 * they and hostname (su_hostname.c, which must be included after this
 * file) share.
 *
 * THERE IS NO NSS ON tobyOS. What a Linux system answers through
 * nsswitch.conf's modules is, here, the files in /etc and -- for hosts --
 * the kernel's DNS resolver. So this file is a "files" backend for every
 * database getent knows, written to glibc's measured behaviour, plus a
 * "dns" one for hosts: on the guest ABI_SYS_DNS_RESOLVE (one IPv4
 * address, no reverse lookups, no AAAA), on the Linux build glibc's own
 * resolver. Which services a database uses comes, as on Linux, from
 * getent -s, else /etc/nsswitch.conf, else "files" ("files dns" for
 * hosts); a service this file does not have (systemd, nis, db ...)
 * answers "unavailable", so `netgroup: nis` on Ubuntu finds nothing here
 * just as it does there.
 *
 * THE FILES, as glibc reads them (all measured on 2.43 with private
 * copies of /etc in a mount namespace):
 *   passwd  name:passwd:uid:gid[:gecos[:dir[:shell]]] -- uid and gid all
 *           digits and 32-bit, or the line is skipped; the shell is the
 *           rest of the line, so a line with too many fields is "error
 *           writing passwd entry: Invalid argument" when printed; +/-
 *           (NIS compat) lines are listed but never found
 *   group   name:passwd:gid[:members], members comma-separated with
 *           leading blanks dropped; the scan that answers initgroups
 *           (and groups USER) does NOT skip '#' lines, the others do
 *   shadow  exactly 8 or 9 fields, the numbers digits or empty
 *   gshadow anything, missing fields empty
 *   hosts   address names...; "#" starts a comment anywhere; an IPv4
 *           query sees ::1 as 127.0.0.1 and ::ffff:a.b.c.d as a.b.c.d, an
 *           IPv6 query sees no IPv4 line; names match in any case; with
 *           "multi on" in /etc/host.conf every matching line is merged,
 *           else the first wins
 *   services/protocols/rpc  name number[/proto] aliases..., names in
 *           their case; networks: names in any case, short numbers
 *           left-justified (10 is 10.0.0.0); ethers: no enumeration
 *
 * OUTPUT, exit status 0 (found / listed), 1 (bad usage: no database,
 * unknown database -- the "Try" line on stdout, the complaint on
 * stderr), 2 (a key not found), 3 (that database cannot be listed),
 * 64 (a bad option, as argp says it).
 *
 * The old getent knew passwd group hosts ahosts services, answered
 * localhost from nowhere, and called every other database unknown. */

#ifdef SU_HOST
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#endif

#define NSF_SUCCESS  0
#define NSF_NOTFOUND 1
#define NSF_UNAVAIL  2
#define NSF_TRYAGAIN 3

/* ---- small helpers ------------------------------------------------------ */

static void *nsf_alloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) su_error(1, ENOMEM, "memory exhausted");
    return p;
}

static char *nsf_strdup(const char *s) {
    size_t l = strlen(s);
    char *r = (char *)nsf_alloc(l + 1);
    memcpy(r, s, l + 1);
    return r;
}

static int nsf_blank(int c) { return c == ' ' || c == '\t'; }
static int nsf_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

static int nsf_casecmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y) return x - y;
        if (!x) return 0;
    }
}

/* one line of F, without its newline, in a buffer reused between calls;
 * 0 at the end */
static char *nsf_getline(FILE *f) {
    static char *buf;
    static size_t cap;
    size_t n = 0;
    int c, any = 0;
    while ((c = getc(f)) != EOF) {
        any = 1;
        if (n + 2 > cap) {
            cap = cap ? cap * 2 : 256;
            buf = (char *)realloc(buf, cap);
            if (!buf) su_error(1, ENOMEM, "memory exhausted");
        }
        if (c == '\n') break;
        buf[n++] = (char)c;
    }
    if (!any) return 0;
    if (!buf) { cap = 256; buf = (char *)nsf_alloc(cap); }
    buf[n] = 0;
    return buf;
}

/* strtoul's reading, base 10, on any libc: blanks, a sign, digits;
 * *END after them (at S when there are none) */
static unsigned long nsf_strtoul(const char *s, const char **end) {
    const char *p = s;
    while (nsf_space((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') { neg = *p == '-'; p++; }
    if (!isdigit((unsigned char)*p)) { *end = s; return 0; }
    unsigned long v = 0;
    int over = 0;
    for (; isdigit((unsigned char)*p); p++) {
        unsigned d = (unsigned)(*p - '0');
        if (v > (ULONG_MAX - d) / 10) over = 1;
        else v = v * 10 + d;
    }
    *end = p;
    if (over) return ULONG_MAX;
    return neg ? 0UL - v : v;
}

/* all digits, not empty, at most 2^32-1: a uid/gid field */
static int nsf_id(const char *s, uint32_t *out) {
    if (!*s) return 0;
    uint64_t v = 0;
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) return 0;
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > 0xffffffffull) return 0;
    }
    *out = (uint32_t)v;
    return 1;
}

/* atoi's reading: leading digits */
static long nsf_atoi(const char *s) {
    long v = 0;
    for (; isdigit((unsigned char)*s); s++) {
        v = v * 10 + (*s - '0');
        if (v > 0x7fffffffL) v = 0x7fffffffL;
    }
    return v;
}

/* split S in place at blanks; up to MAX words */
static int nsf_words(char *s, char **w, int max) {
    int n = 0;
    for (;;) {
        while (nsf_blank((unsigned char)*s)) s++;
        if (!*s) break;
        if (n == max) break;
        w[n++] = s;
        while (*s && !nsf_blank((unsigned char)*s)) s++;
        if (*s) *s++ = 0;
    }
    return n;
}

/* cut a '#' comment off */
static void nsf_uncomment(char *s) {
    char *h = strchr(s, '#');
    if (h) *h = 0;
}

/* ---- addresses -----------------------------------------------------------
 *
 * libtoby has no <arpa/inet.h>, so the text forms are read and written
 * here, as glibc's inet_pton/inet_aton/inet_network/inet_ntop do. */

#define NSF_AF4 4
#define NSF_AF6 6

struct nsf_addr { int af; unsigned char a[16]; };

/* inet_pton(AF_INET): exactly four decimal parts, no leading zeros */
static int nsf_pton4(const char *s, unsigned char out[4]) {
    int parts = 0;
    while (parts < 4) {
        if (!isdigit((unsigned char)*s)) return 0;
        unsigned v = 0;
        const char *st = s;
        while (isdigit((unsigned char)*s)) {
            v = v * 10 + (unsigned)(*s - '0');
            if (v > 255) return 0;
            s++;
        }
        if (s - st > 1 && *st == '0') return 0;
        out[parts++] = (unsigned char)v;
        if (parts < 4) { if (*s != '.') return 0; s++; }
    }
    return *s == 0;
}

/* one part of inet_aton / inet_network: 0x hex, 0 octal, else decimal */
static int nsf_numpart(const char **ps, uint64_t *v) {
    const char *s = *ps;
    uint64_t x = 0;
    int base = 10, any = 0;
    if (*s == '0') {
        if (s[1] == 'x' || s[1] == 'X') { base = 16; s += 2; }
        else { base = 8; any = 1; s++; }
    }
    for (;; s++) {
        int d;
        if (isdigit((unsigned char)*s)) d = *s - '0';
        else if (base == 16 && isxdigit((unsigned char)*s)) d = tolower((unsigned char)*s) - 'a' + 10;
        else break;
        if (d >= base) return 0;
        x = x * (uint64_t)base + (uint64_t)d;
        if (x > 0xffffffffull) return 0;
        any = 1;
    }
    if (!any) return 0;
    *ps = s;
    *v = x;
    return 1;
}

/* inet_aton, the whole string: a, a.b (8.24), a.b.c (8.8.16), a.b.c.d */
static int nsf_aton(const char *s, uint32_t *out) {
    uint64_t p[4];
    int n = 0;
    for (;;) {
        if (n == 4) return 0;
        if (!nsf_numpart(&s, &p[n])) return 0;
        n++;
        if (*s == '.') { s++; continue; }
        if (*s) return 0;
        break;
    }
    uint64_t v;
    switch (n) {
    case 1: v = p[0]; break;
    case 2: if (p[0] > 255 || p[1] > 0xffffff) return 0; v = p[0] << 24 | p[1]; break;
    case 3: if (p[0] > 255 || p[1] > 255 || p[2] > 0xffff) return 0;
            v = p[0] << 24 | p[1] << 16 | p[2]; break;
    default:
        for (int i = 0; i < 4; i++) if (p[i] > 255) return 0;
        v = p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
    }
    *out = (uint32_t)v;
    return 1;
}

/* inet_network: up to four parts of at most 255, concatenated */
static int nsf_inet_network(const char *s, uint32_t *out, int *nparts) {
    uint64_t v = 0, x;
    int n = 0;
    for (;;) {
        if (n == 4) return 0;
        if (!nsf_numpart(&s, &x) || x > 255) return 0;
        v = v << 8 | x;
        n++;
        if (*s == '.') { s++; continue; }
        if (*s) return 0;
        break;
    }
    *out = (uint32_t)v;
    *nparts = n;
    return 1;
}

/* inet_pton(AF_INET6) */
static int nsf_pton6(const char *s, unsigned char out[16]) {
    unsigned short w[8];
    int n = 0, gap = -1;
    if (s[0] == ':') {
        if (s[1] != ':') return 0;
        gap = 0;
        s += 2;
        if (!*s) goto done;
    }
    for (;;) {
        /* an embedded IPv4 address ends it */
        const char *dot = 0;
        for (const char *q = s; *q && *q != ':'; q++) if (*q == '.') { dot = q; break; }
        if (dot) {
            unsigned char v4[4];
            if (n > 6 || !nsf_pton4(s, v4)) return 0;
            w[n++] = (unsigned short)(v4[0] << 8 | v4[1]);
            w[n++] = (unsigned short)(v4[2] << 8 | v4[3]);
            break;
        }
        unsigned v = 0;
        int d = 0;
        while (isxdigit((unsigned char)*s)) {
            if (++d > 4) return 0;
            v = v * 16 + (unsigned)(isdigit((unsigned char)*s) ? *s - '0' : tolower((unsigned char)*s) - 'a' + 10);
            s++;
        }
        if (!d || n == 8) return 0;
        w[n++] = (unsigned short)v;
        if (!*s) break;
        if (*s != ':') return 0;
        s++;
        if (*s == ':') {
            if (gap >= 0) return 0;
            gap = n;
            s++;
            if (!*s) break;
        } else if (!*s) return 0;
    }
done:
    if (gap < 0 && n != 8) return 0;
    if (gap >= 0 && n > 7) return 0;
    {
        unsigned short full[8] = { 0 };
        int tail = gap >= 0 ? n - gap : 0;
        for (int i = 0; i < (gap >= 0 ? gap : n); i++) full[i] = w[i];
        for (int i = 0; i < tail; i++) full[8 - tail + i] = w[gap + i];
        for (int i = 0; i < 8; i++) { out[2 * i] = (unsigned char)(full[i] >> 8); out[2 * i + 1] = (unsigned char)full[i]; }
    }
    return 1;
}

static void nsf_ntop4(const unsigned char *a, char *buf) {
    sprintf(buf, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
}

/* inet_ntop(AF_INET6): the longest run of two or more zero groups as
 * "::", hex in lower case; ::ffff:a.b.c.d and ::a.b.c.d (but not ::1) in
 * the dotted form, as glibc writes them */
static void nsf_ntop6(const unsigned char *a, char *buf) {
    unsigned w[8];
    for (int i = 0; i < 8; i++) w[i] = (unsigned)(a[2 * i] << 8 | a[2 * i + 1]);
    int best = -1, blen = 0;
    for (int i = 0; i < 8;) {
        if (w[i]) { i++; continue; }
        int j = i;
        while (j < 8 && !w[j]) j++;
        if (j - i > blen) { best = i; blen = j - i; }
        i = j;
    }
    if (blen < 2) best = -1;
    char *p = buf;
    for (int i = 0; i < 8; i++) {
        if (best >= 0 && i >= best && i < best + blen) {
            if (i == best) *p++ = ':';
            continue;
        }
        if (i) *p++ = ':';
        if (i == 6 && best == 0 && (blen == 6 || (blen == 5 && w[5] == 0xffff))) {
            nsf_ntop4(a + 12, p);
            p += strlen(p);
            break;
        }
        p += sprintf(p, "%x", w[i]);
    }
    if (best >= 0 && best + blen == 8) *p++ = ':';
    *p = 0;
}

static void nsf_ntop(const struct nsf_addr *ad, char *buf) {
    if (ad->af == NSF_AF4) nsf_ntop4(ad->a, buf);
    else nsf_ntop6(ad->a, buf);
}

static int nsf_is_mapped(const unsigned char *a) {
    static const unsigned char pre[12] = { 0,0,0,0,0,0,0,0,0,0,0xff,0xff };
    return memcmp(a, pre, 12) == 0;
}

static int nsf_is_loop6(const unsigned char *a) {
    for (int i = 0; i < 15; i++) if (a[i]) return 0;
    return a[15] == 1;
}

/* ---- which services a database uses -------------------------------------- */

static const char *const nsf_dbs[] = {
    "ahosts", "ahostsv4", "ahostsv6", "aliases", "ethers", "group", "gshadow",
    "hosts", "initgroups", "netgroup", "networks", "passwd", "protocols",
    "rpc", "services", "shadow", 0
};
#define NSF_NDB 16

static char *nsf_override[NSF_NDB];     /* -s DB:SERVICES */
static char *nsf_override_all;          /* -s SERVICES */

static int nsf_dbindex(const char *name) {
    for (int i = 0; nsf_dbs[i]; i++) if (!strcmp(nsf_dbs[i], name)) return i;
    return -1;
}

/* the ahosts databases are hosts' */
static const char *nsf_dbbase(const char *db) {
    return !strncmp(db, "ahosts", 6) ? "hosts" : db;
}

/* DB's line in /etc/nsswitch.conf, malloc'd, or 0 */
static char *nsf_conf_line(const char *db) {
    FILE *f = fopen("/etc/nsswitch.conf", "r");
    if (!f) return 0;
    char *line, *r = 0;
    size_t dl = strlen(db);
    while ((line = nsf_getline(f))) {
        nsf_uncomment(line);
        char *p = line;
        while (nsf_blank((unsigned char)*p)) p++;
        if (strncmp(p, db, dl) || p[dl] != ':') continue;
        r = nsf_strdup(p + dl + 1);
        break;
    }
    fclose(f);
    return r;
}

/* the service list for DB */
static const char *nsf_config(const char *db) {
    db = nsf_dbbase(db);
    int i = nsf_dbindex(db);
    if (i >= 0 && nsf_override[i]) return nsf_override[i];
    if (nsf_override_all) return nsf_override_all;
    static char *cache[NSF_NDB];
    static int done[NSF_NDB];
    char *l;
    if (i >= 0 && done[i]) l = cache[i];
    else {
        l = nsf_conf_line(db);
        if (i >= 0) { done[i] = 1; cache[i] = l; }
    }
    if (l) return l;
    /* initgroups without a line of its own is group's */
    if (!strcmp(db, "initgroups")) return nsf_config("group");
    return !strcmp(db, "hosts") ? "files dns" : "files";
}

struct nsf_svc { char name[32]; unsigned char ret[4]; };

/* "files [NOTFOUND=return] dns" into services and their actions */
static int nsf_services(const char *spec, struct nsf_svc *sv, int max) {
    int n = 0;
    const char *p = spec;
    for (;;) {
        while (nsf_space((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '[') {
            p++;
            for (;;) {
                while (nsf_space((unsigned char)*p)) p++;
                if (!*p) return n;
                if (*p == ']') { p++; break; }
                int neg = 0;
                if (*p == '!') { neg = 1; p++; }
                char st[16], act[16];
                size_t k = 0;
                while (*p && *p != '=' && *p != ']' && !nsf_space((unsigned char)*p)) {
                    if (k < sizeof st - 1) st[k++] = (char)tolower((unsigned char)*p);
                    p++;
                }
                st[k] = 0;
                k = 0;
                if (*p == '=') p++;
                while (*p && *p != ']' && !nsf_space((unsigned char)*p)) {
                    if (k < sizeof act - 1) act[k++] = (char)tolower((unsigned char)*p);
                    p++;
                }
                act[k] = 0;
                if (!n) continue;
                int code = !strcmp(st, "success") ? NSF_SUCCESS : !strcmp(st, "notfound") ? NSF_NOTFOUND
                         : !strcmp(st, "unavail") ? NSF_UNAVAIL : !strcmp(st, "tryagain") ? NSF_TRYAGAIN : -1;
                if (code < 0) continue;
                unsigned char r = !strcmp(act, "return");
                for (int s = 0; s < 4; s++)
                    if (neg ? s != code : s == code) sv[n - 1].ret[s] = r;
            }
            continue;
        }
        size_t k = 0;
        char nm[32];
        while (*p && !nsf_space((unsigned char)*p) && *p != '[') {
            if (k < sizeof nm - 1) nm[k++] = *p;
            p++;
        }
        nm[k] = 0;
        if (n < max) {
            snprintf(sv[n].name, sizeof sv[n].name, "%s", nm);
            sv[n].ret[NSF_SUCCESS] = 1;
            sv[n].ret[NSF_NOTFOUND] = sv[n].ret[NSF_UNAVAIL] = sv[n].ret[NSF_TRYAGAIN] = 0;
            n++;
        }
    }
    return n;
}

/* Run FN for each of DB's services in turn, as NSS does: until one's
 * answer says return. FN answers NSF_UNAVAIL for a service it lacks. */
typedef int (*nsf_fn)(const char *svc, void *ctx);
static int nsf_run(const char *db, nsf_fn fn, void *ctx) {
    struct nsf_svc sv[16];
    int n = nsf_services(nsf_config(db), sv, 16);
    int st = NSF_UNAVAIL;
    for (int i = 0; i < n; i++) {
        st = fn(sv[i].name, ctx);
        if (sv[i].ret[st]) break;
    }
    return st;
}

/* does DB's list name SVC (for listing: every "files" once) */
static int nsf_has_service(const char *db, const char *svc) {
    struct nsf_svc sv[16];
    int n = nsf_services(nsf_config(db), sv, 16);
    for (int i = 0; i < n; i++) if (!strcmp(sv[i].name, svc)) return 1;
    return 0;
}

/* ---- passwd --------------------------------------------------------------- */

struct nsf_pw {
    char *name, *passwd, *uid_s, *gid_s, *gecos, *dir, *shell;
    uint32_t uid, gid;
    int compat;
};

/* the next field of *P up to ':' (or the end); 0 when there is none */
static char *nsf_field(char **p) {
    if (!*p) return 0;
    char *s = *p, *c = strchr(s, ':');
    if (c) { *c = 0; *p = c + 1; }
    else *p = 0;
    return s;
}

static int nsf_parse_pw(char *line, struct nsf_pw *pw) {
    while (nsf_blank((unsigned char)*line)) line++;
    if (!*line || *line == '#') return 0;
    char *p = line;
    memset(pw, 0, sizeof *pw);
    pw->name = nsf_field(&p);
    pw->compat = pw->name[0] == '+' || pw->name[0] == '-';
    pw->passwd = nsf_field(&p);
    pw->uid_s = nsf_field(&p);
    pw->gid_s = nsf_field(&p);
    pw->gecos = nsf_field(&p);
    pw->dir = nsf_field(&p);
    pw->shell = p;                   /* the rest, colons and all */
    if (!pw->passwd) pw->passwd = (char *)"";
    if (!pw->gecos) pw->gecos = (char *)"";
    if (!pw->dir) pw->dir = (char *)"";
    if (!pw->shell) pw->shell = (char *)"";
    if (pw->compat) {
        if (!pw->uid_s) pw->uid_s = (char *)"";
        if (!pw->gid_s) pw->gid_s = (char *)"";
        return 1;
    }
    if (!pw->uid_s || !pw->gid_s) return 0;
    if (!nsf_id(pw->uid_s, &pw->uid) || !nsf_id(pw->gid_s, &pw->gid)) return 0;
    return 1;
}

static void nsf_put_pw(const struct nsf_pw *pw) {
    if (strchr(pw->gecos, ':') || strchr(pw->dir, ':') || strchr(pw->shell, ':')) {
        fflush(stdout);
        fprintf(stderr, "error writing passwd entry: %s\n", strerror(EINVAL));
        return;
    }
    if (pw->compat)
        printf("%s:%s:%s:%s:%s:%s:%s\n", pw->name, pw->passwd, pw->uid_s, pw->gid_s,
               pw->gecos, pw->dir, pw->shell);
    else
        printf("%s:%s:%lu:%lu:%s:%s:%s\n", pw->name, pw->passwd, (unsigned long)pw->uid,
               (unsigned long)pw->gid, pw->gecos, pw->dir, pw->shell);
}

/* a found entry is copied out, the strings malloc'd together */
struct nsf_pwkey { const char *name; int by_id; uint32_t id; struct nsf_pw out; char *mem; };

static int nsf_pw_files(const char *svc, void *ctx) {
    struct nsf_pwkey *k = (struct nsf_pwkey *)ctx;
    if (strcmp(svc, "files")) return NSF_UNAVAIL;
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return NSF_UNAVAIL;
    char *line;
    int st = NSF_NOTFOUND;
    while ((line = nsf_getline(f))) {
        char *copy = nsf_strdup(line);
        struct nsf_pw pw;
        if (nsf_parse_pw(copy, &pw) && !pw.compat &&
            (k->by_id ? pw.uid == k->id : !strcmp(pw.name, k->name))) {
            k->out = pw;
            k->mem = copy;
            st = NSF_SUCCESS;
            break;
        }
        free(copy);
    }
    fclose(f);
    return st;
}

/* getpwnam / getpwuid through the passwd services; 1 when found */
static int nsf_getpw(const char *name, int by_id, uint32_t id, struct nsf_pwkey *k) {
    memset(k, 0, sizeof *k);
    k->name = name;
    k->by_id = by_id;
    k->id = id;
    return nsf_run("passwd", nsf_pw_files, k) == NSF_SUCCESS;
}

/* ---- group ---------------------------------------------------------------- */

struct nsf_gr {
    char *name, *passwd;
    uint32_t gid;
    char **mem;
    int nmem;
};

/* INITG: the scan initgroups makes, which reads '#' lines as groups */
static int nsf_parse_gr(char *line, struct nsf_gr *gr, int initg) {
    while (nsf_blank((unsigned char)*line)) line++;
    if (!*line || (*line == '#' && !initg)) return 0;
    char *p = line;
    gr->name = nsf_field(&p);
    gr->passwd = nsf_field(&p);
    char *gid = nsf_field(&p);
    if (!gr->passwd || !gid || !nsf_id(gid, &gr->gid)) return 0;
    gr->nmem = 0;
    gr->mem = 0;
    if (p) {
        size_t cap = 1;
        for (char *q = p; *q; q++) if (*q == ',') cap++;
        gr->mem = (char **)nsf_alloc(cap * sizeof *gr->mem);
        char *s = p;
        for (;;) {
            char *c = strchr(s, ',');
            if (c) *c = 0;
            while (nsf_blank((unsigned char)*s)) s++;
            if (*s) gr->mem[gr->nmem++] = s;
            if (!c) break;
            s = c + 1;
        }
    }
    return 1;
}

static void nsf_put_gr(const struct nsf_gr *gr) {
    for (int i = 0; i < gr->nmem; i++)
        if (strchr(gr->mem[i], ':')) {
            fflush(stdout);
            fprintf(stderr, "error writing group entry: %s\n", strerror(EINVAL));
            return;
        }
    printf("%s:%s:%lu:", gr->name, gr->passwd, (unsigned long)gr->gid);
    for (int i = 0; i < gr->nmem; i++) printf("%s%s", i ? "," : "", gr->mem[i]);
    putchar('\n');
}

struct nsf_grkey { const char *name; int by_id; uint32_t id; char *found; };

static int nsf_gr_files(const char *svc, void *ctx) {
    struct nsf_grkey *k = (struct nsf_grkey *)ctx;
    if (strcmp(svc, "files")) return NSF_UNAVAIL;
    FILE *f = fopen("/etc/group", "r");
    if (!f) return NSF_UNAVAIL;
    char *line;
    int st = NSF_NOTFOUND;
    while ((line = nsf_getline(f))) {
        char *copy = nsf_strdup(line);
        struct nsf_gr gr;
        int ok = nsf_parse_gr(copy, &gr, 0);
        if (ok) free(gr.mem);
        if (ok && (k->by_id ? gr.gid == k->id : !strcmp(gr.name, k->name))) {
            free(copy);
            k->found = nsf_strdup(line);
            st = NSF_SUCCESS;
            break;
        }
        free(copy);
    }
    fclose(f);
    return st;
}

/* getgrnam / getgrgid: the entry's line (malloc'd), or 0 */
static char *nsf_getgr(const char *name, int by_id, uint32_t id) {
    struct nsf_grkey k = { name, by_id, id, 0 };
    if (nsf_run("group", nsf_gr_files, &k) != NSF_SUCCESS) return 0;
    return k.found;
}

/* the name of group GID into OUT; 0 when it has none */
static int nsf_group_name(uint32_t gid, char *out, size_t cap) {
    char *l = nsf_getgr(0, 1, gid);
    if (!l) return 0;
    struct nsf_gr gr;
    char *p = l;
    while (nsf_blank((unsigned char)*p)) p++;
    nsf_parse_gr(p, &gr, 0);
    snprintf(out, cap, "%s", gr.name);
    free(gr.mem);
    free(l);
    return 1;
}

/* the groups USER is a member of, in the file's order, duplicates and
 * all; SKIP (when not -1) left out -- getgrouplist's list without its
 * first element */
struct nsf_initg { const char *user; int64_t skip; uint32_t *g; int n, cap; };

static int nsf_initg_files(const char *svc, void *ctx) {
    struct nsf_initg *k = (struct nsf_initg *)ctx;
    if (strcmp(svc, "files")) return NSF_UNAVAIL;
    FILE *f = fopen("/etc/group", "r");
    if (!f) return NSF_UNAVAIL;
    char *line;
    while ((line = nsf_getline(f))) {
        struct nsf_gr gr;
        if (!nsf_parse_gr(line, &gr, 1)) continue;
        for (int i = 0; i < gr.nmem; i++) {
            if (strcmp(gr.mem[i], k->user)) continue;
            if (k->skip < 0 || gr.gid != (uint32_t)k->skip) {
                if (k->n == k->cap) {
                    k->cap = k->cap ? k->cap * 2 : 16;
                    k->g = (uint32_t *)realloc(k->g, (size_t)k->cap * sizeof *k->g);
                    if (!k->g) su_error(1, ENOMEM, "memory exhausted");
                }
                k->g[k->n++] = gr.gid;
            }
            break;
        }
        free(gr.mem);
    }
    fclose(f);
    return NSF_SUCCESS;
}

static void nsf_initgroups(const char *user, int64_t skip, struct nsf_initg *k) {
    memset(k, 0, sizeof *k);
    k->user = user;
    k->skip = skip;
    nsf_run("initgroups", nsf_initg_files, k);
}

/* ---- hosts ----------------------------------------------------------------- */

struct nsf_hent {
    char *name;
    char **al;
    int nal, alcap;
    struct nsf_addr *ad;
    int nad, adcap;
};

static void nsf_hent_free(struct nsf_hent *h) {
    free(h->name);
    for (int i = 0; i < h->nal; i++) free(h->al[i]);
    free(h->al);
    free(h->ad);
    memset(h, 0, sizeof *h);
}

static void nsf_hent_alias(struct nsf_hent *h, const char *s) {
    if (h->nal == h->alcap) {
        h->alcap = h->alcap ? h->alcap * 2 : 8;
        h->al = (char **)realloc(h->al, (size_t)h->alcap * sizeof *h->al);
        if (!h->al) su_error(1, ENOMEM, "memory exhausted");
    }
    h->al[h->nal++] = nsf_strdup(s);
}

static void nsf_hent_addr(struct nsf_hent *h, const struct nsf_addr *a) {
    if (h->nad == h->adcap) {
        h->adcap = h->adcap ? h->adcap * 2 : 8;
        h->ad = (struct nsf_addr *)realloc(h->ad, (size_t)h->adcap * sizeof *h->ad);
        if (!h->ad) su_error(1, ENOMEM, "memory exhausted");
    }
    h->ad[h->nad++] = *a;
}

/* one line of /etc/hosts: its address as FAMILY sees it (0: as written),
 * names in W[1..N-1]; 0 when the line has none for FAMILY */
static int nsf_hosts_line(char *line, int family, struct nsf_addr *ad, char **w, int *nw) {
    nsf_uncomment(line);
    int n = nsf_words(line, w, 256);
    if (n < 1) return 0;
    unsigned char a6[16];
    if (nsf_pton4(w[0], ad->a)) ad->af = NSF_AF4;
    else if (nsf_pton6(w[0], a6)) {
        ad->af = NSF_AF6;
        memcpy(ad->a, a6, 16);
    } else return 0;
    if (family == NSF_AF4 && ad->af == NSF_AF6) {
        if (nsf_is_mapped(ad->a)) memmove(ad->a, ad->a + 12, 4);
        else if (nsf_is_loop6(ad->a)) { ad->a[0] = 127; ad->a[1] = 0; ad->a[2] = 0; ad->a[3] = 1; }
        else return 0;
        ad->af = NSF_AF4;
    } else if (family == NSF_AF6 && ad->af == NSF_AF4) return 0;
    *nw = n;
    return 1;
}

static int nsf_hosts_multi(void) {
    static int v = -1;
    if (v >= 0) return v;
    v = 0;
    FILE *f = fopen("/etc/host.conf", "r");
    if (!f) return v;
    char *line;
    while ((line = nsf_getline(f))) {
        nsf_uncomment(line);
        char *w[4];
        int n = nsf_words(line, w, 4);
        if (n >= 2 && !nsf_casecmp(w[0], "multi")) v = !nsf_casecmp(w[1], "on");
    }
    fclose(f);
    return v;
}

static int nsf_addr_eq(const struct nsf_addr *a, const struct nsf_addr *b) {
    return a->af == b->af && !memcmp(a->a, b->a, a->af == NSF_AF4 ? 4 : 16);
}

/* /etc/hosts by name, as FAMILY (NSF_AF4, NSF_AF6, or 0 for any) sees it */
static int nsf_hosts_byname_files(const char *name, int family, struct nsf_hent *h) {
    FILE *f = fopen("/etc/hosts", "r");
    if (!f) return NSF_UNAVAIL;
    int multi = nsf_hosts_multi(), found = 0;
    char *line, *w[256];
    while ((line = nsf_getline(f))) {
        struct nsf_addr ad;
        int n;
        if (!nsf_hosts_line(line, family, &ad, w, &n)) continue;
        int m = 0;
        for (int i = 1; i < n && !m; i++) m = !nsf_casecmp(w[i], name);
        if (!m) continue;
        if (!found) {
            h->name = nsf_strdup(n > 1 ? w[1] : "");
            for (int i = 2; i < n; i++) nsf_hent_alias(h, w[i]);
            found = 1;
        } else {
            if (strcmp(w[1], h->name)) nsf_hent_alias(h, w[1]);
            for (int i = 2; i < n; i++) nsf_hent_alias(h, w[i]);
        }
        nsf_hent_addr(h, &ad);
        if (!multi) break;
    }
    fclose(f);
    return found ? NSF_SUCCESS : NSF_NOTFOUND;
}

/* /etc/hosts by address: the first line with it */
static int nsf_hosts_byaddr_files(const struct nsf_addr *key, struct nsf_hent *h) {
    struct nsf_addr k = *key;
    int mapped = k.af == NSF_AF6 && nsf_is_mapped(k.a);
    if (mapped) { memmove(k.a, k.a + 12, 4); k.af = NSF_AF4; }
    FILE *f = fopen("/etc/hosts", "r");
    if (!f) return NSF_UNAVAIL;
    char *line, *w[256];
    int found = 0;
    while ((line = nsf_getline(f))) {
        struct nsf_addr ad;
        int n;
        if (!nsf_hosts_line(line, k.af, &ad, w, &n) || !nsf_addr_eq(&ad, &k)) continue;
        h->name = nsf_strdup(n > 1 ? w[1] : "");
        for (int i = 2; i < n; i++) nsf_hent_alias(h, w[i]);
        nsf_hent_addr(h, key);
        found = 1;
        break;
    }
    fclose(f);
    return found ? NSF_SUCCESS : NSF_NOTFOUND;
}

#ifndef SU_HOST
static long nsf_sc2(long n, long a, long b) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(n), "D"(a), "S"(b) : "rcx", "r11", "memory");
    return r;
}
#endif

/* the dns service by name: on the guest the kernel's resolver, which
 * knows A records only; on Linux glibc's */
static int nsf_hosts_byname_dns(const char *name, int family, struct nsf_hent *h) {
#ifdef SU_HOST
    struct hostent *he = gethostbyname2(name, family == NSF_AF6 ? AF_INET6 : AF_INET);
    if (!he) return h_errno == TRY_AGAIN ? NSF_TRYAGAIN : NSF_NOTFOUND;
    h->name = nsf_strdup(he->h_name);
    for (char **a = he->h_aliases; a && *a; a++) nsf_hent_alias(h, *a);
    for (char **a = he->h_addr_list; a && *a; a++) {
        struct nsf_addr ad;
        ad.af = he->h_addrtype == AF_INET6 ? NSF_AF6 : NSF_AF4;
        memcpy(ad.a, *a, ad.af == NSF_AF6 ? 16 : 4);
        nsf_hent_addr(h, &ad);
    }
    return NSF_SUCCESS;
#else
    if (family == NSF_AF6) return NSF_NOTFOUND;
    uint32_t ip = 0;
    if (nsf_sc2(ABI_SYS_DNS_RESOLVE, (long)(uintptr_t)name, (long)(uintptr_t)&ip) < 0 || !ip)
        return NSF_NOTFOUND;
    struct nsf_addr ad;
    ad.af = NSF_AF4;
    memcpy(ad.a, &ip, 4);              /* network order already */
    h->name = nsf_strdup(name);
    nsf_hent_addr(h, &ad);
    return NSF_SUCCESS;
#endif
}

static int nsf_hosts_byaddr_dns(const struct nsf_addr *key, struct nsf_hent *h) {
#ifdef SU_HOST
    struct hostent *he = gethostbyaddr(key->a, key->af == NSF_AF6 ? 16 : 4,
                                       key->af == NSF_AF6 ? AF_INET6 : AF_INET);
    if (!he) return h_errno == TRY_AGAIN ? NSF_TRYAGAIN : NSF_NOTFOUND;
    h->name = nsf_strdup(he->h_name);
    for (char **a = he->h_aliases; a && *a; a++) nsf_hent_alias(h, *a);
    nsf_hent_addr(h, key);
    return NSF_SUCCESS;
#else
    (void)key; (void)h;
    return NSF_NOTFOUND;               /* the kernel resolves no PTR records */
#endif
}

struct nsf_hkey { const char *name; const struct nsf_addr *addr; int family; struct nsf_hent *h; };

static int nsf_hosts_svc(const char *svc, void *ctx) {
    struct nsf_hkey *k = (struct nsf_hkey *)ctx;
    int st;
    if (!strcmp(svc, "files"))
        st = k->addr ? nsf_hosts_byaddr_files(k->addr, k->h) : nsf_hosts_byname_files(k->name, k->family, k->h);
    else if (!strcmp(svc, "dns"))
        st = k->addr ? nsf_hosts_byaddr_dns(k->addr, k->h) : nsf_hosts_byname_dns(k->name, k->family, k->h);
    else return NSF_UNAVAIL;
    if (st != NSF_SUCCESS) nsf_hent_free(k->h);
    return st;
}

/* gethostbyname2(NAME, FAMILY): a name of only digits and dots is its
 * own answer for IPv4 (inet_aton's reading) and no answer for IPv6 */
static int nsf_gethostbyname(const char *name, int family, struct nsf_hent *h) {
    memset(h, 0, sizeof *h);
    if (*name && strspn(name, "0123456789.") == strlen(name)) {
        uint32_t v;
        if (family != NSF_AF4 || name[strlen(name) - 1] == '.' || !nsf_aton(name, &v)) return 0;
        struct nsf_addr ad;
        ad.af = NSF_AF4;
        ad.a[0] = (unsigned char)(v >> 24); ad.a[1] = (unsigned char)(v >> 16);
        ad.a[2] = (unsigned char)(v >> 8); ad.a[3] = (unsigned char)v;
        h->name = nsf_strdup(name);
        nsf_hent_addr(h, &ad);
        return 1;
    }
    struct nsf_hkey k = { name, 0, family, h };
    return nsf_run("hosts", nsf_hosts_svc, &k) == NSF_SUCCESS;
}

static int nsf_gethostbyaddr(const struct nsf_addr *a, struct nsf_hent *h) {
    memset(h, 0, sizeof *h);
    struct nsf_hkey k = { 0, a, a->af, h };
    return nsf_run("hosts", nsf_hosts_svc, &k) == NSF_SUCCESS;
}

/* getaddrinfo's answer: addresses (each once; getent shows the three
 * socket types of each) and the canonical name */
struct nsf_ai { struct nsf_addr *ad; int n, cap; char *canon; };

static void nsf_ai_add(struct nsf_ai *r, const struct nsf_addr *a) {
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 8;
        r->ad = (struct nsf_addr *)realloc(r->ad, (size_t)r->cap * sizeof *r->ad);
        if (!r->ad) su_error(1, ENOMEM, "memory exhausted");
    }
    r->ad[r->n++] = *a;
}

static void nsf_ai_free(struct nsf_ai *r) {
    free(r->ad);
    free(r->canon);
    memset(r, 0, sizeof *r);
}

static void nsf_map4(struct nsf_addr *a) {
    unsigned char v4[4];
    memcpy(v4, a->a, 4);
    memset(a->a, 0, 10);
    a->a[10] = a->a[11] = 0xff;
    memcpy(a->a + 12, v4, 4);
    a->af = NSF_AF6;
}

/* the order getaddrinfo's sorting gives on a machine whose only IPv6
 * route is loopback (measured: ::1 first, then IPv4, then other IPv6) */
static int nsf_ai_rank(const struct nsf_addr *a) {
    if (a->af == NSF_AF6 && nsf_is_loop6(a->a)) return 0;
    if (a->af == NSF_AF4 || nsf_is_mapped(a->a)) return 1;
    return 2;
}

static void nsf_ai_sort(struct nsf_ai *r) {
    for (int i = 1; i < r->n; i++) {
        struct nsf_addr x = r->ad[i];
        int j = i;
        while (j > 0 && nsf_ai_rank(&r->ad[j - 1]) > nsf_ai_rank(&x)) { r->ad[j] = r->ad[j - 1]; j--; }
        r->ad[j] = x;
    }
}

struct nsf_aikey { const char *name; int family; struct nsf_ai *r; };

static int nsf_ai_svc(const char *svc, void *ctx) {
    struct nsf_aikey *k = (struct nsf_aikey *)ctx;
    struct nsf_hent h;
    memset(&h, 0, sizeof h);
    int st;
    if (!strcmp(svc, "files")) {
        if (k->family == NSF_AF6) {
            st = nsf_hosts_byname_files(k->name, NSF_AF6, &h);
            if (st == NSF_NOTFOUND) {
                st = nsf_hosts_byname_files(k->name, NSF_AF4, &h);
                for (int i = 0; i < h.nad; i++) nsf_map4(&h.ad[i]);
            }
        } else st = nsf_hosts_byname_files(k->name, k->family, &h);
    } else if (!strcmp(svc, "dns")) {
#ifdef SU_HOST
        struct addrinfo hints, *res = 0;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = k->family == NSF_AF4 ? AF_INET : k->family == NSF_AF6 ? AF_INET6 : AF_UNSPEC;
        hints.ai_flags = AI_CANONNAME | (k->family == NSF_AF6 ? AI_V4MAPPED : 0);
        int e = getaddrinfo(k->name, 0, &hints, &res);
        if (e) return e == EAI_AGAIN ? NSF_TRYAGAIN : NSF_NOTFOUND;
        for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
            struct nsf_addr ad;
            if (ai->ai_family == AF_INET) {
                ad.af = NSF_AF4;
                memcpy(ad.a, &((struct sockaddr_in *)ai->ai_addr)->sin_addr, 4);
            } else if (ai->ai_family == AF_INET6) {
                ad.af = NSF_AF6;
                memcpy(ad.a, &((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr, 16);
            } else continue;
            int dup = 0;
            for (int i = 0; i < k->r->n && !dup; i++) dup = nsf_addr_eq(&k->r->ad[i], &ad);
            if (!dup) nsf_ai_add(k->r, &ad);
            if (ai->ai_canonname && !k->r->canon) k->r->canon = nsf_strdup(ai->ai_canonname);
        }
        freeaddrinfo(res);
        if (!k->r->canon) k->r->canon = nsf_strdup(k->name);
        return k->r->n ? NSF_SUCCESS : NSF_NOTFOUND;
#else
        st = nsf_hosts_byname_dns(k->name, NSF_AF4, &h);
        if (k->family == NSF_AF6) for (int i = 0; i < h.nad; i++) nsf_map4(&h.ad[i]);
#endif
    } else return NSF_UNAVAIL;
    if (st == NSF_SUCCESS) {
        for (int i = 0; i < h.nad; i++) nsf_ai_add(k->r, &h.ad[i]);
        k->r->canon = nsf_strdup(h.name);
        nsf_ai_sort(k->r);
    }
    nsf_hent_free(&h);
    return st;
}

/* getaddrinfo(NAME) for FAMILY (0 for any; NSF_AF6 maps IPv4 answers);
 * 1 when it answers. A numeric NAME answers itself, as inet_aton and
 * inet_pton read it. */
static int nsf_getaddrinfo(const char *name, int family, struct nsf_ai *r) {
    memset(r, 0, sizeof *r);
    struct nsf_addr ad;
    uint32_t v;
    if (nsf_pton6(name, ad.a)) {
        ad.af = NSF_AF6;
        if (family == NSF_AF4) {
            if (!nsf_is_mapped(ad.a)) return 0;
            memmove(ad.a, ad.a + 12, 4);
            ad.af = NSF_AF4;
        }
        nsf_ai_add(r, &ad);
        r->canon = nsf_strdup(name);
        return 1;
    }
    if (nsf_aton(name, &v)) {
        ad.af = NSF_AF4;
        ad.a[0] = (unsigned char)(v >> 24); ad.a[1] = (unsigned char)(v >> 16);
        ad.a[2] = (unsigned char)(v >> 8); ad.a[3] = (unsigned char)v;
        if (family == NSF_AF6) nsf_map4(&ad);
        nsf_ai_add(r, &ad);
        r->canon = nsf_strdup(name);
        return 1;
    }
    struct nsf_aikey k = { name, family, r };
    if (nsf_run("hosts", nsf_ai_svc, &k) == NSF_SUCCESS) return 1;
    nsf_ai_free(r);
    return 0;
}

/* ---- getent: the databases -------------------------------------------------- */

static int gent_passwd(int nk, char **keys) {
    if (!nk) {
        if (!nsf_has_service("passwd", "files")) return 0;
        FILE *f = fopen("/etc/passwd", "r");
        if (!f) return 0;
        char *line;
        while ((line = nsf_getline(f))) {
            struct nsf_pw pw;
            if (nsf_parse_pw(line, &pw)) nsf_put_pw(&pw);
        }
        fclose(f);
        return 0;
    }
    int rc = 0;
    for (int i = 0; i < nk; i++) {
        const char *end;
        unsigned long id = nsf_strtoul(keys[i], &end);
        int by_id = *keys[i] && end != keys[i] && !*end;
        struct nsf_pwkey k;
        if (nsf_getpw(keys[i], by_id, (uint32_t)id, &k)) {
            nsf_put_pw(&k.out);
            free(k.mem);
        } else rc = 2;
    }
    return rc;
}

static int gent_group(int nk, char **keys) {
    if (!nk) {
        if (!nsf_has_service("group", "files")) return 0;
        FILE *f = fopen("/etc/group", "r");
        if (!f) return 0;
        char *line;
        while ((line = nsf_getline(f))) {
            struct nsf_gr gr;
            if (nsf_parse_gr(line, &gr, 0)) { nsf_put_gr(&gr); free(gr.mem); }
        }
        fclose(f);
        return 0;
    }
    int rc = 0;
    for (int i = 0; i < nk; i++) {
        const char *end;
        unsigned long id = nsf_strtoul(keys[i], &end);
        int by_id = *keys[i] && end != keys[i] && !*end;
        char *l = nsf_getgr(keys[i], by_id, (uint32_t)id);
        if (!l) { rc = 2; continue; }
        struct nsf_gr gr;
        nsf_parse_gr(l, &gr, 0);
        nsf_put_gr(&gr);
        free(gr.mem);
        free(l);
    }
    return rc;
}

/* shadow: name:pw:lastchg:min:max:warn:inact:expire[:flag] */
static int nsf_shadow_line(char *line, char **fv) {
    while (nsf_blank((unsigned char)*line)) line++;
    if (!*line || *line == '#') return 0;
    int n = 0;
    char *p = line;
    while (p && n < 10) fv[n++] = nsf_field(&p);
    if (p || n < 8 || n > 9) return 0;
    if (n == 8) fv[8] = (char *)"";
    for (int i = 2; i < 9; i++) {
        for (char *q = fv[i]; *q; q++) if (!isdigit((unsigned char)*q)) return 0;
    }
    return 1;
}

static void nsf_put_num(const char *s, int last) {
    if (*s) printf("%lu", (unsigned long)strtoul(s, 0, 10));
    if (!last) putchar(':');
}

static void nsf_put_shadow(char **fv) {
    printf("%s:%s:", fv[0], fv[1]);
    for (int i = 2; i < 9; i++) nsf_put_num(fv[i], i == 8);
    putchar('\n');
}

static int gent_shadow(int nk, char **keys) {
    int has = nsf_has_service("shadow", "files");
    if (!nk) {
        FILE *f = has ? fopen("/etc/shadow", "r") : 0;
        if (!f) return 0;
        char *line, *fv[10];
        while ((line = nsf_getline(f))) if (nsf_shadow_line(line, fv)) nsf_put_shadow(fv);
        fclose(f);
        return 0;
    }
    int rc = 0;
    for (int i = 0; i < nk; i++) {
        FILE *f = has ? fopen("/etc/shadow", "r") : 0;
        int found = 0;
        if (f) {
            char *line, *fv[10];
            while ((line = nsf_getline(f)))
                if (nsf_shadow_line(line, fv) && !strcmp(fv[0], keys[i])) {
                    nsf_put_shadow(fv);
                    found = 1;
                    break;
                }
            fclose(f);
        }
        if (!found) rc = 2;
    }
    return rc;
}

/* gshadow: name[:passwd[:admins[:members]]], lists with blanks dropped */
static void nsf_put_list(char *s) {
    int first = 1;
    for (;;) {
        char *c = strchr(s, ',');
        if (c) *c = 0;
        while (nsf_blank((unsigned char)*s)) s++;
        if (*s) { printf("%s%s", first ? "" : ",", s); first = 0; }
        if (!c) break;
        s = c + 1;
    }
}

static int nsf_gshadow_line(char *line, char **fv) {
    while (nsf_blank((unsigned char)*line)) line++;
    if (!*line || *line == '#') return 0;
    char *p = line;
    for (int i = 0; i < 4; i++) {
        if (i == 3) { fv[3] = p ? p : (char *)""; break; }
        fv[i] = nsf_field(&p);
        if (!fv[i]) fv[i] = (char *)"";
    }
    return 1;
}

static void nsf_put_gshadow(char **fv) {
    printf("%s:%s:", fv[0], fv[1]);
    nsf_put_list(fv[2]);
    putchar(':');
    nsf_put_list(fv[3]);
    putchar('\n');
}

static int gent_gshadow(int nk, char **keys) {
    int has = nsf_has_service("gshadow", "files");
    if (!nk) {
        FILE *f = has ? fopen("/etc/gshadow", "r") : 0;
        if (!f) return 0;
        char *line, *fv[4];
        while ((line = nsf_getline(f))) if (nsf_gshadow_line(line, fv)) nsf_put_gshadow(fv);
        fclose(f);
        return 0;
    }
    int rc = 0;
    for (int i = 0; i < nk; i++) {
        FILE *f = has ? fopen("/etc/gshadow", "r") : 0;
        int found = 0;
        if (f) {
            char *line, *fv[4];
            while ((line = nsf_getline(f)))
                if (nsf_gshadow_line(line, fv) && !strcmp(fv[0], keys[i])) {
                    nsf_put_gshadow(fv);
                    found = 1;
                    break;
                }
            fclose(f);
        }
        if (!found) rc = 2;
    }
    return rc;
}

static void gent_put_hent(const struct nsf_hent *h) {
    for (int i = 0; i < h->nad; i++) {
        char b[64];
        nsf_ntop(&h->ad[i], b);
        printf("%-15s %s", b, h->name);
        for (int j = 0; j < h->nal; j++) printf(" %s", h->al[j]);
        putchar('\n');
    }
}

/* every line of /etc/hosts, as an IPv4 query sees them */
static void gent_hosts_list(void) {
    if (!nsf_has_service("hosts", "files")) return;
    FILE *f = fopen("/etc/hosts", "r");
    if (!f) return;
    char *line, *w[256];
    while ((line = nsf_getline(f))) {
        struct nsf_addr ad;
        int n;
        if (!nsf_hosts_line(line, NSF_AF4, &ad, w, &n)) continue;
        char b[64];
        nsf_ntop(&ad, b);
        printf("%-15s %s", b, n > 1 ? w[1] : "");
        for (int i = 2; i < n; i++) printf(" %s", w[i]);
        putchar('\n');
    }
    fclose(f);
}

static int gent_hosts(int nk, char **keys) {
    if (!nk) { gent_hosts_list(); return 0; }
    int rc = 0;
    for (int i = 0; i < nk; i++) {
        struct nsf_hent h;
        struct nsf_addr ad;
        int ok;
        if (nsf_pton6(keys[i], ad.a)) { ad.af = NSF_AF6; ok = nsf_gethostbyaddr(&ad, &h); }
        else if (nsf_pton4(keys[i], ad.a)) { ad.af = NSF_AF4; ok = nsf_gethostbyaddr(&ad, &h); }
        else {
            ok = nsf_gethostbyname(keys[i], NSF_AF6, &h);
            if (!ok) ok = nsf_gethostbyname(keys[i], NSF_AF4, &h);
        }
        if (!ok) { rc = 2; continue; }
        gent_put_hent(&h);
        nsf_hent_free(&h);
    }
    return rc;
}

static int gent_ahosts(int nk, char **keys, int family) {
    if (!nk) { gent_hosts_list(); return 0; }
    int rc = 0;
    static const char *const types[3] = { "STREAM", "DGRAM", "RAW" };
    for (int i = 0; i < nk; i++) {
        struct nsf_ai r;
        if (!nsf_getaddrinfo(keys[i], family, &r)) { rc = 2; continue; }
        for (int a = 0; a < r.n; a++)
            for (int t = 0; t < 3; t++) {
                char b[64];
                nsf_ntop(&r.ad[a], b);
                printf("%-15s %-6s %s\n", b, types[t], a == 0 && t == 0 ? r.canon : "");
            }
        nsf_ai_free(&r);
    }
    return rc;
}

/* services: name port/proto aliases... */
struct nsf_serv { char *name, *proto; unsigned port; char **al; int nal; };

static int nsf_serv_line(char *line, struct nsf_serv *s, char **w) {
    nsf_uncomment(line);
    int n = nsf_words(line, w, 256);
    if (n < 2) return 0;
    char *sl = strchr(w[1], '/');
    if (sl) *sl = 0;
    if (!*w[1] || strspn(w[1], "0123456789") != strlen(w[1])) return 0;
    unsigned long v = strtoul(w[1], 0, 10);
    s->name = w[0];
    s->port = (unsigned)(v & 0xffff);
    s->proto = sl ? sl + 1 : (char *)"";
    s->al = w + 2;
    s->nal = n - 2;
    return 1;
}

static void nsf_put_serv(const struct nsf_serv *s) {
    char pp[64];
    snprintf(pp, sizeof pp, "%u/%s", s->port, s->proto);
    printf("%-21s %s", s->name, pp);
    for (int i = 0; i < s->nal; i++) printf(" %s", s->al[i]);
    putchar('\n');
}

static int gent_services(int nk, char **keys) {
    int has = nsf_has_service("services", "files");
    if (!nk) {
        FILE *f = has ? fopen("/etc/services", "r") : 0;
        if (!f) return 0;
        char *line, *w[256];
        struct nsf_serv s;
        while ((line = nsf_getline(f))) if (nsf_serv_line(line, &s, w)) nsf_put_serv(&s);
        fclose(f);
        return 0;
    }
    int rc = 0;
    for (int i = 0; i < nk; i++) {
        char *key = nsf_strdup(keys[i]);
        char *proto = strchr(key, '/');
        if (proto) *proto++ = 0;
        int by_port = *key && strspn(key, "0123456789") == strlen(key);
        unsigned long port = by_port ? strtoul(key, 0, 10) : 0;   /* past 65535: none */
        FILE *f = has ? fopen("/etc/services", "r") : 0;
        int found = 0;
        if (f) {
            char *line, *w[256];
            struct nsf_serv s;
            while (!found && (line = nsf_getline(f))) {
                if (!nsf_serv_line(line, &s, w)) continue;
                if (proto && strcmp(proto, s.proto)) continue;
                if (by_port) found = port == s.port;
                else {
                    found = !strcmp(key, s.name);
                    for (int j = 0; j < s.nal && !found; j++) found = !strcmp(key, s.al[j]);
                }
                if (found) nsf_put_serv(&s);
            }
            fclose(f);
        }
        if (!found) rc = 2;
        free(key);
    }
    return rc;
}

/* protocols and rpc: name number aliases... */
static int nsf_numline(char *line, char **w, int *n, long *num) {
    nsf_uncomment(line);
    *n = nsf_words(line, w, 256);
    if (*n < 2 || strspn(w[1], "0123456789") != strlen(w[1])) return 0;
    *num = nsf_atoi(w[1]);
    return 1;
}

static int gent_numdb(int nk, char **keys, const char *db, const char *path, int rpc) {
    int has = nsf_has_service(db, "files");
    int rc = 0;
    for (int i = 0; i < (nk ? nk : 1); i++) {
        FILE *f = has ? fopen(path, "r") : 0;
        int found = 0;
        if (f) {
            char *line, *w[256];
            int n;
            long num;
            int by_num = nk && isdigit((unsigned char)keys[i][0]);
            long want = by_num ? nsf_atoi(keys[i]) : 0;
            while ((line = nsf_getline(f))) {
                if (!nsf_numline(line, w, &n, &num)) continue;
                int m = !nk;
                if (nk && by_num) m = num == want;
                else if (nk) {
                    m = !strcmp(keys[i], w[0]);
                    for (int j = 2; j < n && !m; j++) m = !strcmp(keys[i], w[j]);
                }
                if (!m) continue;
                if (rpc) {
                    printf("%-15s %ld", w[0], num);
                    if (n > 2) putchar(' ');
                } else printf("%-21s %ld", w[0], num);
                for (int j = 2; j < n; j++) printf(" %s", w[j]);
                putchar('\n');
                found = 1;
                if (nk) break;
            }
            fclose(f);
        }
        if (nk && !found) rc = 2;
    }
    return rc;
}

/* networks: name address aliases..., a short address left-justified */
static int nsf_net_line(char *line, char **w, int *n, uint32_t *net) {
    nsf_uncomment(line);
    *n = nsf_words(line, w, 256);
    int parts;
    if (*n < 2 || !nsf_inet_network(w[1], net, &parts)) return 0;
    *net <<= 8 * (4 - parts);
    return 1;
}

static int gent_networks(int nk, char **keys) {
    int has = nsf_has_service("networks", "files");
    int rc = 0;
    for (int i = 0; i < (nk ? nk : 1); i++) {
        FILE *f = has ? fopen("/etc/networks", "r") : 0;
        int found = 0;
        uint32_t want = 0;
        int by_addr = nk && strchr(keys[i], '.');
        if (by_addr && !nsf_aton(keys[i], &want)) { if (f) fclose(f); rc = 2; continue; }
        if (f) {
            char *line, *w[256];
            int n;
            uint32_t net;
            while ((line = nsf_getline(f))) {
                if (!nsf_net_line(line, w, &n, &net)) continue;
                int m = !nk;
                if (nk && by_addr) m = net == want;
                else if (nk) {
                    m = !nsf_casecmp(keys[i], w[0]);
                    for (int j = 2; j < n && !m; j++) m = !nsf_casecmp(keys[i], w[j]);
                }
                if (!m) continue;
                char b[20];
                unsigned char a[4] = { (unsigned char)(net >> 24), (unsigned char)(net >> 16),
                                       (unsigned char)(net >> 8), (unsigned char)net };
                nsf_ntop4(a, b);
                printf("%-21s %s", w[0], b);
                for (int j = 2; j < n; j++) printf(" %s", w[j]);
                putchar('\n');
                found = 1;
                if (nk) break;
            }
            fclose(f);
        }
        if (nk && !found) rc = 2;
    }
    return rc;
}

/* ethers: six hex fields of one or two digits */
static int nsf_ether_aton(const char *s, unsigned char m[6], const char **end) {
    for (int i = 0; i < 6; i++) {
        if (!isxdigit((unsigned char)*s)) return 0;
        unsigned v = 0;
        int d = 0;
        while (isxdigit((unsigned char)*s) && d < 2) {
            v = v * 16 + (unsigned)(isdigit((unsigned char)*s) ? *s - '0' : tolower((unsigned char)*s) - 'a' + 10);
            s++;
            d++;
        }
        m[i] = (unsigned char)v;
        if (i < 5) { if (*s != ':') return 0; s++; }
    }
    *end = s;
    return 1;
}

static int gent_ethers(int nk, char **keys) {
    if (!nk) {
        fflush(stdout);
        fprintf(stderr, "Enumeration not supported on ethers\n");
        return 3;
    }
    int has = nsf_has_service("ethers", "files");
    int rc = 0;
    for (int i = 0; i < nk; i++) {
        unsigned char want[6];
        const char *e;
        int by_mac = nsf_ether_aton(keys[i], want, &e) && !*e;
        FILE *f = has ? fopen("/etc/ethers", "r") : 0;
        int found = 0;
        if (f) {
            char *line, *w[4];
            while (!found && (line = nsf_getline(f))) {
                nsf_uncomment(line);
                unsigned char m[6];
                const char *end;
                char *p = line;
                while (nsf_blank((unsigned char)*p)) p++;
                if (!nsf_ether_aton(p, m, &end) || !nsf_blank((unsigned char)*end)) continue;
                if (nsf_words((char *)end, w, 4) < 1) continue;
                if (by_mac ? memcmp(m, want, 6) != 0 : nsf_casecmp(w[0], keys[i]) != 0) continue;
                printf("%x:%x:%x:%x:%x:%x %s\n", m[0], m[1], m[2], m[3], m[4], m[5],
                       by_mac ? w[0] : keys[i]);
                found = 1;
            }
            fclose(f);
        }
        if (!found) rc = 2;
    }
    return rc;
}

/* aliases: "name: member, member", continued on lines that start blank */
static int gent_aliases(int nk, char **keys) {
    int has = nsf_has_service("aliases", "files");
    FILE *f = has ? fopen("/etc/aliases", "r") : 0;
    char **ent = 0;
    int ne = 0, cap = 0;
    if (f) {
        char *line;
        while ((line = nsf_getline(f))) {
            nsf_uncomment(line);
            if (nsf_blank((unsigned char)*line) && ne) {
                char *p = line;
                while (nsf_blank((unsigned char)*p)) p++;
                if (!*p) continue;
                size_t l = strlen(ent[ne - 1]);
                ent[ne - 1] = (char *)realloc(ent[ne - 1], l + strlen(p) + 2);
                if (!ent[ne - 1]) su_error(1, ENOMEM, "memory exhausted");
                strcat(ent[ne - 1], p);
                continue;
            }
            char *c = strchr(line, ':');
            if (!c || c == line) continue;
            if (ne == cap) {
                cap = cap ? cap * 2 : 16;
                ent = (char **)realloc(ent, (size_t)cap * sizeof *ent);
                if (!ent) su_error(1, ENOMEM, "memory exhausted");
            }
            ent[ne++] = nsf_strdup(line);
        }
        fclose(f);
    }
    int rc = 0;
    for (int i = 0; i < (nk ? nk : 1); i++) {
        int found = 0;
        for (int e = 0; e < ne; e++) {
            char *copy = nsf_strdup(ent[e]);
            char *c = strchr(copy, ':');
            *c = 0;
            if (nk && nsf_casecmp(copy, keys[i])) { free(copy); continue; }
            char head[512];
            snprintf(head, sizeof head, "%s:", copy);
            printf("%-15s ", head);
            char *s = c + 1;
            int first = 1;
            for (;;) {
                char *k = strchr(s, ',');
                if (k) *k = 0;
                while (nsf_space((unsigned char)*s)) s++;
                size_t l = strlen(s);
                while (l && nsf_space((unsigned char)s[l - 1])) s[--l] = 0;
                if (*s) { printf("%s%s", first ? "" : ", ", s); first = 0; }
                if (!k) break;
                s = k + 1;
            }
            putchar('\n');
            free(copy);
            found = 1;
            if (nk) break;
        }
        if (nk && !found) rc = 2;
    }
    for (int e = 0; e < ne; e++) free(ent[e]);
    free(ent);
    return rc;
}

/* netgroup: name then (host,user,domain) triples and other netgroups */
struct nsf_triple { char *h, *u, *d; };
struct nsf_ng { struct nsf_triple *t; int n, cap; };

static char *nsf_ng_line(const char *name) {
    if (!nsf_has_service("netgroup", "files")) return 0;
    FILE *f = fopen("/etc/netgroup", "r");
    if (!f) return 0;
    char *line, *r = 0;
    size_t l = strlen(name);
    while ((line = nsf_getline(f))) {
        nsf_uncomment(line);
        char *p = line;
        while (nsf_blank((unsigned char)*p)) p++;
        if (strncmp(p, name, l) || (p[l] && !nsf_blank((unsigned char)p[l]))) continue;
        r = nsf_strdup(p + l);
        break;
    }
    fclose(f);
    return r;
}

static void nsf_ng_expand(const char *name, struct nsf_ng *ng, int depth, int *found) {
    if (depth > 32) return;
    char *l = nsf_ng_line(name);
    if (!l) return;
    if (!depth) *found = 1;
    char *groups[64];
    int ngr = 0;
    char *p = l;
    for (;;) {
        while (nsf_blank((unsigned char)*p)) p++;
        if (!*p) break;
        if (*p == '(') {
            char *e = strchr(p, ')');
            if (!e) break;
            *e = 0;
            char *f1 = p + 1, *c1 = strchr(f1, ',');
            char *f2 = c1 ? c1 + 1 : (char *)"", *c2 = c1 ? strchr(f2, ',') : 0;
            char *f3 = c2 ? c2 + 1 : (char *)"";
            if (c1) *c1 = 0;
            if (c2) *c2 = 0;
            if (ng->n == ng->cap) {
                ng->cap = ng->cap ? ng->cap * 2 : 16;
                ng->t = (struct nsf_triple *)realloc(ng->t, (size_t)ng->cap * sizeof *ng->t);
                if (!ng->t) su_error(1, ENOMEM, "memory exhausted");
            }
            ng->t[ng->n].h = nsf_strdup(f1);
            ng->t[ng->n].u = nsf_strdup(f2);
            ng->t[ng->n].d = nsf_strdup(f3);
            ng->n++;
            p = e + 1;
            continue;
        }
        char *s = p;
        while (*p && !nsf_blank((unsigned char)*p)) p++;
        if (*p) *p++ = 0;
        if (ngr < 64) groups[ngr++] = s;
    }
    /* a netgroup's own triples, then those of the ones it names */
    for (int i = 0; i < ngr; i++) nsf_ng_expand(groups[i], ng, depth + 1, found);
    free(l);
}

static int gent_netgroup(int nk, char **keys) {
    if (!nk) {
        fflush(stdout);
        fprintf(stderr, "Enumeration not supported on netgroup\n");
        return 3;
    }
    struct nsf_ng ng = { 0, 0, 0 };
    int found = 0;
    nsf_ng_expand(keys[0], &ng, 0, &found);
    int rc = 0;
    if (nk == 4) {
        /* innetgr: an empty argument, or an empty field, matches anything */
        int in = 0;
        for (int i = 0; i < ng.n && !in; i++) {
            const struct nsf_triple *t = &ng.t[i];
            in = (!*keys[1] || !*t->h || !strcmp(t->h, keys[1])) &&
                 (!*keys[2] || !*t->u || !strcmp(t->u, keys[2])) &&
                 (!*keys[3] || !*t->d || !strcmp(t->d, keys[3]));
        }
        printf("%-21s (%s,%s,%s) = %d\n", keys[0], keys[1], keys[2], keys[3], in);
    } else if (nk == 1) {
        if (!found) rc = 2;
        else {
            printf("%-21s", keys[0]);
            /* an empty host shows as a blank, as glibc's does */
            for (int i = 0; i < ng.n; i++)
                printf(" (%s,%s,%s)", *ng.t[i].h ? ng.t[i].h : " ", ng.t[i].u, ng.t[i].d);
            putchar('\n');
        }
    }
    for (int i = 0; i < ng.n; i++) { free(ng.t[i].h); free(ng.t[i].u); free(ng.t[i].d); }
    free(ng.t);
    return rc;
}

static int gent_initgroups(int nk, char **keys) {
    if (!nk) {
        fflush(stdout);
        fprintf(stderr, "Enumeration not supported on initgroups\n");
        return 3;
    }
    for (int i = 0; i < nk; i++) {
        struct nsf_initg k;
        nsf_initgroups(keys[i], -1, &k);
        printf("%-21s", keys[i]);
        for (int j = 0; j < k.n; j++) printf(" %lu", (unsigned long)k.g[j]);
        putchar('\n');
        free(k.g);
    }
    return 0;
}

/* ---- getent ----------------------------------------------------------------- */

static void gent_try(FILE *to) {
    fprintf(to, "Try `%s --help' or `%s --usage' for more information.\n", su_short, su_short);
}

static void gent_usage(void) {
    printf("Usage: %s [-Ai?V] [-s CONFIG] [--no-addrconfig] [--no-idn]\n"
           "            [--service=CONFIG] [--help] [--usage] [--version]\n"
           "            database [key ...]\n", su_short);
}

static void gent_help(void) {
    printf("Usage: %s [OPTION...] database [key ...]\n"
           "Look KEYs up in one of the system's databases, or list all of it.\n"
           "\n"
           "  -A, --no-addrconfig        keep IPv4/IPv6 answers this machine could not\n"
           "                             use (ahosts*)\n"
           "  -i, --no-idn               leave internationalized names unencoded\n"
           "  -s, --service=CONFIG       the services to ask, as in nsswitch.conf\n"
           "                             (DATABASE:SERVICES for one database)\n"
           "  -?, --help                 show this help and exit\n"
           "      --usage                show the short usage and exit\n"
           "  -V, --version              show the version and exit\n"
           "\n"
           "On this system the services are the files in /etc (files) and, for\n"
           "hosts, the DNS resolver (dns).\n"
           "\n"
           "Databases:\n"
           "ahosts ahostsv4 ahostsv6 aliases ethers group gshadow hosts initgroups\n"
           "netgroup networks passwd protocols rpc services shadow\n", su_short);
}

static int do_getent(int argc, char **argv) {
    enum { USAGE_OPT = 0x100 };
    static const struct su_option longs[] = {
        { "no-idn", SU_NO_ARG, 0, 'i' },
        { "no-addrconfig", SU_NO_ARG, 0, 'A' },
        { "service", SU_REQ_ARG, 0, 's' },
        { "help", SU_NO_ARG, 0, '?' },
        { "usage", SU_NO_ARG, 0, USAGE_OPT },
        { "version", SU_NO_ARG, 0, 'V' },
        { 0, 0, 0, 0 }
    };
    int c;
    for (;;) {
        su_optopt = -1;
        c = su_getopt_long(argc, argv, "Ai?Vs:", longs, 0);
        if (c == -1) break;
        switch (c) {
        case 'A': case 'i': break;
        case 's': {
            const char *colon = strchr(su_optarg, ':');
            if (!colon) {
                free(nsf_override_all);
                nsf_override_all = nsf_strdup(su_optarg);
                break;
            }
            char db[64];
            snprintf(db, sizeof db, "%.*s", (int)(colon - su_optarg), su_optarg);
            int i = nsf_dbindex(nsf_dbbase(db));
            if (i < 0) su_error(1, 0, "Unknown database name");
            free(nsf_override[i]);
            nsf_override[i] = nsf_strdup(colon + 1);
            break;
        }
        case USAGE_OPT: gent_usage(); return 0;
        case 'V': return su_version();
        case '?':
            if (su_optopt == -1) { gent_help(); return 0; }
            gent_try(stderr);              /* a bad option, as argp says it */
            return 64;
        default:
            gent_try(stderr);
            return 64;
        }
    }
    if (su_optind >= argc) {
        su_error(0, 0, "wrong number of arguments");
        gent_try(stdout);
        return 1;
    }
    const char *db = argv[su_optind];
    char **keys = argv + su_optind + 1;
    int nk = argc - su_optind - 1, rc;
    if (!strcmp(db, "passwd")) rc = gent_passwd(nk, keys);
    else if (!strcmp(db, "group")) rc = gent_group(nk, keys);
    else if (!strcmp(db, "shadow")) rc = gent_shadow(nk, keys);
    else if (!strcmp(db, "gshadow")) rc = gent_gshadow(nk, keys);
    else if (!strcmp(db, "hosts")) rc = gent_hosts(nk, keys);
    else if (!strcmp(db, "ahosts")) rc = gent_ahosts(nk, keys, 0);
    else if (!strcmp(db, "ahostsv4")) rc = gent_ahosts(nk, keys, NSF_AF4);
    else if (!strcmp(db, "ahostsv6")) rc = gent_ahosts(nk, keys, NSF_AF6);
    else if (!strcmp(db, "services")) rc = gent_services(nk, keys);
    else if (!strcmp(db, "protocols")) rc = gent_numdb(nk, keys, "protocols", "/etc/protocols", 0);
    else if (!strcmp(db, "rpc")) rc = gent_numdb(nk, keys, "rpc", "/etc/rpc", 1);
    else if (!strcmp(db, "networks")) rc = gent_networks(nk, keys);
    else if (!strcmp(db, "ethers")) rc = gent_ethers(nk, keys);
    else if (!strcmp(db, "aliases")) rc = gent_aliases(nk, keys);
    else if (!strcmp(db, "netgroup")) rc = gent_netgroup(nk, keys);
    else if (!strcmp(db, "initgroups")) rc = gent_initgroups(nk, keys);
    else {
        fflush(stdout);
        fprintf(stderr, "Unknown database: %s\n", db);
        gent_try(stdout);
        return 1;
    }
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return rc;
}

/* ---- groups ----------------------------------------------------------------- */

/* print group GID's name (or its number, and say so); 0 when it has none */
static int grps_put(uint32_t gid, int first) {
    char nm[1024];
    if (!first) putchar(' ');
    if (nsf_group_name(gid, nm, sizeof nm)) { fputs(nm, stdout); return 1; }
    su_error(0, 0, "cannot find name for group ID %lu", (unsigned long)gid);
    printf("%lu", (unsigned long)gid);
    return 0;
}

static int do_groups(int argc, char **argv) {
    static const struct su_option longs[] = { SU_LONG_HV, { 0, 0, 0, 0 } };
    int c;
    while ((c = su_getopt_long(argc, argv, "", longs, 0)) != -1) {
        if (c == SU_HELP_OPT) {
            printf("Usage: %s [OPTION]... [USERNAME]...\n"
                   "Print the groups each USERNAME belongs to -- or, with none, the groups\n"
                   "this process has, which can differ from what the group database now\n"
                   "says.\n"
                   "      --help        show this help and exit\n"
                   "      --version     show the version and exit\n",
                   su_name);
            return 0;
        }
        if (c == SU_VERSION_OPT) return su_version();
        su_try_help(1);
    }
    int ok = 1;
    if (su_optind >= argc) {
        /* the process's: real gid, effective gid, then the supplementary
         * ones, each once (measured: getgroups' duplicates are dropped) */
        int n = getgroups(0, 0);
        if (n < 0) n = 0;
        gid_t *g = (gid_t *)nsf_alloc(((size_t)n + 2) * sizeof *g);
        int m = n ? getgroups(n, g + 2) : 0;
        if (m < 0) m = 0;
        g[0] = getgid();
        g[1] = getegid();
        uint32_t *out = (uint32_t *)nsf_alloc(((size_t)m + 2) * sizeof *out);
        int k = 0;
        for (int i = 0; i < m + 2; i++) {
            int dup = 0;
            for (int j = 0; j < k && !dup; j++) dup = out[j] == (uint32_t)g[i];
            if (!dup) out[k++] = (uint32_t)g[i];
        }
        for (int i = 0; i < k; i++) ok &= grps_put(out[i], i == 0);
        putchar('\n');
        free(g);
        free(out);
    } else {
        for (int i = su_optind; i < argc; i++) {
            struct nsf_pwkey pk;
            if (!nsf_getpw(argv[i], 0, 0, &pk)) {
                su_error(0, 0, "%s: no such user", su_quote(argv[i]));
                ok = 0;
                continue;
            }
            /* the passwd entry's group, then getgrouplist's: the groups
             * that list the user, in the file's order, less that one */
            uint32_t primary = pk.out.gid;
            free(pk.mem);
            printf("%s : ", argv[i]);
            ok &= grps_put(primary, 1);
            struct nsf_initg k;
            nsf_initgroups(argv[i], primary, &k);
            for (int j = 0; j < k.n; j++) ok &= grps_put(k.g[j], 0);
            free(k.g);
            putchar('\n');
        }
    }
    if (fflush(stdout) != 0 || ferror(stdout)) su_error(1, errno, "write error");
    return ok ? 0 : 1;
}
