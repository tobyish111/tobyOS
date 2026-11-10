/* ftp / ftpd -- an RFC 959 client and the server it is tested against.
 *
 *     ftpd [-p port] [-d root] [-v]        serve, until killed
 *     ftp  [-n] [-v] [HOST [PORT]]         read commands from stdin
 *
 * THE SERVER EXISTS SO THE CLIENT CAN BE TESTED. `ftp` was the one absent
 * distro utility that was writable but had nothing to talk to: shipping a
 * network client that had never completed a transfer would have been a
 * guess. ftpd runs in the boot harness, ftp drives it, and the gate
 * compares the bytes that came back against the bytes that went out.
 *
 * What is implemented, on both sides: USER/PASS, PWD, CWD, CDUP, TYPE,
 * PASV, LIST, NLST, RETR, STOR, DELE, MKD, RMD, SIZE, SYST, NOOP, QUIT.
 *
 * PASV ONLY. Active mode (PORT) has the server open a connection back to
 * the client, which is the half of FTP that firewalls broke and that no
 * modern client defaults to; leaving it out is a decision, not an
 * oversight, and `ftp` says so if a server refuses PASV.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#include <toby/net.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

static const char *prog = "ftp";
static int verbose;

static u16 htons_(u16 v) { return (u16)((v >> 8) | (v << 8)); }

static void yield_(void) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "0"(5L) : "rcx", "r11", "memory");
    (void)r;
}

static int parse_ipv4(const char *s, u32 *out_be) {
    u32 v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        int n = 0;
        while (*s >= '0' && *s <= '9') { n = n * 10 + (*s++ - '0'); if (n > 255) return 0; }
        v = (v << 8) | (u32)n;
        if (part < 3) { if (*s != '.') return 0; s++; }
    }
    if (*s) return 0;
    *out_be = ((v & 0xFF) << 24) | ((v & 0xFF00) << 8)
            | ((v >> 8) & 0xFF00) | ((v >> 24) & 0xFF);
    return 1;
}

/* ------------------------------------------------------- line-oriented IO
 *
 * FTP's control channel is CRLF-terminated lines. TCP gives no message
 * boundaries, so a reply can arrive split across reads or two replies can
 * arrive in one -- both happen on loopback. Everything goes through this
 * buffer so neither case is a special case at the call sites.
 */
struct line_io {
    int  conn;
    char buf[4096];
    size_t len;
};

static void lio_init(struct line_io *l, int conn) { l->conn = conn; l->len = 0; }

/* Reads one CRLF line into `out` (without the terminator). Returns 1 on a
 * line, 0 on clean EOF, -1 on error or timeout. */
/* WHAT tcp_recv RETURNS, because getting this backwards is silent:
 *
 *     > 0   bytes, and it returned as soon as they arrived
 *     == 0  the call TIMED OUT with nothing to show -- the connection is
 *           fine, the peer has simply not spoken yet
 *     < 0   the peer closed, or the connection failed
 *
 * Reading 0 as end-of-file is the mistake this file made first: the FTP
 * client asked for the server greeting, the read returned 0 the moment the
 * ten-second cap expired, and the client reported "no greeting from the
 * server" about a server that was sitting there perfectly happily. The
 * kernel's cap is 10 s per call, so a caller that wants to wait longer
 * calls again rather than treating the first 0 as an answer.
 */
static int lio_getline(struct line_io *l, char *out, size_t cap, int tries) {
    int empty = 0;
    for (;;) {
        for (size_t i = 0; i + 1 <= l->len; i++) {
            if (l->buf[i] != '\n') continue;
            size_t n = i;
            if (n && l->buf[n - 1] == '\r') n--;
            if (n >= cap) n = cap - 1;
            memcpy(out, l->buf, n);
            out[n] = 0;
            memmove(l->buf, l->buf + i + 1, l->len - i - 1);
            l->len -= i + 1;
            return 1;
        }
        if (l->len == sizeof l->buf) return -1;      /* a line longer than the buffer */
        int n = toby_tcp_recv(l->conn, l->buf + l->len, sizeof l->buf - l->len);
        if (n > 0) { l->len += (size_t)n; empty = 0; continue; }
        if (n < 0) return 0;                    /* peer closed */
        if (++empty >= tries) return -1;        /* nothing, for long enough */
    }
}

static int send_all(int conn, const void *data, size_t len) {
    const char *p = (const char *)data;
    size_t off = 0;
    while (off < len) {
        int n = toby_tcp_send(conn, p + off, len - off);
        if (n < 0) return -1;
        if (n == 0) { yield_(); continue; }
        off += (size_t)n;
    }
    return 0;
}

static int send_line(int conn, const char *fmt, const char *a, int n) {
    char buf[1024];
    int len;
    if (a && n >= 0)      len = snprintf(buf, sizeof buf, fmt, n, a);
    else if (a)           len = snprintf(buf, sizeof buf, fmt, a);
    else if (n >= 0)      len = snprintf(buf, sizeof buf, fmt, n);
    else                  len = snprintf(buf, sizeof buf, "%s", fmt);
    if (len < 0) return -1;
    if ((size_t)len > sizeof buf - 3) len = (int)sizeof buf - 3;
    buf[len++] = '\r';
    buf[len++] = '\n';
    return send_all(conn, buf, (size_t)len);
}

/* ====================================================================== */
/*                                 SERVER                                 */
/* ====================================================================== */

static char g_root[512] = "/";
static char g_cwd[512]  = "/";

/* Joins the session's cwd with a client-supplied path and REFUSES anything
 * that climbs out of the served root. Without this, `RETR ../../etc/passwd`
 * is a valid request. */
static int resolve_path(const char *arg, char *out, size_t cap) {
    char tmp[1024];
    if (!arg || !*arg) snprintf(tmp, sizeof tmp, "%s", g_cwd);
    else if (arg[0] == '/') snprintf(tmp, sizeof tmp, "%s", arg);
    else snprintf(tmp, sizeof tmp, "%s/%s", strcmp(g_cwd, "/") ? g_cwd : "", arg);

    /* Collapse . and .. textually; there is no realpath() here, and doing
     * it textually is what makes the containment check meaningful. */
    char parts[32][128];
    int np = 0;
    const char *p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *e = p;
        while (*e && *e != '/') e++;
        size_t l = (size_t)(e - p);
        if (l == 1 && p[0] == '.') { p = e; continue; }
        if (l == 2 && p[0] == '.' && p[1] == '.') { if (np) np--; p = e; continue; }
        if (l >= sizeof parts[0]) return 0;
        if (np >= 32) return 0;
        memcpy(parts[np], p, l);
        parts[np][l] = 0;
        np++;
        p = e;
    }
    size_t n = 0;
    out[0] = 0;
    if (strcmp(g_root, "/")) n += (size_t)snprintf(out + n, cap - n, "%s", g_root);
    for (int i = 0; i < np; i++)
        n += (size_t)snprintf(out + n, cap - n, "/%s", parts[i]);
    if (!out[0]) snprintf(out, cap, "/");
    return 1;
}

/* Rebuilds the session-visible cwd (root-relative) from a resolved path. */
static void set_cwd_from(const char *resolved) {
    size_t rl = strcmp(g_root, "/") ? strlen(g_root) : 0;
    const char *rel = resolved + rl;
    if (!*rel) rel = "/";
    snprintf(g_cwd, sizeof g_cwd, "%s", rel);
}

/* ls -l shaped listing, which is what every client parses. */
static void list_into(int dconn, const char *path, int names_only) {
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        struct stat st;
        int isdir = 0;
        long size = 0;
        if (stat(full, &st) == 0) {
            isdir = S_ISDIR(st.st_mode) ? 1 : 0;
            size = (long)st.st_size;
        }
        char line[1024];
        int n;
        if (names_only)
            n = snprintf(line, sizeof line, "%s\r\n", e->d_name);
        else
            n = snprintf(line, sizeof line,
                         "%crw-r--r-- 1 root root %8ld Jan  1 00:00 %s\r\n",
                         isdir ? 'd' : '-', size, e->d_name);
        send_all(dconn, line, (size_t)n);
    }
    closedir(d);
}

/* Opens the PASV data listener and reports it. Returns the listen id. */
static int open_pasv(int cconn, u16 *port_out) {
    /* A fixed span keeps this predictable in a test harness; a real server
     * would take any ephemeral port. */
    for (u16 port = 50000; port < 50040; port++) {
        int lid = toby_tcp_listen(htons_(port), 1);
        if (lid < 0) continue;
        u32 me = 0;
        /* The address the client should dial. Loopback is what the harness
         * uses and the only thing this server is reachable on in practice. */
        parse_ipv4("127.0.0.1", &me);
        char msg[128];
        int n = snprintf(msg, sizeof msg,
                         "227 Entering Passive Mode (127,0,0,1,%u,%u)\r\n",
                         (unsigned)(port >> 8), (unsigned)(port & 0xFF));
        send_all(cconn, msg, (size_t)n);
        *port_out = port;
        return lid;
    }
    send_line(cconn, "425 Cannot open a data port", NULL, -1);
    return -1;
}

static int accept_data(int lid) {
    /* toby_tcp_accept already blocks in the kernel until a connection
     * arrives or 30 s pass. Calling it in a spin loop does not make it more
     * responsive -- it makes each failure cost thirty seconds again. */
    return toby_tcp_accept(lid);
}

static int serve_session(int cconn) {
    struct line_io lio;
    lio_init(&lio, cconn);
    snprintf(g_cwd, sizeof g_cwd, "%s", "/");

    send_line(cconn, "220 tobyOS FTP server ready", NULL, -1);

    int logged_in = 0;
    int data_lid = -1;
    u16 data_port = 0;
    int binary = 1;

    for (;;) {
        char line[1024];
        int r = lio_getline(&lio, line, sizeof line, 30);
        if (r <= 0) break;
        if (verbose) fprintf(stderr, "ftpd: < %s\n", line);

        char verb[16];
        const char *arg = "";
        size_t i = 0;
        while (i < sizeof verb - 1 && line[i] && line[i] != ' ') {
            verb[i] = (line[i] >= 'a' && line[i] <= 'z') ? (char)(line[i] - 32) : line[i];
            i++;
        }
        verb[i] = 0;
        if (line[i] == ' ') arg = line + i + 1;

        if (!strcmp(verb, "USER")) { send_line(cconn, "331 Password required", NULL, -1); continue; }
        if (!strcmp(verb, "PASS")) { logged_in = 1; send_line(cconn, "230 Logged in", NULL, -1); continue; }
        if (!strcmp(verb, "QUIT")) { send_line(cconn, "221 Goodbye", NULL, -1); break; }
        if (!strcmp(verb, "NOOP")) { send_line(cconn, "200 OK", NULL, -1); continue; }
        if (!strcmp(verb, "SYST")) { send_line(cconn, "215 UNIX Type: L8", NULL, -1); continue; }

        if (!logged_in) { send_line(cconn, "530 Log in first", NULL, -1); continue; }

        if (!strcmp(verb, "TYPE")) {
            binary = (arg[0] == 'I' || arg[0] == 'i');
            send_line(cconn, "200 Type set", NULL, -1);
            continue;
        }
        if (!strcmp(verb, "PWD") || !strcmp(verb, "XPWD")) {
            char msg[600];
            snprintf(msg, sizeof msg, "257 \"%s\"", g_cwd);
            send_line(cconn, msg, NULL, -1);
            continue;
        }
        if (!strcmp(verb, "CWD") || !strcmp(verb, "CDUP")) {
            char path[1024];
            const char *target = !strcmp(verb, "CDUP") ? ".." : arg;
            if (!resolve_path(target, path, sizeof path)) {
                send_line(cconn, "550 Bad path", NULL, -1);
                continue;
            }
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
                send_line(cconn, "550 No such directory", NULL, -1);
                continue;
            }
            set_cwd_from(path);
            send_line(cconn, "250 Directory changed", NULL, -1);
            continue;
        }
        if (!strcmp(verb, "PASV")) {
            if (data_lid >= 0) { toby_tcp_close(data_lid); data_lid = -1; }
            data_lid = open_pasv(cconn, &data_port);
            continue;
        }
        if (!strcmp(verb, "PORT")) {
            send_line(cconn, "502 Active mode is not supported here -- use PASV", NULL, -1);
            continue;
        }
        if (!strcmp(verb, "SIZE")) {
            char path[1024];
            struct stat st;
            if (!resolve_path(arg, path, sizeof path) || stat(path, &st) != 0) {
                send_line(cconn, "550 No such file", NULL, -1);
            } else {
                char msg[64];
                snprintf(msg, sizeof msg, "213 %ld", (long)st.st_size);
                send_line(cconn, msg, NULL, -1);
            }
            continue;
        }
        if (!strcmp(verb, "DELE")) {
            char path[1024];
            if (!resolve_path(arg, path, sizeof path) || unlink(path) != 0)
                send_line(cconn, "550 Cannot delete", NULL, -1);
            else
                send_line(cconn, "250 Deleted", NULL, -1);
            continue;
        }
        if (!strcmp(verb, "MKD") || !strcmp(verb, "XMKD")) {
            char path[1024];
            if (!resolve_path(arg, path, sizeof path) || mkdir(path, 0755) != 0)
                send_line(cconn, "550 Cannot create directory", NULL, -1);
            else {
                char msg[600];
                snprintf(msg, sizeof msg, "257 \"%s\" created", arg);
                send_line(cconn, msg, NULL, -1);
            }
            continue;
        }
        if (!strcmp(verb, "RMD") || !strcmp(verb, "XRMD")) {
            char path[1024];
            if (!resolve_path(arg, path, sizeof path) || rmdir(path) != 0)
                send_line(cconn, "550 Cannot remove directory", NULL, -1);
            else
                send_line(cconn, "250 Removed", NULL, -1);
            continue;
        }

        if (!strcmp(verb, "LIST") || !strcmp(verb, "NLST") ||
            !strcmp(verb, "RETR") || !strcmp(verb, "STOR")) {
            if (data_lid < 0) {
                send_line(cconn, "425 Use PASV first", NULL, -1);
                continue;
            }
            char path[1024];
            if (!resolve_path(arg, path, sizeof path)) {
                send_line(cconn, "550 Bad path", NULL, -1);
                toby_tcp_close(data_lid); data_lid = -1;
                continue;
            }

            /* RETR/STOR must fail BEFORE the 150, or the client opens a data
             * connection for a transfer that was never going to happen and
             * then waits for bytes that never come. */
            FILE *fp = NULL;
            if (!strcmp(verb, "RETR")) {
                fp = fopen(path, "rb");
                if (!fp) {
                    send_line(cconn, "550 No such file", NULL, -1);
                    toby_tcp_close(data_lid); data_lid = -1;
                    continue;
                }
            } else if (!strcmp(verb, "STOR")) {
                fp = fopen(path, "wb");
                if (!fp) {
                    send_line(cconn, "550 Cannot create file", NULL, -1);
                    toby_tcp_close(data_lid); data_lid = -1;
                    continue;
                }
            }

            send_line(cconn, "150 Opening data connection", NULL, -1);
            int dconn = accept_data(data_lid);
            if (dconn < 0) {
                if (fp) fclose(fp);
                toby_tcp_close(data_lid); data_lid = -1;
                send_line(cconn, "425 No data connection", NULL, -1);
                continue;
            }

            if (!strcmp(verb, "LIST"))      list_into(dconn, path, 0);
            else if (!strcmp(verb, "NLST")) list_into(dconn, path, 1);
            else if (!strcmp(verb, "RETR")) {
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof buf, fp)) > 0)
                    if (send_all(dconn, buf, n) != 0) break;
                fclose(fp);
            } else {
                char buf[4096];
                int idle = 0;
                for (;;) {
                    int n = toby_tcp_recv(dconn, buf, sizeof buf);
                    if (n > 0) { fwrite(buf, 1, (size_t)n, fp); idle = 0; continue; }
                    if (n < 0) break;                 /* client closed: done */
                    if (++idle >= 2) break;           /* silent for 20 s */
                }
                fclose(fp);
            }
            toby_tcp_close(dconn);
            toby_tcp_close(data_lid);
            data_lid = -1;
            send_line(cconn, "226 Transfer complete", NULL, -1);
            continue;
        }

        send_line(cconn, "502 Command not implemented", NULL, -1);
    }
    (void)binary;
    if (data_lid >= 0) toby_tcp_close(data_lid);
    toby_tcp_close(cconn);
    return 0;
}

static int run_ftpd(int argc, char **argv) {
    u16 port = 21;
    int sessions = 0;             /* 0 = serve until killed */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-p") && i + 1 < argc) { port = (u16)atoi(argv[++i]); continue; }
        if (!strcmp(a, "-d") && i + 1 < argc) { snprintf(g_root, sizeof g_root, "%s", argv[++i]); continue; }
        if (!strcmp(a, "-1")) { sessions = 1; continue; }
        if (!strcmp(a, "-n") && i + 1 < argc) { sessions = atoi(argv[++i]); continue; }
        if (!strcmp(a, "-v")) { verbose = 1; continue; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            printf("usage: ftpd [-p port] [-d root] [-n sessions] [-v]\n");
            return 0;
        }
    }

    int lid = toby_tcp_listen(htons_(port), 4);
    if (lid < 0) {
        fprintf(stderr, "ftpd: cannot listen on port %u\n", (unsigned)port);
        return 1;
    }
    fprintf(stderr, "ftpd: listening on port %u, serving %s\n",
            (unsigned)port, g_root);

    int served = 0;
    for (;;) {
        int c = toby_tcp_accept(lid);
        if (c < 0) { yield_(); continue; }
        if (verbose) fprintf(stderr, "ftpd: session %d\n", served + 1);
        serve_session(c);
        served++;
        if (sessions && served >= sessions) break;
    }
    toby_tcp_close(lid);
    return 0;
}

/* ====================================================================== */
/*                                 CLIENT                                 */
/* ====================================================================== */

struct ftp_client {
    int conn;
    struct line_io lio;
    int connected;
};

/* Reads one reply, handling the multi-line form (a "250-" first line is
 * continued until a line starting with the same code and a space). */
static int read_reply(struct ftp_client *c, char *out, size_t cap) {
    char line[1024];
    if (lio_getline(&c->lio, line, sizeof line, 2) <= 0) return -1;
    snprintf(out, cap, "%s", line);
    if (verbose) printf("%s\n", line);
    if (strlen(line) >= 4 && line[3] == '-') {
        char code[4];
        memcpy(code, line, 3);
        code[3] = 0;
        for (;;) {
            if (lio_getline(&c->lio, line, sizeof line, 2) <= 0) return -1;
            if (verbose) printf("%s\n", line);
            if (strlen(line) >= 4 && !strncmp(line, code, 3) && line[3] == ' ') {
                snprintf(out, cap, "%s", line);
                break;
            }
        }
    }
    return atoi(out);
}

static int cmd(struct ftp_client *c, const char *s, char *reply, size_t cap) {
    if (verbose) printf("---> %s\n", s);
    if (send_line(c->conn, s, NULL, -1) != 0) return -1;
    return read_reply(c, reply, cap);
}

/* Parses "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)". */
static int parse_pasv(const char *reply, u32 *ip_be, u16 *port) {
    const char *p = strchr(reply, '(');
    if (!p) return 0;
    unsigned v[6];
    int n = 0;
    p++;
    while (n < 6) {
        if (*p < '0' || *p > '9') return 0;
        unsigned x = 0;
        while (*p >= '0' && *p <= '9') x = x * 10 + (unsigned)(*p++ - '0');
        v[n++] = x;
        if (n < 6) { if (*p != ',') return 0; p++; }
    }
    *ip_be = v[0] | (v[1] << 8) | (v[2] << 16) | (v[3] << 24);
    *port = (u16)((v[4] << 8) | v[5]);
    return 1;
}

static int open_data(struct ftp_client *c) {
    char reply[512];
    int code = cmd(c, "PASV", reply, sizeof reply);
    if (code != 227) {
        fprintf(stderr, "ftp: the server refused PASV (%s). This client is "
                        "passive-only.\n", reply);
        return -1;
    }
    u32 ip;
    u16 port;
    if (!parse_pasv(reply, &ip, &port)) {
        fprintf(stderr, "ftp: cannot parse the PASV reply: %s\n", reply);
        return -1;
    }
    int d = toby_tcp_connect(ip, htons_(port), 10000);
    if (d < 0) {
        fprintf(stderr, "ftp: cannot open the data connection to port %u\n",
                (unsigned)port);
        return -1;
    }
    return d;
}

static void drain_to_stdout(int d) {
    char buf[4096];
    int idle = 0;
    for (;;) {
        int n = toby_tcp_recv(d, buf, sizeof buf);
        if (n > 0) { fwrite(buf, 1, (size_t)n, stdout); idle = 0; continue; }
        if (n < 0) break;                     /* server closed the data conn */
        if (++idle >= 2) break;
    }
    fflush(stdout);
}

static int run_ftp(int argc, char **argv) {
    const char *host = NULL;
    u16 port = 21;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-v")) { verbose = 1; continue; }
        if (!strcmp(a, "-n")) continue;                /* no auto-login: default */
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            printf("usage: ftp [-v] [HOST [PORT]]\n"
                   "commands: open user ls dir cd pwd get put delete mkdir "
                   "rmdir size ascii binary quit\n");
            return 0;
        }
        if (a[0] == '-' && a[1]) continue;
        if (!host) host = a;
        else port = (u16)atoi(a);
    }

    struct ftp_client c;
    memset(&c, 0, sizeof c);
    c.conn = -1;

    char reply[1024];

    if (host) {
        u32 ip;
        if (!parse_ipv4(host, &ip)) {
            fprintf(stderr, "ftp: %s: this client takes a dotted-quad address\n", host);
            return 1;
        }
        c.conn = toby_tcp_connect(ip, htons_(port), 10000);
        if (c.conn < 0) {
            fprintf(stderr, "ftp: cannot connect to %s port %u\n", host, (unsigned)port);
            return 1;
        }
        lio_init(&c.lio, c.conn);
        c.connected = 1;
        if (read_reply(&c, reply, sizeof reply) < 0) {
            fprintf(stderr, "ftp: no greeting from the server\n");
            return 1;
        }
        printf("Connected to %s.\n%s\n", host, reply);
    }

    char line[1024];
    int status = 0;
    while (fgets(line, sizeof line, stdin)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;

        char verb[32];
        const char *arg = "";
        size_t i = 0;
        while (i < sizeof verb - 1 && line[i] && line[i] != ' ') { verb[i] = line[i]; i++; }
        verb[i] = 0;
        if (line[i] == ' ') arg = line + i + 1;

        if (!strcmp(verb, "quit") || !strcmp(verb, "bye")) {
            if (c.connected) { cmd(&c, "QUIT", reply, sizeof reply); toby_tcp_close(c.conn); }
            break;
        }
        if (!strcmp(verb, "open")) {
            u32 ip;
            char h[128] = "";
            unsigned pn = 21;
            sscanf(arg, "%127s %u", h, &pn);
            if (!parse_ipv4(h, &ip)) { printf("ftp: need a dotted-quad address\n"); status = 1; continue; }
            c.conn = toby_tcp_connect(ip, htons_((u16)pn), 10000);
            if (c.conn < 0) { printf("ftp: cannot connect to %s\n", h); status = 1; continue; }
            lio_init(&c.lio, c.conn);
            c.connected = 1;
            read_reply(&c, reply, sizeof reply);
            printf("Connected to %s.\n%s\n", h, reply);
            continue;
        }
        if (!c.connected) { printf("Not connected.\n"); status = 1; continue; }

        if (!strcmp(verb, "user")) {
            char s[256];
            snprintf(s, sizeof s, "USER %s", arg[0] ? arg : "anonymous");
            cmd(&c, s, reply, sizeof reply);
            printf("%s\n", reply);
            snprintf(s, sizeof s, "PASS %s", "ftp@tobyos");
            int code = cmd(&c, s, reply, sizeof reply);
            printf("%s\n", reply);
            if (code / 100 != 2) status = 1;
            continue;
        }
        if (!strcmp(verb, "pwd")) { cmd(&c, "PWD", reply, sizeof reply); printf("%s\n", reply); continue; }
        if (!strcmp(verb, "cd")) {
            char s[600];
            snprintf(s, sizeof s, "CWD %s", arg);
            int code = cmd(&c, s, reply, sizeof reply);
            printf("%s\n", reply);
            if (code / 100 != 2) status = 1;
            continue;
        }
        if (!strcmp(verb, "binary")) { cmd(&c, "TYPE I", reply, sizeof reply); printf("%s\n", reply); continue; }
        if (!strcmp(verb, "ascii"))  { cmd(&c, "TYPE A", reply, sizeof reply); printf("%s\n", reply); continue; }
        if (!strcmp(verb, "size")) {
            char s[600];
            snprintf(s, sizeof s, "SIZE %s", arg);
            cmd(&c, s, reply, sizeof reply);
            printf("%s\n", reply);
            continue;
        }
        if (!strcmp(verb, "delete")) {
            char s[600];
            snprintf(s, sizeof s, "DELE %s", arg);
            int code = cmd(&c, s, reply, sizeof reply);
            printf("%s\n", reply);
            if (code / 100 != 2) status = 1;
            continue;
        }
        if (!strcmp(verb, "mkdir")) {
            char s[600];
            snprintf(s, sizeof s, "MKD %s", arg);
            cmd(&c, s, reply, sizeof reply);
            printf("%s\n", reply);
            continue;
        }
        if (!strcmp(verb, "rmdir")) {
            char s[600];
            snprintf(s, sizeof s, "RMD %s", arg);
            cmd(&c, s, reply, sizeof reply);
            printf("%s\n", reply);
            continue;
        }
        if (!strcmp(verb, "ls") || !strcmp(verb, "dir") || !strcmp(verb, "nlist")) {
            int d = open_data(&c);
            if (d < 0) { status = 1; continue; }
            char s[600];
            snprintf(s, sizeof s, "%s%s%s",
                     !strcmp(verb, "nlist") ? "NLST" : "LIST",
                     arg[0] ? " " : "", arg);
            if (verbose) printf("---> %s\n", s);
            send_line(c.conn, s, NULL, -1);
            read_reply(&c, reply, sizeof reply);      /* 150 */
            drain_to_stdout(d);
            toby_tcp_close(d);
            read_reply(&c, reply, sizeof reply);      /* 226 */
            continue;
        }
        if (!strcmp(verb, "get")) {
            char remote[256] = "", local[256] = "";
            sscanf(arg, "%255s %255s", remote, local);
            if (!remote[0]) { printf("usage: get REMOTE [LOCAL]\n"); status = 1; continue; }
            if (!local[0]) snprintf(local, sizeof local, "%s", remote);
            int d = open_data(&c);
            if (d < 0) { status = 1; continue; }
            char s[600];
            snprintf(s, sizeof s, "RETR %s", remote);
            if (verbose) printf("---> %s\n", s);
            send_line(c.conn, s, NULL, -1);
            int code = read_reply(&c, reply, sizeof reply);
            if (code / 100 != 1) {
                printf("%s\n", reply);
                toby_tcp_close(d);
                status = 1;
                continue;
            }
            FILE *fp = fopen(local, "wb");
            if (!fp) {
                printf("ftp: cannot write %s\n", local);
                toby_tcp_close(d);
                status = 1;
                continue;
            }
            long total = 0;
            char buf[4096];
            int idle = 0;
            for (;;) {
                int n = toby_tcp_recv(d, buf, sizeof buf);
                if (n > 0) { fwrite(buf, 1, (size_t)n, fp); total += n; idle = 0; continue; }
                if (n < 0) break;             /* server closed: transfer done */
                if (++idle >= 2) break;
            }
            fclose(fp);
            toby_tcp_close(d);
            read_reply(&c, reply, sizeof reply);
            printf("%ld bytes received for %s\n", total, remote);
            continue;
        }
        if (!strcmp(verb, "put")) {
            char local[256] = "", remote[256] = "";
            sscanf(arg, "%255s %255s", local, remote);
            if (!local[0]) { printf("usage: put LOCAL [REMOTE]\n"); status = 1; continue; }
            if (!remote[0]) snprintf(remote, sizeof remote, "%s", local);
            FILE *fp = fopen(local, "rb");
            if (!fp) { printf("ftp: cannot read %s\n", local); status = 1; continue; }
            int d = open_data(&c);
            if (d < 0) { fclose(fp); status = 1; continue; }
            char s[600];
            snprintf(s, sizeof s, "STOR %s", remote);
            if (verbose) printf("---> %s\n", s);
            send_line(c.conn, s, NULL, -1);
            int code = read_reply(&c, reply, sizeof reply);
            if (code / 100 != 1) {
                printf("%s\n", reply);
                fclose(fp);
                toby_tcp_close(d);
                status = 1;
                continue;
            }
            long total = 0;
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, fp)) > 0) {
                if (send_all(d, buf, n) != 0) break;
                total += (long)n;
            }
            fclose(fp);
            toby_tcp_close(d);
            read_reply(&c, reply, sizeof reply);
            printf("%ld bytes sent for %s\n", total, remote);
            continue;
        }
        printf("?Invalid command: %s\n", verb);
        status = 1;
    }
    return status;
}

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "ftp";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    prog = base;
    (void)prog;
    if (!strcmp(base, "ftpd")) return run_ftpd(argc, argv);
    return run_ftp(argc, argv);
}
