/* user_ssh -- the SSH client tobyOS did not have.
 *
 *   ssh ssh-keygen ssh-add scp sftp
 *
 * tobyOS has had an SSH SERVER for a while; there was no way to connect OUT.
 * One binary dispatching on argv[0], the same shape as user_coreutil and
 * user_admin.
 *
 * WHAT IT SPEAKS. Exactly what src/ssh.c speaks, deliberately:
 *
 *   kex          curve25519-sha256
 *   host key     ssh-ed25519
 *   cipher       chacha20-poly1305@openssh.com  (implicit MAC)
 *   compression  none
 *
 * That is a real OpenSSH-interoperable subset, not a private protocol -- the
 * same four choices a modern OpenSSH offers first. Matching the server's set
 * also means the client can be tested against our own sshd over loopback,
 * with no second machine in the loop.
 *
 * The crypto is monocypher, the same implementation the server uses, now
 * also linked into libtoby. Sharing it means client and server cannot
 * disagree about a primitive -- a disagreement there shows up as an
 * unexplained handshake failure with nothing in the logs to say why.
 *
 * HOST KEY VERIFICATION IS REAL. The server's Ed25519 signature over the
 * exchange hash is checked before anything else happens, and the key is
 * pinned in known_hosts. A client that skipped either would look identical
 * in every test that passes and offer no security at all.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <termios.h>
#include <sys/stat.h>

#include <toby/net.h>
#include <toby/sha2.h>
#include <monocypher.h>
#include <monocypher-ed25519.h>

#define SSH_BANNER      "SSH-2.0-tobyOS_1.0"
#define KNOWN_HOSTS     "/data/ssh/known_hosts"
#define KEY_DIR         "/data/ssh"
#define CLIENT_KEY      "/data/ssh/id_ed25519"

#define MSG_DISCONNECT      1
#define MSG_IGNORE          2
#define MSG_UNIMPLEMENTED   3
#define MSG_DEBUG           4
#define MSG_SERVICE_REQUEST 5
#define MSG_SERVICE_ACCEPT  6
#define MSG_KEXINIT        20
#define MSG_NEWKEYS        21
#define MSG_KEX_ECDH_INIT  30
#define MSG_KEX_ECDH_REPLY 31
#define MSG_USERAUTH_REQUEST 50
#define MSG_USERAUTH_FAILURE 51
#define MSG_USERAUTH_SUCCESS 52
#define MSG_USERAUTH_BANNER  53
#define MSG_GLOBAL_REQUEST   80
#define MSG_CHANNEL_OPEN     90
#define MSG_CHANNEL_OPEN_CONFIRM 91
#define MSG_CHANNEL_OPEN_FAILURE 92
#define MSG_CHANNEL_WINDOW_ADJUST 93
#define MSG_CHANNEL_DATA     94
#define MSG_CHANNEL_EXT_DATA 95
#define MSG_CHANNEL_EOF      96
#define MSG_CHANNEL_CLOSE    97
#define MSG_CHANNEL_REQUEST  98
#define MSG_CHANNEL_SUCCESS  99
#define MSG_CHANNEL_FAILURE 100

#define PKT_MAX  32768
#define TAG_LEN  16

static const char *g_prog = "ssh";
static int  g_conn = -1;
static int  g_verbose;

/* ---- byte helpers ------------------------------------------------------ */
static void put_u32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v;
}
static uint32_t get_u32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static void put_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (56 - 8*i));
}

struct buf { uint8_t *p; size_t n, cap; };
static void b_init(struct buf *b, uint8_t *store, size_t cap) { b->p=store; b->n=0; b->cap=cap; }
static void b_u8 (struct buf *b, uint8_t v) { if (b->n < b->cap) b->p[b->n++] = v; }
static void b_raw(struct buf *b, const void *d, size_t n) {
    if (b->n + n <= b->cap) { memcpy(b->p + b->n, d, n); b->n += n; }
}
static void b_u32(struct buf *b, uint32_t v) { uint8_t t[4]; put_u32(t,v); b_raw(b,t,4); }
static void b_str(struct buf *b, const void *d, size_t n) { b_u32(b,(uint32_t)n); b_raw(b,d,n); }
static void b_cstr(struct buf *b, const char *s) { b_str(b, s, strlen(s)); }

/* An SSH mpint is signed: a leading bit set means a 0x00 must be prefixed,
 * or the peer reads the shared secret as negative and every derived key is
 * different -- which surfaces only as a MAC failure on the first packet. */
static void b_mpint(struct buf *b, const uint8_t *v, size_t n) {
    size_t i = 0;
    while (i < n && v[i] == 0) i++;            /* strip leading zeros */
    if (i == n) { b_u32(b, 0); return; }
    int pad = (v[i] & 0x80) ? 1 : 0;
    b_u32(b, (uint32_t)(n - i + pad));
    if (pad) b_u8(b, 0);
    b_raw(b, v + i, n - i);
}

static int die(const char *msg, const char *arg) {
    if (arg) fprintf(stderr, "%s: %s: %s\n", g_prog, arg, msg);
    else     fprintf(stderr, "%s: %s\n", g_prog, msg);
    if (g_conn >= 0) toby_tcp_close(g_conn);
    return 255;   /* what OpenSSH returns for its own errors */
}

/* ---- randomness -------------------------------------------------------- */
/* /dev/urandom is the kernel CSPRNG. There is no fallback on purpose: a
 * predictable ephemeral key silently destroys the security of the whole
 * session, so if entropy is unavailable the connection must not proceed. */
static int fill_random(void *buf, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < n) {
        long r = read(fd, (char *)buf + got, n - got);
        if (r <= 0) { close(fd); return -1; }
        got += (size_t)r;
    }
    close(fd);
    return 0;
}

/* ---- transport --------------------------------------------------------- */
struct cipher { uint8_t key[64]; int active; };
static struct cipher g_out, g_in;
static uint32_t g_send_seq, g_recv_seq;
static uint8_t  g_session_id[32];
static int      g_have_session;

static int io_send(const void *p, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        int w = toby_tcp_send(g_conn, (const char *)p + sent, n - sent);
        if (w <= 0) return -1;
        sent += (size_t)w;
    }
    return 0;
}

static int io_recv(void *p, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = toby_tcp_recv(g_conn, (char *)p + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static void cc_len(const struct cipher *c, uint32_t seq, uint8_t out[4], const uint8_t in[4]) {
    uint8_t nonce[8];
    put_u64(nonce, seq);
    crypto_chacha20_djb(out, in, 4, c->key + 32, nonce, 0);
}
static void cc_payload(const struct cipher *c, uint32_t seq, uint8_t *out,
                       const uint8_t *in, size_t n) {
    uint8_t nonce[8];
    put_u64(nonce, seq);
    crypto_chacha20_djb(out, in, n, c->key, nonce, 1);
}
static void cc_tag(const struct cipher *c, uint32_t seq, const uint8_t *ct,
                   size_t n, uint8_t tag[16]) {
    uint8_t nonce[8], poly[64];
    put_u64(nonce, seq);
    crypto_chacha20_djb(poly, 0, sizeof poly, c->key, nonce, 0);
    crypto_poly1305(tag, ct, n, poly);
    crypto_wipe(poly, sizeof poly);
}

static int send_packet(const uint8_t *payload, size_t len) {
    /* Padding: at least 4 bytes, total (len+pad+1) a multiple of 8. With
     * chacha20-poly1305 the 4-byte length is NOT counted in that alignment,
     * which is the detail that makes this cipher's framing different. */
    size_t block = 8;
    size_t pad = block - ((1 + len) % block);
    if (pad < 4) pad += block;
    size_t plen = len + pad + 1;

    static uint8_t pkt[PKT_MAX + 64];
    if (plen + 4 + TAG_LEN > sizeof pkt) return -1;
    put_u32(pkt, (uint32_t)plen);
    pkt[4] = (uint8_t)pad;
    memcpy(pkt + 5, payload, len);
    if (g_out.active) {
        if (fill_random(pkt + 5 + len, pad) != 0) return -1;
    } else {
        memset(pkt + 5 + len, 0, pad);
    }

    if (!g_out.active) {
        int rc = io_send(pkt, plen + 4);
        g_send_seq++;
        return rc;
    }
    uint8_t enc_len[4];
    cc_len(&g_out, g_send_seq, enc_len, pkt);
    static uint8_t out[PKT_MAX + 64];
    memcpy(out, enc_len, 4);
    cc_payload(&g_out, g_send_seq, out + 4, pkt + 4, plen);
    uint8_t tag[16];
    cc_tag(&g_out, g_send_seq, out, plen + 4, tag);
    memcpy(out + 4 + plen, tag, TAG_LEN);
    int rc = io_send(out, plen + 4 + TAG_LEN);
    g_send_seq++;
    return rc;
}

/* Returns payload length, fills *out. <0 on error. */
static long recv_packet(uint8_t *out, size_t cap) {
    uint8_t hdr[4];
    if (io_recv(hdr, 4) != 0) return -1;

    uint32_t plen;
    if (g_in.active) {
        uint8_t dec[4];
        cc_len(&g_in, g_recv_seq, dec, hdr);
        plen = get_u32(dec);
        if (plen < 8 || plen > PKT_MAX) return -1;
        static uint8_t ct[PKT_MAX + 64];
        if (io_recv(ct, plen + TAG_LEN) != 0) return -1;
        uint8_t full[PKT_MAX + 64];
        memcpy(full, hdr, 4);
        memcpy(full + 4, ct, plen);
        uint8_t want[16];
        cc_tag(&g_in, g_recv_seq, full, plen + 4, want);
        if (crypto_verify16(want, ct + plen) != 0) return -1;   /* constant time */
        static uint8_t pt[PKT_MAX + 64];
        cc_payload(&g_in, g_recv_seq, pt, ct, plen);
        uint8_t pad = pt[0];
        if (pad + 1u > plen) return -1;
        size_t n = plen - pad - 1;
        if (n > cap) return -1;
        memcpy(out, pt + 1, n);
        g_recv_seq++;
        return (long)n;
    }
    plen = get_u32(hdr);
    if (plen < 8 || plen > PKT_MAX) return -1;
    static uint8_t body[PKT_MAX + 64];
    if (io_recv(body, plen) != 0) return -1;
    uint8_t pad = body[0];
    if (pad + 1u > plen) return -1;
    size_t n = plen - pad - 1;
    if (n > cap) return -1;
    memcpy(out, body + 1, n);
    g_recv_seq++;
    return (long)n;
}

/* ---- known_hosts ------------------------------------------------------- */
static void b64(char *out, const uint8_t *in, size_t n) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i+1 < n) v |= (uint32_t)in[i+1] << 8;
        if (i+2 < n) v |= in[i+2];
        out[o++] = T[(v>>18)&63];
        out[o++] = T[(v>>12)&63];
        out[o++] = (i+1 < n) ? T[(v>>6)&63] : '=';
        out[o++] = (i+2 < n) ? T[v&63]      : '=';
    }
    out[o] = 0;
}

/* 0 = known and matching, 1 = new (recorded), -1 = MISMATCH. */
static int known_hosts_check(const char *host, const uint8_t *keyblob, size_t n) {
    char enc[1024];
    if (n > 700) return -1;
    b64(enc, keyblob, n);

    char line[1200];
    snprintf(line, sizeof line, "%s ssh-ed25519 %s", host, enc);

    FILE *f = fopen(KNOWN_HOSTS, "r");
    if (f) {
        char cur[1200];
        size_t hl = strlen(host);
        while (fgets(cur, sizeof cur, f)) {
            size_t cl = strlen(cur);
            while (cl && (cur[cl-1]=='\n' || cur[cl-1]=='\r')) cur[--cl] = 0;
            if (strncmp(cur, host, hl) || cur[hl] != ' ') continue;
            fclose(f);
            /* Same host, different key: STOP. This is the one case where
             * carrying on would be actively harmful, so it is a refusal
             * rather than a warning. */
            return strcmp(cur, line) == 0 ? 0 : -1;
        }
        fclose(f);
    }
    (void)mkdir(KEY_DIR, 0700);
    FILE *a = fopen(KNOWN_HOSTS, "a");
    if (!a) {
        fprintf(stderr, "%s: warning: cannot record the host key in %s: %s\n",
                g_prog, KNOWN_HOSTS, strerror(errno));
        return 1;
    }
    fprintf(a, "%s\n", line);
    fclose(a);
    return 1;
}

/* ---- the handshake ----------------------------------------------------- */
struct kexinit_blob { uint8_t b[2048]; size_t n; };

static int build_kexinit(struct kexinit_blob *k) {
    struct buf b;
    b_init(&b, k->b, sizeof k->b);
    b_u8(&b, MSG_KEXINIT);
    uint8_t cookie[16];
    if (fill_random(cookie, sizeof cookie) != 0) return -1;
    b_raw(&b, cookie, 16);
    b_cstr(&b, "curve25519-sha256");
    b_cstr(&b, "ssh-ed25519");
    b_cstr(&b, "chacha20-poly1305@openssh.com");   /* c2s */
    b_cstr(&b, "chacha20-poly1305@openssh.com");   /* s2c */
    b_cstr(&b, "none");                            /* mac c2s (implicit) */
    b_cstr(&b, "none");                            /* mac s2c */
    b_cstr(&b, "none");                            /* comp c2s */
    b_cstr(&b, "none");                            /* comp s2c */
    b_cstr(&b, "");                                /* lang c2s */
    b_cstr(&b, "");                                /* lang s2c */
    b_u8(&b, 0);                                   /* first_kex_packet_follows */
    b_u32(&b, 0);                                  /* reserved */
    k->n = b.n;
    return 0;
}

static int do_handshake(const char *host, const char *user, int *authed) {
    char sbanner[256];
    /* --- version exchange --- */
    if (io_send(SSH_BANNER "\r\n", strlen(SSH_BANNER) + 2) != 0)
        return die("could not send the version string", 0);

    size_t bn = 0;
    for (;;) {
        char c;
        if (toby_tcp_recv(g_conn, &c, 1) <= 0) return die("no banner from the server", 0);
        if (c == '\n') break;
        if (bn + 1 < sizeof sbanner) sbanner[bn++] = c;
    }
    while (bn && (sbanner[bn-1] == '\r')) bn--;
    sbanner[bn] = 0;
    if (strncmp(sbanner, "SSH-2.0", 7))
        return die("not an SSH-2.0 server", sbanner);
    if (g_verbose) fprintf(stderr, "%s: server is %s\n", g_prog, sbanner);

    /* --- KEXINIT both ways --- */
    struct kexinit_blob ci, si;
    if (build_kexinit(&ci) != 0) return die("no entropy for the kex cookie", 0);
    if (send_packet(ci.b, ci.n) != 0) return die("could not send KEXINIT", 0);

    long n = recv_packet(si.b, sizeof si.b);
    if (n <= 0 || si.b[0] != MSG_KEXINIT) return die("expected KEXINIT", 0);
    si.n = (size_t)n;

    /* --- curve25519 --- */
    uint8_t esk[32], epk[32];
    if (fill_random(esk, sizeof esk) != 0)
        return die("no entropy for the ephemeral key -- refusing to connect", 0);
    crypto_x25519_public_key(epk, esk);

    uint8_t out[64];
    struct buf b;
    b_init(&b, out, sizeof out);
    b_u8(&b, MSG_KEX_ECDH_INIT);
    b_str(&b, epk, 32);
    if (send_packet(out, b.n) != 0) return die("could not send KEX_ECDH_INIT", 0);

    static uint8_t rep[PKT_MAX];
    n = recv_packet(rep, sizeof rep);
    if (n <= 0 || rep[0] != MSG_KEX_ECDH_REPLY) return die("expected KEX_ECDH_REPLY", 0);

    size_t o = 1;
    uint32_t ks_len = get_u32(rep + o); o += 4;
    const uint8_t *ks = rep + o; o += ks_len;
    uint32_t qs_len = get_u32(rep + o); o += 4;
    const uint8_t *qs = rep + o; o += qs_len;
    uint32_t sig_len = get_u32(rep + o); o += 4;
    const uint8_t *sig = rep + o;
    if (qs_len != 32 || (size_t)o + sig_len > (size_t)n)
        return die("malformed KEX_ECDH_REPLY", 0);

    uint8_t shared[32];
    crypto_x25519(shared, esk, qs);
    crypto_wipe(esk, sizeof esk);
    /* An all-zero shared secret means a low-order public key: the peer can
     * force a known secret. monocypher reports it rather than hiding it. */
    uint8_t zero[32];
    memset(zero, 0, sizeof zero);
    if (crypto_verify32(shared, zero) == 0)
        return die("server offered a degenerate curve25519 key", 0);

    /* --- exchange hash --- */
    toby_sha256_ctx h;
    toby_sha256_init(&h, 0);
    uint8_t t4[4];
    #define H_STR(p, l) do { put_u32(t4, (uint32_t)(l)); \
                             toby_sha256_update(&h, t4, 4); \
                             toby_sha256_update(&h, (p), (l)); } while (0)
    H_STR(SSH_BANNER, strlen(SSH_BANNER));
    H_STR(sbanner, strlen(sbanner));
    H_STR(ci.b, ci.n);
    H_STR(si.b, si.n);
    H_STR(ks, ks_len);
    H_STR(epk, 32);
    H_STR(qs, 32);
    {
        uint8_t mp[64];
        struct buf m;
        b_init(&m, mp, sizeof mp);
        b_mpint(&m, shared, 32);
        toby_sha256_update(&h, mp, m.n);
    }
    uint8_t H[32];
    toby_sha256_final(&h, H);

    /* --- verify the host key signature BEFORE trusting anything --- */
    /* ks is "ssh-ed25519" || string(32-byte key); sig is
     * "ssh-ed25519" || string(64-byte signature). */
    if (ks_len < 4 + 11 + 4 + 32) return die("short host key blob", 0);
    const uint8_t *hostkey = ks + 4 + 11 + 4;
    if (sig_len < 4 + 11 + 4 + 64) return die("short signature blob", 0);
    const uint8_t *sigraw = sig + 4 + 11 + 4;
    if (crypto_ed25519_check(sigraw, hostkey, H, 32) != 0)
        return die("HOST KEY SIGNATURE IS INVALID -- refusing to continue", host);

    int kh = known_hosts_check(host, ks, ks_len);
    if (kh < 0) {
        fprintf(stderr,
"%s: @@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
"%s: WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!\n"
"%s: The host key for %s does not match the one in %s.\n"
"%s: Someone could be eavesdropping right now. Refusing to connect.\n",
            g_prog, g_prog, g_prog, host, KNOWN_HOSTS, g_prog);
        return 255;
    }
    if (kh == 1) {
        uint8_t fp[32];
        char fpb[64];
        toby_sha256(ks, ks_len, fp);
        b64(fpb, fp, 32);
        fprintf(stderr, "%s: the authenticity of host '%s' was not established;"
                        " its ed25519 key SHA256:%s is now recorded in %s\n",
                g_prog, host, fpb, KNOWN_HOSTS);
    }

    if (!g_have_session) { memcpy(g_session_id, H, 32); g_have_session = 1; }

    /* --- NEWKEYS --- */
    uint8_t nk = MSG_NEWKEYS;
    if (send_packet(&nk, 1) != 0) return die("could not send NEWKEYS", 0);
    n = recv_packet(rep, sizeof rep);
    if (n <= 0 || rep[0] != MSG_NEWKEYS) return die("expected NEWKEYS", 0);

    /* --- derive keys --- */
    uint8_t kmp[64];
    struct buf m;
    b_init(&m, kmp, sizeof kmp);
    b_mpint(&m, shared, 32);

    /* RFC 4253 6.4: HASH(K || H || letter || session_id), extended by
     * HASH(K || H || everything produced so far) until long enough. */
    for (int which = 0; which < 2; which++) {
        char letter = which ? 'D' : 'C';       /* C = c2s, D = s2c */
        uint8_t *dst = which ? g_in.key : g_out.key;
        size_t need = 64, done = 0;
        uint8_t dg[32];
        toby_sha256_ctx s;
        toby_sha256_init(&s, 0);
        toby_sha256_update(&s, kmp, m.n);
        toby_sha256_update(&s, H, 32);
        toby_sha256_update(&s, &letter, 1);
        toby_sha256_update(&s, g_session_id, 32);
        toby_sha256_final(&s, dg);
        while (done < need) {
            size_t take = need - done; if (take > 32) take = 32;
            memcpy(dst + done, dg, take);
            done += take;
            if (done >= need) break;
            toby_sha256_init(&s, 0);
            toby_sha256_update(&s, kmp, m.n);
            toby_sha256_update(&s, H, 32);
            toby_sha256_update(&s, dst, done);
            toby_sha256_final(&s, dg);
        }
    }
    crypto_wipe(shared, sizeof shared);
    g_out.active = g_in.active = 1;

    /* --- service request + auth --- */
    b_init(&b, out, sizeof out);
    b_u8(&b, MSG_SERVICE_REQUEST);
    b_cstr(&b, "ssh-userauth");
    if (send_packet(out, b.n) != 0) return die("could not request ssh-userauth", 0);
    n = recv_packet(rep, sizeof rep);
    if (n <= 0 || rep[0] != MSG_SERVICE_ACCEPT) return die("ssh-userauth refused", 0);

    static uint8_t areq[512];
    b_init(&b, areq, sizeof areq);
    b_u8(&b, MSG_USERAUTH_REQUEST);
    b_cstr(&b, user);
    b_cstr(&b, "ssh-connection");
    b_cstr(&b, "none");
    if (send_packet(areq, b.n) != 0) return die("could not send the auth request", 0);

    for (;;) {
        n = recv_packet(rep, sizeof rep);
        if (n <= 0) return die("connection closed during authentication", 0);
        if (rep[0] == MSG_USERAUTH_BANNER) continue;
        if (rep[0] == MSG_IGNORE || rep[0] == MSG_DEBUG) continue;
        if (rep[0] == MSG_USERAUTH_SUCCESS) { *authed = 1; break; }
        if (rep[0] == MSG_USERAUTH_FAILURE) {
            /* The server names the methods it will take; say so rather than
             * a bare "permission denied", which hides whether the account
             * or the METHOD was the problem. */
            uint32_t l = get_u32(rep + 1);
            char methods[256];
            size_t k = l < sizeof methods - 1 ? l : sizeof methods - 1;
            memcpy(methods, rep + 5, k);
            methods[k] = 0;
            fprintf(stderr, "%s: authentication failed for '%s'; the server "
                            "accepts: %s\n", g_prog, user, methods);
            return 255;
        }
        return die("unexpected message during authentication", 0);
    }
    return 0;
}

/* ---- session ----------------------------------------------------------- */
static int run_session(const char *command) {
    static uint8_t out[PKT_MAX], rep[PKT_MAX];
    struct buf b;

    b_init(&b, out, sizeof out);
    b_u8(&b, MSG_CHANNEL_OPEN);
    b_cstr(&b, "session");
    b_u32(&b, 0);            /* our channel id */
    b_u32(&b, 1048576);      /* window */
    b_u32(&b, 32768);        /* max packet */
    if (send_packet(out, b.n) != 0) return die("could not open a channel", 0);

    uint32_t remote_chan = 0;
    for (;;) {
        long n = recv_packet(rep, sizeof rep);
        if (n <= 0) return die("connection closed opening the channel", 0);
        if (rep[0] == MSG_CHANNEL_OPEN_CONFIRM) { remote_chan = get_u32(rep + 5); break; }
        if (rep[0] == MSG_CHANNEL_OPEN_FAILURE) return die("channel open refused", 0);
        if (rep[0] == MSG_GLOBAL_REQUEST || rep[0] == MSG_IGNORE ||
            rep[0] == MSG_DEBUG) continue;
        return die("unexpected reply to channel open", 0);
    }

    b_init(&b, out, sizeof out);
    b_u8(&b, MSG_CHANNEL_REQUEST);
    b_u32(&b, remote_chan);
    if (command) {
        b_cstr(&b, "exec");
        b_u8(&b, 1);
        b_cstr(&b, command);
    } else {
        b_cstr(&b, "shell");
        b_u8(&b, 1);
    }
    if (send_packet(out, b.n) != 0) return die("could not send the channel request", 0);

    int exit_status = 0;
    for (;;) {
        long n = recv_packet(rep, sizeof rep);
        if (n <= 0) break;
        switch (rep[0]) {
        case MSG_CHANNEL_DATA: {
            uint32_t l = get_u32(rep + 5);
            if (5 + 4 + l <= (uint32_t)n) {
                ssize_t w = write(1, rep + 9, l);
                (void)w;
            }
            break;
        }
        case MSG_CHANNEL_EXT_DATA: {
            uint32_t l = get_u32(rep + 9);
            if (9 + 4 + l <= (uint32_t)n) {
                ssize_t w = write(2, rep + 13, l);
                (void)w;
            }
            break;
        }
        case MSG_CHANNEL_REQUEST: {
            /* exit-status carries the remote command's status, which is what
             * `ssh host cmd` must exit with -- scripts branch on it. */
            uint32_t l = get_u32(rep + 5);
            if (l == 11 && !memcmp(rep + 9, "exit-status", 11))
                exit_status = (int)get_u32(rep + 9 + 11 + 1);
            break;
        }
        case MSG_CHANNEL_EOF:
        case MSG_CHANNEL_CLOSE:
            goto done;
        case MSG_CHANNEL_SUCCESS:
        case MSG_CHANNEL_FAILURE:
        case MSG_CHANNEL_WINDOW_ADJUST:
        case MSG_IGNORE:
        case MSG_DEBUG:
            break;
        case MSG_DISCONNECT:
            goto done;
        default:
            break;
        }
    }
done:
    toby_tcp_close(g_conn);
    g_conn = -1;
    return exit_status;
}

/* ---- ssh --------------------------------------------------------------- */
static int cmd_ssh(int argc, char **argv) {
    const char *host = 0, *user = 0, *command = 0;
    int port = 22;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "-p") && i + 1 < argc) { port = atoi(argv[++i]); continue; }
        if (!strcmp(a, "-l") && i + 1 < argc) { user = argv[++i]; continue; }
        if (!strcmp(a, "-v")) { g_verbose = 1; continue; }
        if (!strcmp(a, "-q")) continue;
        if (!strcmp(a, "-o") && i + 1 < argc) { i++; continue; }   /* accepted, ignored */
        if (!strcmp(a, "-T") || !strcmp(a, "-t") || !strcmp(a, "-n")) continue;
        fprintf(stderr, "%s: unknown option %s\n", g_prog, a);
        return 255;
    }
    if (i >= argc) {
        fprintf(stderr, "usage: ssh [-p port] [-l user] [-v] [user@]host [command]\n");
        return 255;
    }
    char hostbuf[256];
    snprintf(hostbuf, sizeof hostbuf, "%s", argv[i++]);
    char *at = strchr(hostbuf, '@');
    if (at) { *at = 0; user = hostbuf; host = at + 1; }
    else    { host = hostbuf; }
    if (!user) {
        const char *u = getenv("USER");
        user = (u && *u) ? u : "root";
    }
    if (i < argc) {
        static char cmdbuf[4096];      /* the server's exec cap, and ABI_ARG_MAX */
        size_t o = 0;
        for (; i < argc; i++) {
            int k = snprintf(cmdbuf + o, sizeof cmdbuf - o, "%s%s",
                             o ? " " : "", argv[i]);
            if (k < 0) break;
            o += (size_t)k;
            if (o >= sizeof cmdbuf) break;
        }
        command = cmdbuf;
    }

    g_conn = toby_tcp_connect_host(host, (uint16_t)port, 10000);
    if (g_conn < 0) {
        fprintf(stderr, "%s: connect to %s port %d failed\n", g_prog, host, port);
        return 255;
    }
    int authed = 0;
    int rc = do_handshake(host, user, &authed);
    if (rc != 0) return rc;
    if (!authed) return die("not authenticated", 0);
    return run_session(command);
}

/* ---- ssh-keygen -------------------------------------------------------- */
static int cmd_keygen(int argc, char **argv) {
    const char *out = CLIENT_KEY;
    int show_fp = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) { out = argv[++i]; continue; }
        if (!strcmp(argv[i], "-l")) { show_fp = 1; continue; }
        if (!strcmp(argv[i], "-t") && i + 1 < argc) {
            const char *t = argv[++i];
            if (strcmp(t, "ed25519")) {
                fprintf(stderr, "ssh-keygen: only ed25519 keys are supported "
                                "(asked for '%s')\n", t);
                return 1;
            }
            continue;
        }
        if (!strcmp(argv[i], "-N") && i + 1 < argc) {
            if (argv[++i][0]) {
                /* A passphrase implies an encrypted private key format this
                 * does not implement; writing it unencrypted while accepting
                 * -N would be a lie about how the key is stored. */
                fprintf(stderr, "ssh-keygen: passphrase-protected keys are not "
                                "supported; the key would be stored in the "
                                "clear, so refusing\n");
                return 1;
            }
            continue;
        }
        if (!strcmp(argv[i], "-C") && i + 1 < argc) { i++; continue; }
        if (!strcmp(argv[i], "-q")) continue;
    }

    char pub[512];
    snprintf(pub, sizeof pub, "%s.pub", out);

    if (show_fp) {
        FILE *f = fopen(pub, "r");
        if (!f) { fprintf(stderr, "ssh-keygen: %s: %s\n", pub, strerror(errno)); return 1; }
        char line[1024];
        if (!fgets(line, sizeof line, f)) { fclose(f); return 1; }
        fclose(f);
        printf("256 %s (ED25519)\n", line);
        return 0;
    }

    uint8_t seed[32], sk[64], pk[32];
    if (fill_random(seed, sizeof seed) != 0)
        return (fprintf(stderr, "ssh-keygen: no entropy available\n"), 1);
    crypto_ed25519_key_pair(sk, pk, seed);

    (void)mkdir(KEY_DIR, 0700);
    FILE *f = fopen(out, "w");
    if (!f) { fprintf(stderr, "ssh-keygen: %s: %s\n", out, strerror(errno)); return 1; }
    /* Raw seed, not OpenSSH's PEM container: this client does not do
     * public-key auth yet, so the file exists to be a stable identity, and
     * pretending to the OpenSSH format would invite feeding it to OpenSSH. */
    fprintf(f, "tobyos-ed25519-seed ");
    for (int i = 0; i < 32; i++) fprintf(f, "%02x", seed[i]);
    fprintf(f, "\n");
    fclose(f);
    (void)chmod(out, 0600);

    uint8_t blob[64];
    struct buf b;
    b_init(&b, blob, sizeof blob);
    b_cstr(&b, "ssh-ed25519");
    b_str(&b, pk, 32);
    char enc[256];
    b64(enc, blob, b.n);
    FILE *p = fopen(pub, "w");
    if (p) { fprintf(p, "ssh-ed25519 %s tobyos\n", enc); fclose(p); }

    uint8_t fp[32];
    char fpb[64];
    toby_sha256(blob, b.n, fp);
    b64(fpb, fp, 32);
    printf("Your identification has been saved in %s\n", out);
    printf("Your public key has been saved in %s\n", pub);
    printf("The key fingerprint is:\nSHA256:%s\n", fpb);
    crypto_wipe(sk, sizeof sk);
    crypto_wipe(seed, sizeof seed);
    return 0;
}

/* ---- ssh-add / scp / sftp ---------------------------------------------- */
static int cmd_sshadd(int argc, char **argv) {
    (void)argc; (void)argv;
    /* Exactly what OpenSSH's ssh-add says with no agent running, because
     * that is precisely the situation: there is no agent on this system. */
    fprintf(stderr, "Could not open a connection to your authentication agent.\n");
    return 2;
}

static int cmd_scp(int argc, char **argv) {
    (void)argc; (void)argv;
    /* scp's remote half runs `scp -t`/`scp -f` ON THE SERVER over an exec
     * channel. The client transport below is ready for it, but the protocol
     * itself is a separate piece of work and shipping a half of it would
     * corrupt files rather than fail. */
    fprintf(stderr, "scp: not implemented yet. The SSH transport it needs is "
                    "working -- use `ssh host 'cat > file'` to move a file "
                    "meanwhile.\n");
    return 1;
}

static int cmd_sftp(int argc, char **argv) {
    (void)argc; (void)argv;
    /* sftp needs a subsystem request AND an sftp-server on the far side.
     * tobyOS's sshd has no subsystem support, so there is nothing to talk
     * to even with a perfect client. */
    fprintf(stderr, "sftp: not available. It needs an sftp-server subsystem "
                    "on the remote host, and tobyOS's sshd does not implement "
                    "subsystems.\n");
    return 1;
}

/* ---- dispatch ---------------------------------------------------------- */
struct applet { const char *name; int (*fn)(int, char **); };
static const struct applet g_applets[] = {
    { "ssh",        cmd_ssh },
    { "ssh-keygen", cmd_keygen },
    { "ssh-add",    cmd_sshadd },
    { "scp",        cmd_scp },
    { "sftp",       cmd_sftp },
    { 0, 0 }
};

int main(int argc, char **argv) {
    const char *base = argv[0] ? argv[0] : "ssh";
    for (const char *p = base; *p; p++) if (*p == '/') base = p + 1;
    g_prog = base;
    for (int i = 0; g_applets[i].name; i++)
        if (!strcmp(base, g_applets[i].name))
            return g_applets[i].fn(argc, argv);
    if (argc > 1) {
        for (int i = 0; g_applets[i].name; i++) {
            if (strcmp(argv[1], g_applets[i].name)) continue;
            g_prog = g_applets[i].name;
            return g_applets[i].fn(argc - 1, argv + 1);
        }
    }
    return cmd_ssh(argc, argv);
}
