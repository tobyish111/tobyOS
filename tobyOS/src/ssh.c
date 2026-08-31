/* ssh.c -- minimal SSH-2 transport entry point on TCP/22.
 *
 * Full SSH support requires a real transport stack: version exchange,
 * binary packet protocol, algorithm negotiation, key exchange, host-key
 * signatures, encryption/MAC, user authentication, and channel/session
 * messages. This file now covers the first transport pieces: version
 * exchange, plaintext binary packets, SSH_MSG_KEXINIT negotiation, and
 * a Curve25519/Ed25519 key-exchange reply. It still stops before real
 * encrypted packet mode, user authentication, and channels.
 */

#include <tobyos/ssh.h>
#include <tobyos/tcp.h>
#include <tobyos/net.h>
#include <tobyos/ssh_crypto.h>
#include <tobyos/sec.h>
#include <tobyos/rng.h>
#include <tobyos/users.h>
#include <tobyos/pty.h>
#include <tobyos/users.h>
#include <tobyos/proc.h>
#include <tobyos/sched.h>
#include <tobyos/cpu.h>
#include <tobyos/file.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/pit.h>

#define SSH_IDENT_RAW "SSH-2.0-tobyOS_0.1"
#define SSH_IDENT     SSH_IDENT_RAW "\r\n"
#define SSH_LINE_MAX 256u
#define SSH_RX_MAX 4096u
#define SSH_PAYLOAD_MAX 2048u
#define SSH_KEXINIT_MAX SSH_PAYLOAD_MAX

#define SSH_MSG_DISCONNECT 1u
#define SSH_MSG_SERVICE_REQUEST 5u
#define SSH_MSG_SERVICE_ACCEPT  6u
#define SSH_MSG_NEWKEYS    21u
#define SSH_MSG_KEXINIT    20u
#define SSH_MSG_KEX_ECDH_INIT  30u
#define SSH_MSG_KEX_ECDH_REPLY 31u
#define SSH_MSG_USERAUTH_REQUEST 50u
#define SSH_MSG_USERAUTH_FAILURE 51u
#define SSH_MSG_USERAUTH_SUCCESS 52u
#define SSH_MSG_GLOBAL_REQUEST 80u
#define SSH_MSG_REQUEST_SUCCESS 81u
#define SSH_MSG_REQUEST_FAILURE 82u
#define SSH_MSG_CHANNEL_OPEN 90u
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION 91u
#define SSH_MSG_CHANNEL_OPEN_FAILURE 92u
#define SSH_MSG_CHANNEL_WINDOW_ADJUST 93u
#define SSH_MSG_CHANNEL_DATA 94u
#define SSH_MSG_CHANNEL_EOF 96u
#define SSH_MSG_CHANNEL_CLOSE 97u
#define SSH_MSG_CHANNEL_REQUEST 98u
#define SSH_MSG_CHANNEL_SUCCESS 99u
#define SSH_MSG_CHANNEL_FAILURE 100u

#define SSH_DISCONNECT_PROTOCOL_ERROR 2u
#define SSH_DISCONNECT_KEY_EXCHANGE_FAILED 3u
#define SSH_DISCONNECT_SERVICE_NOT_AVAILABLE 7u

#define SSH_OPEN_ADMINISTRATIVELY_PROHIBITED 1u
#define SSH_OPEN_CONNECT_FAILED 2u

#define SSH_KEX_ALGS     "curve25519-sha256,curve25519-sha256@libssh.org"
#define SSH_HOSTKEY_ALGS "ssh-ed25519"
#define SSH_CIPHER_ALGS  "chacha20-poly1305@openssh.com"
#define SSH_MAC_ALGS     "none"
#define SSH_COMP_ALGS    "none"
#define SSH_SERVICE_USERAUTH "ssh-userauth"
#define SSH_SERVICE_CONNECTION "ssh-connection"
#define SSH_AUTH_METHODS "none"
#define SSH_CHANNEL_WINDOW 32768u
#define SSH_CHANNEL_PACKET 2048u

/* One interactive session.
 *
 * This used to hold a `struct term_session`, which is a 14-builtin command
 * interpreter living inside term.c -- no pipelines, no redirection, no
 * external programs, and an input path that discarded every control
 * character. `cat f | less` did not fail there; it silently ran `cat f` and
 * threw away the rest of the line, because the builtin reads argv[1] and
 * nothing else.
 *
 * Now the session is a real /bin/tsh on a real pty, so the shell doing the
 * work is the same one the bash-parity gate measures. */
struct ssh_channel {
    bool active;
    bool term_started;
    bool exec_mode;
    bool close_sent;
    uint32_t local_id;
    uint32_t remote_id;
    uint32_t remote_window;
    uint32_t remote_max_packet;

    struct file *ptym;          /* pty master, held by the server */
    int          pty_idx;
    int          child_pid;     /* the /bin/tsh we spawned, -1 = none */

    /* The spawn is DEFERRED, not done in the request handler. ssh_poll()
     * runs both from the pid-0 idle loop and from the syscall-return path,
     * so `current_proc()` at request time is whoever happened to syscall
     * last -- and a child inherits its parent's caps, cwd and identity. A
     * shell whose privileges depend on unrelated timing is a flake
     * generator, so we wait for a tick that really is pid 0. */
    bool         want_spawn;
    /* 4 KiB: real command lines fit, and anything past it is
     * REFUSED rather than silently cut (see the exec handler). */
    char         exec_cmd[4096];
    uint16_t     win_cols, win_rows;
};

struct ssh_chachapoly_ctx {
    uint8_t key[SSH_CRYPTO_CHACHAPOLY_KEY_LEN];
    bool active;
};

enum ssh_state {
    SSH_WAIT_IDENT = 0,
    SSH_WAIT_KEXINIT,
    SSH_WAIT_ECDH_INIT,
    SSH_WAIT_NEWKEYS,
    SSH_WAIT_SERVICE_REQUEST,
    SSH_WAIT_USERAUTH_REQUEST,
    SSH_AUTHENTICATED,
    SSH_CHANNEL_OPEN,
    SSH_SENT_DISCONNECT,
};

#define SSH_TX_MAX 65536u
static void ssh_tx_flush(void);

static struct tcp_conn *g_lsn;
static struct tcp_conn *g_cli;
static enum ssh_state  g_state;
static char            g_line[SSH_LINE_MAX];
static size_t          g_line_len;
static uint8_t         g_rx[SSH_RX_MAX];
static size_t          g_rx_len;
static uint8_t         g_packet_plain[SSH_RX_MAX];
static uint8_t         g_send_pkt[SSH_PAYLOAD_MAX + 96u];
static uint8_t         g_send_enc[SSH_PAYLOAD_MAX + 96u];
static uint8_t         g_client_kexinit[SSH_KEXINIT_MAX];
static size_t          g_client_kexinit_len;
static uint8_t         g_server_kexinit[SSH_KEXINIT_MAX];
static size_t          g_server_kexinit_len;
static uint8_t         g_session_id[SHA256_DIGEST_LEN];
static bool            g_have_session_id;
static struct ssh_chachapoly_ctx g_in_cipher;
static struct ssh_chachapoly_ctx g_out_cipher;
static char            g_auth_user[USER_NAME_MAX];
static struct ssh_channel g_chan;
static uint8_t         g_tx[SSH_TX_MAX];
static size_t          g_tx_head, g_tx_tail;
static int             g_worker_pid = -1;
static uint32_t        g_send_seq;
static uint32_t        g_recv_seq;

/* Release the pty master. The child's own fds are closed by its exit path;
 * dropping this reference is what lets the pair be reused. */
static void ssh_channel_teardown(void) {
#ifdef SSH_PTY_DIAG
    kprintf("[ptydiag] ssh teardown idx=%d child=%d\n",
            g_chan.pty_idx, g_chan.child_pid);
#endif
    if (g_chan.ptym) { pty_close(g_chan.ptym); kfree(g_chan.ptym); g_chan.ptym = 0; }
    g_chan.pty_idx = -1;
    g_chan.child_pid = -1;
    g_chan.want_spawn = false;
}

static void ssh_close_client(void) {
    ssh_channel_teardown();
    g_tx_head = g_tx_tail = 0;
    /* Push any queued bytes, then close WITHOUT waiting.
     *
     * tcp_close() waits up to 5 s for the peer's FIN and then lingers through
     * TIME_WAIT -- and ssh_close_client() runs inside ssh_poll(), on pid 0,
     * from net_service_tick(). That parked the idle loop on every session
     * teardown: "pid 0 idle loop stalled 10753ms" from the watchdog, with the
     * whole machine (network, GUI, input) frozen behind it. tcp_close_nowait
     * sends the FIN and hands the handshake plus the linger to
     * tcp_service_tick, which is what it exists for. The conn is untouchable
     * afterwards, which is fine -- g_cli is cleared on the next line. */
    if (g_cli) { ssh_tx_flush(); tcp_close_nowait(g_cli); }
    g_cli = 0;
    g_state = SSH_WAIT_IDENT;
    g_line_len = 0;
    g_line[0] = '\0';
    g_rx_len = 0;
    g_client_kexinit_len = 0;
    g_server_kexinit_len = 0;
    g_have_session_id = false;
    ssh_crypto_wipe(g_session_id, sizeof(g_session_id));
    ssh_crypto_wipe(&g_in_cipher, sizeof(g_in_cipher));
    ssh_crypto_wipe(&g_out_cipher, sizeof(g_out_cipher));
    memset(g_auth_user, 0, sizeof(g_auth_user));
    memset(&g_chan, 0, sizeof(g_chan));
    g_send_seq = 0;
    g_recv_seq = 0;
}

/* ---- TX window ------------------------------------------------------
 *
 * Everything the server sends is buffered here and drained with
 * tcp_send_nb(). It used to go straight out through tcp_send(), which
 * BLOCKS: it waits for the congestion/receive window to open and then waits
 * again for the data to be acknowledged, up to (TCP_RETX_LIMIT + 2) * rto.
 *
 * ssh_poll() runs from net_service_tick() with the BKL held, so that wait
 * parks pid 0 holding the lock and the whole machine stops -- observed as
 * "pid 0 idle loop stalled 10816ms" from the watchdog the first time a real
 * shell produced enough output to fill the window. The old 14-builtin
 * interpreter emitted a few bytes and closed, so it never got there.
 *
 * Same shape as vnc.c, and for the same reason -- see the note at the top
 * of that file. */
static size_t ssh_tx_pending(void) { return g_tx_tail - g_tx_head; }

static void ssh_tx_compact(void) {
    if (g_tx_head == 0) return;
    size_t n = ssh_tx_pending();
    if (n) memmove(g_tx, g_tx + g_tx_head, n);
    g_tx_head = 0;
    g_tx_tail = n;
}

static size_t ssh_tx_space(void) {
    ssh_tx_compact();
    return SSH_TX_MAX - g_tx_tail;
}

/* Drain whatever the send window will take right now. Never blocks. */
static void ssh_tx_flush(void) {
    while (g_cli && g_tx_head < g_tx_tail) {
        long s = tcp_send_nb(g_cli, g_tx + g_tx_head, ssh_tx_pending());
        if (s <= 0) break;
        g_tx_head += (size_t)s;
    }
    if (g_tx_head == g_tx_tail) g_tx_head = g_tx_tail = 0;
}

/* Queue bytes for the client. Returns false only when the buffer is full,
 * which callers treat as a fatal connection error -- the alternative is a
 * silently truncated SSH packet, and the protocol cannot resynchronise from
 * that. Backpressure is applied earlier instead: ssh_drain_channel_output()
 * stops reading the pty while the buffer is close to full, so the shell is
 * throttled rather than its output lost. */
static bool ssh_send_all(struct tcp_conn *c, const void *buf, size_t len) {
    (void)c;
    ssh_tx_flush();                       /* make room if the wire allows */
    if (ssh_tx_space() < len) return false;
    memcpy(g_tx + g_tx_tail, buf, len);
    g_tx_tail += len;
    ssh_tx_flush();
    return true;
}

static void put_u32_be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_u32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           ((uint32_t)p[3]);
}

static void put_u64_be(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v >> 56);
    p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40);
    p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24);
    p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8);
    p[7] = (uint8_t)v;
}

static void ssh_fill_padding(uint8_t *p, size_t n) {
    uint32_t x = (uint32_t)pit_ticks() ^ 0x9e3779b9u ^ g_send_seq;
    for (size_t i = 0; i < n; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        p[i] = (uint8_t)x;
    }
}

static void ssh_chachapoly_len_crypt(const struct ssh_chachapoly_ctx *ctx,
                                     uint32_t seq, uint8_t out[4],
                                     const uint8_t in[4]) {
    uint8_t nonce[8];
    put_u64_be(nonce, seq);
    ssh_crypto_chacha20_djb(out, in, 4, ctx->key + 32u, nonce, 0);
}

static void ssh_chachapoly_payload_crypt(const struct ssh_chachapoly_ctx *ctx,
                                         uint32_t seq, uint8_t *out,
                                         const uint8_t *in, size_t n) {
    uint8_t nonce[8];
    put_u64_be(nonce, seq);
    ssh_crypto_chacha20_djb(out, in, n, ctx->key, nonce, 1);
}

static void ssh_chachapoly_tag(const struct ssh_chachapoly_ctx *ctx,
                               uint32_t seq, const uint8_t *cipher,
                               size_t cipher_len,
                               uint8_t tag[SSH_CRYPTO_POLY1305_TAG_LEN]) {
    uint8_t nonce[8];
    uint8_t poly_key[64];
    put_u64_be(nonce, seq);
    ssh_crypto_chacha20_djb(poly_key, 0, sizeof(poly_key),
                            ctx->key, nonce, 0);
    ssh_crypto_poly1305(tag, cipher, cipher_len, poly_key);
    ssh_crypto_wipe(poly_key, sizeof(poly_key));
}

static bool ssh_chachapoly_tag_ok(const uint8_t *a, const uint8_t *b) {
    return sec_memeq_ct(a, b, SSH_CRYPTO_POLY1305_TAG_LEN) == 0;
}

static bool ssh_send_packet(struct tcp_conn *c,
                            const uint8_t *payload, size_t payload_len) {
    uint8_t *pkt = g_send_pkt;
    if (!payload || payload_len == 0 || payload_len > SSH_PAYLOAD_MAX)
        return false;

    size_t aligned_len = payload_len + (g_out_cipher.active ? 1u : 5u);
    size_t pad_len = 8u - (aligned_len & 7u);
    if (pad_len < 4u) pad_len += 8u;

    size_t packet_len = payload_len + pad_len + 1u;
    size_t total_len = packet_len + 4u;
    if (total_len > SSH_PAYLOAD_MAX + 96u) return false;

    put_u32_be(pkt, (uint32_t)packet_len);
    pkt[4] = (uint8_t)pad_len;
    memcpy(pkt + 5, payload, payload_len);
    ssh_fill_padding(pkt + 5 + payload_len, pad_len);

    if (g_out_cipher.active) {
        uint8_t *enc = g_send_enc;
        uint8_t tag[SSH_CRYPTO_POLY1305_TAG_LEN];
        if (total_len + sizeof(tag) > SSH_PAYLOAD_MAX + 96u) return false;

        ssh_chachapoly_len_crypt(&g_out_cipher, g_send_seq, enc, pkt);
        ssh_chachapoly_payload_crypt(&g_out_cipher, g_send_seq,
                                     enc + 4u, pkt + 4u, packet_len);
        ssh_chachapoly_tag(&g_out_cipher, g_send_seq,
                           enc, total_len, tag);
        memcpy(enc + total_len, tag, sizeof(tag));
        if (!ssh_send_all(c, enc, total_len + sizeof(tag))) return false;
    } else {
        if (!ssh_send_all(c, pkt, total_len)) return false;
    }
    g_send_seq++;
    return true;
}

static bool payload_put_u8(uint8_t *p, size_t cap, size_t *o, uint8_t v) {
    if (*o + 1u > cap) return false;
    p[(*o)++] = v;
    return true;
}

static bool payload_put_u32(uint8_t *p, size_t cap, size_t *o, uint32_t v) {
    if (*o + 4u > cap) return false;
    put_u32_be(p + *o, v);
    *o += 4u;
    return true;
}

static bool payload_put_raw(uint8_t *p, size_t cap, size_t *o,
                            const void *src, size_t n) {
    if (*o + n > cap) return false;
    if (n) memcpy(p + *o, src, n);
    *o += n;
    return true;
}

static bool payload_put_string(uint8_t *p, size_t cap, size_t *o,
                               const char *s) {
    size_t n = strlen(s);
    return payload_put_u32(p, cap, o, (uint32_t)n) &&
           payload_put_raw(p, cap, o, s, n);
}

static bool payload_put_string_raw(uint8_t *p, size_t cap, size_t *o,
                                   const void *src, size_t n) {
    return payload_put_u32(p, cap, o, (uint32_t)n) &&
           payload_put_raw(p, cap, o, src, n);
}

static bool payload_put_mpint(uint8_t *p, size_t cap, size_t *o,
                              const uint8_t *src, size_t n) {
    while (n > 0 && *src == 0) {
        src++;
        n--;
    }
    if (n == 0) return payload_put_u32(p, cap, o, 0);

    bool prefix_zero = (src[0] & 0x80u) != 0;
    if (!payload_put_u32(p, cap, o, (uint32_t)(n + (prefix_zero ? 1u : 0u))))
        return false;
    if (prefix_zero && !payload_put_u8(p, cap, o, 0)) return false;
    return payload_put_raw(p, cap, o, src, n);
}

static bool ssh_send_disconnect_code(struct tcp_conn *c, uint32_t code,
                                     const char *reason) {
    uint8_t payload[256];
    size_t reason_len = strlen(reason);
    if (reason_len > 180) reason_len = 180;

    size_t o = 0;
    if (!payload_put_u8(payload, sizeof(payload), &o, SSH_MSG_DISCONNECT) ||
        !payload_put_u32(payload, sizeof(payload), &o, code) ||
        !payload_put_u32(payload, sizeof(payload), &o,
                         (uint32_t)reason_len) ||
        !payload_put_raw(payload, sizeof(payload), &o, reason, reason_len) ||
        !payload_put_u32(payload, sizeof(payload), &o, 0))
        return false;

    return ssh_send_packet(c, payload, o);
}

static bool ssh_send_disconnect(struct tcp_conn *c, const char *reason) {
    return ssh_send_disconnect_code(c, SSH_DISCONNECT_PROTOCOL_ERROR, reason);
}

static bool ssh_send_kexinit(struct tcp_conn *c) {
    uint8_t payload[512];
    size_t o = 0;

    if (!payload_put_u8(payload, sizeof(payload), &o, SSH_MSG_KEXINIT))
        return false;

    uint8_t cookie[16];
    uint32_t x = (uint32_t)pit_ticks() ^ 0x53534832u;
    for (size_t i = 0; i < sizeof(cookie); i++) {
        x = x * 1664525u + 1013904223u;
        cookie[i] = (uint8_t)(x >> 24);
    }
    if (!payload_put_raw(payload, sizeof(payload), &o,
                         cookie, sizeof(cookie)) ||
        !payload_put_string(payload, sizeof(payload), &o, SSH_KEX_ALGS) ||
        !payload_put_string(payload, sizeof(payload), &o, SSH_HOSTKEY_ALGS) ||
        !payload_put_string(payload, sizeof(payload), &o, SSH_CIPHER_ALGS) ||
        !payload_put_string(payload, sizeof(payload), &o, SSH_CIPHER_ALGS) ||
        !payload_put_string(payload, sizeof(payload), &o, SSH_MAC_ALGS) ||
        !payload_put_string(payload, sizeof(payload), &o, SSH_MAC_ALGS) ||
        !payload_put_string(payload, sizeof(payload), &o, SSH_COMP_ALGS) ||
        !payload_put_string(payload, sizeof(payload), &o, SSH_COMP_ALGS) ||
        !payload_put_string(payload, sizeof(payload), &o, "") ||
        !payload_put_string(payload, sizeof(payload), &o, "") ||
        !payload_put_u8(payload, sizeof(payload), &o, 0) ||
        !payload_put_u32(payload, sizeof(payload), &o, 0))
        return false;

    if (o <= sizeof(g_server_kexinit)) {
        memcpy(g_server_kexinit, payload, o);
        g_server_kexinit_len = o;
    }

    kprintf("[ssh] sending KEXINIT kex=%s hostkey=%s cipher=%s mac=%s\n",
            SSH_KEX_ALGS, SSH_HOSTKEY_ALGS, SSH_CIPHER_ALGS, SSH_MAC_ALGS);
    return ssh_send_packet(c, payload, o);
}

static void hash_ssh_string(struct sha256_ctx *h, const void *p, size_t n) {
    uint8_t len[4];
    put_u32_be(len, (uint32_t)n);
    sha256_update(h, len, sizeof(len));
    sha256_update(h, p, n);
}

static void hash_ssh_mpint(struct sha256_ctx *h, const uint8_t *p, size_t n) {
    uint8_t mp[SSH_CRYPTO_X25519_KEY_LEN + 5u];
    size_t o = 0;
    if (payload_put_mpint(mp, sizeof(mp), &o, p, n))
        sha256_update(h, mp, o);
    ssh_crypto_wipe(mp, sizeof(mp));
}

static bool encode_ssh_mpint(uint8_t *out, size_t cap, size_t *out_n,
                             const uint8_t *p, size_t n) {
    size_t o = 0;
    if (!payload_put_mpint(out, cap, &o, p, n)) return false;
    *out_n = o;
    return true;
}

static bool ssh_kdf(uint8_t *out, size_t out_len, const uint8_t *k_mpint,
                    size_t k_mpint_len, const uint8_t h[SHA256_DIGEST_LEN],
                    char letter) {
    uint8_t digest[SHA256_DIGEST_LEN];
    size_t produced = 0;

    struct sha256_ctx s;
    sha256_init(&s);
    sha256_update(&s, k_mpint, k_mpint_len);
    sha256_update(&s, h, SHA256_DIGEST_LEN);
    sha256_update(&s, &letter, 1);
    sha256_update(&s, g_session_id, SHA256_DIGEST_LEN);
    sha256_final(&s, digest);

    while (produced < out_len) {
        size_t take = out_len - produced;
        if (take > sizeof(digest)) take = sizeof(digest);
        memcpy(out + produced, digest, take);
        produced += take;
        if (produced >= out_len) break;

        sha256_init(&s);
        sha256_update(&s, k_mpint, k_mpint_len);
        sha256_update(&s, h, SHA256_DIGEST_LEN);
        sha256_update(&s, out, produced);
        sha256_final(&s, digest);
    }

    ssh_crypto_wipe(digest, sizeof(digest));
    return true;
}

static bool ssh_install_pending_keys(const uint8_t shared[SSH_CRYPTO_X25519_KEY_LEN],
                                     const uint8_t h[SHA256_DIGEST_LEN]) {
    uint8_t k_mpint[SSH_CRYPTO_X25519_KEY_LEN + 5u];
    size_t k_mpint_len = 0;
    if (!encode_ssh_mpint(k_mpint, sizeof(k_mpint), &k_mpint_len,
                          shared, SSH_CRYPTO_X25519_KEY_LEN))
        return false;

    if (!g_have_session_id) {
        memcpy(g_session_id, h, SHA256_DIGEST_LEN);
        g_have_session_id = true;
    }

    bool ok = ssh_kdf(g_in_cipher.key, sizeof(g_in_cipher.key),
                      k_mpint, k_mpint_len, h, 'C') &&
              ssh_kdf(g_out_cipher.key, sizeof(g_out_cipher.key),
                      k_mpint, k_mpint_len, h, 'D');
    ssh_crypto_wipe(k_mpint, sizeof(k_mpint));
    return ok;
}

static bool build_hostkey_blob(uint8_t *out, size_t cap, size_t *out_n) {
    const uint8_t *pk = ssh_hostkey_public();
    if (!pk) return false;

    size_t o = 0;
    if (!payload_put_string(out, cap, &o, SSH_HOSTKEY_ALGS) ||
        !payload_put_string_raw(out, cap, &o, pk,
                                SSH_CRYPTO_ED25519_PUBLIC_LEN))
        return false;
    *out_n = o;
    return true;
}

static bool build_signature_blob(uint8_t *out, size_t cap, size_t *out_n,
                                 const uint8_t sig[SSH_CRYPTO_ED25519_SIG_LEN]) {
    size_t o = 0;
    if (!payload_put_string(out, cap, &o, SSH_HOSTKEY_ALGS) ||
        !payload_put_string_raw(out, cap, &o, sig,
                                SSH_CRYPTO_ED25519_SIG_LEN))
        return false;
    *out_n = o;
    return true;
}

static bool ssh_send_newkeys(struct tcp_conn *c) {
    uint8_t payload[1] = { SSH_MSG_NEWKEYS };
    return ssh_send_packet(c, payload, sizeof(payload));
}

static bool ssh_send_service_accept(struct tcp_conn *c, const char *service) {
    uint8_t payload[96];
    size_t o = 0;
    return payload_put_u8(payload, sizeof(payload), &o,
                          SSH_MSG_SERVICE_ACCEPT) &&
           payload_put_string(payload, sizeof(payload), &o, service) &&
           ssh_send_packet(c, payload, o);
}

static bool ssh_send_userauth_failure(struct tcp_conn *c) {
    uint8_t payload[96];
    size_t o = 0;
    return payload_put_u8(payload, sizeof(payload), &o,
                          SSH_MSG_USERAUTH_FAILURE) &&
           payload_put_string(payload, sizeof(payload), &o,
                              SSH_AUTH_METHODS) &&
           payload_put_u8(payload, sizeof(payload), &o, 0) &&
           ssh_send_packet(c, payload, o);
}

static bool ssh_send_userauth_success(struct tcp_conn *c) {
    uint8_t payload[1] = { SSH_MSG_USERAUTH_SUCCESS };
    return ssh_send_packet(c, payload, sizeof(payload));
}

static bool ssh_send_request_failure(struct tcp_conn *c) {
    uint8_t payload[1] = { SSH_MSG_REQUEST_FAILURE };
    return ssh_send_packet(c, payload, sizeof(payload));
}

static bool ssh_send_channel_open_confirmation(struct tcp_conn *c) {
    uint8_t payload[64];
    size_t o = 0;
    return payload_put_u8(payload, sizeof(payload), &o,
                          SSH_MSG_CHANNEL_OPEN_CONFIRMATION) &&
           payload_put_u32(payload, sizeof(payload), &o, g_chan.remote_id) &&
           payload_put_u32(payload, sizeof(payload), &o, g_chan.local_id) &&
           payload_put_u32(payload, sizeof(payload), &o, SSH_CHANNEL_WINDOW) &&
           payload_put_u32(payload, sizeof(payload), &o, SSH_CHANNEL_PACKET) &&
           ssh_send_packet(c, payload, o);
}

static bool ssh_send_channel_open_failure(struct tcp_conn *c,
                                          uint32_t recipient,
                                          uint32_t reason,
                                          const char *text) {
    uint8_t payload[256];
    size_t o = 0;
    return payload_put_u8(payload, sizeof(payload), &o,
                          SSH_MSG_CHANNEL_OPEN_FAILURE) &&
           payload_put_u32(payload, sizeof(payload), &o, recipient) &&
           payload_put_u32(payload, sizeof(payload), &o, reason) &&
           payload_put_string(payload, sizeof(payload), &o, text) &&
           payload_put_string(payload, sizeof(payload), &o, "") &&
           ssh_send_packet(c, payload, o);
}

static bool ssh_send_channel_success(struct tcp_conn *c, uint32_t recipient) {
    uint8_t payload[5];
    size_t o = 0;
    return payload_put_u8(payload, sizeof(payload), &o,
                          SSH_MSG_CHANNEL_SUCCESS) &&
           payload_put_u32(payload, sizeof(payload), &o, recipient) &&
           ssh_send_packet(c, payload, o);
}

static bool ssh_send_channel_failure(struct tcp_conn *c, uint32_t recipient) {
    uint8_t payload[5];
    size_t o = 0;
    return payload_put_u8(payload, sizeof(payload), &o,
                          SSH_MSG_CHANNEL_FAILURE) &&
           payload_put_u32(payload, sizeof(payload), &o, recipient) &&
           ssh_send_packet(c, payload, o);
}

static bool ssh_send_channel_data(struct tcp_conn *c, const void *data,
                                  size_t n) {
    uint8_t payload[SSH_CHANNEL_PACKET + 16u];
    size_t o = 0;
    if (n > SSH_CHANNEL_PACKET) n = SSH_CHANNEL_PACKET;
    return payload_put_u8(payload, sizeof(payload), &o,
                          SSH_MSG_CHANNEL_DATA) &&
           payload_put_u32(payload, sizeof(payload), &o, g_chan.remote_id) &&
           payload_put_string_raw(payload, sizeof(payload), &o, data, n) &&
           ssh_send_packet(c, payload, o);
}

static bool ssh_send_channel_eof(struct tcp_conn *c) {
    uint8_t payload[5];
    size_t o = 0;
    return payload_put_u8(payload, sizeof(payload), &o,
                          SSH_MSG_CHANNEL_EOF) &&
           payload_put_u32(payload, sizeof(payload), &o, g_chan.remote_id) &&
           ssh_send_packet(c, payload, o);
}

static bool ssh_send_channel_exit_status(struct tcp_conn *c,
                                         uint32_t status) {
    uint8_t payload[64];
    size_t o = 0;
    return payload_put_u8(payload, sizeof(payload), &o,
                          SSH_MSG_CHANNEL_REQUEST) &&
           payload_put_u32(payload, sizeof(payload), &o, g_chan.remote_id) &&
           payload_put_string(payload, sizeof(payload), &o, "exit-status") &&
           payload_put_u8(payload, sizeof(payload), &o, 0) &&
           payload_put_u32(payload, sizeof(payload), &o, status) &&
           ssh_send_packet(c, payload, o);
}

static bool ssh_send_channel_close(struct tcp_conn *c) {
    if (g_chan.close_sent) return true;
    uint8_t payload[5];
    size_t o = 0;
    bool ok = payload_put_u8(payload, sizeof(payload), &o,
                             SSH_MSG_CHANNEL_CLOSE) &&
              payload_put_u32(payload, sizeof(payload), &o,
                              g_chan.remote_id) &&
              ssh_send_packet(c, payload, o);
    if (ok) g_chan.close_sent = true;
    return ok;
}

static bool read_string(const uint8_t *p, size_t n, size_t *o,
                        const uint8_t **out, size_t *out_n) {
    if (*o + 4u > n) return false;
    uint32_t len = get_u32_be(p + *o);
    *o += 4u;
    if (len > n - *o) return false;
    *out = p + *o;
    *out_n = len;
    *o += len;
    return true;
}

static bool read_bool(const uint8_t *p, size_t n, size_t *o, bool *out) {
    if (*o + 1u > n) return false;
    *out = p[(*o)++] != 0;
    return true;
}

static bool ssh_string_eq(const uint8_t *s, size_t n, const char *lit) {
    size_t lit_n = strlen(lit);
    return n == lit_n && memcmp(s, lit, lit_n) == 0;
}

static void copy_ssh_string(char *dst, size_t cap,
                            const uint8_t *src, size_t n) {
    if (cap == 0) return;
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool namelist_has(const uint8_t *list, size_t list_len,
                         const char *name) {
    size_t name_len = strlen(name);
    size_t i = 0;
    while (i <= list_len) {
        size_t start = i;
        while (i < list_len && list[i] != ',') i++;
        size_t tok_len = i - start;
        if (tok_len == name_len &&
            memcmp(list + start, name, name_len) == 0)
            return true;
        if (i == list_len) break;
        i++;
    }
    return false;
}

static void ssh_log_namelist(const char *label,
                             const uint8_t *list, size_t list_len) {
    char tmp[96];
    size_t n = list_len;
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1u;
    memcpy(tmp, list, n);
    tmp[n] = '\0';
    kprintf("[ssh] client %s: %s%s\n", label, tmp,
            list_len >= sizeof(tmp) ? "..." : "");
}

static void ssh_handle_kexinit_payload(const uint8_t *payload, size_t n) {
    if (n < 1u + 16u || payload[0] != SSH_MSG_KEXINIT) {
        kprintf("[ssh] expected KEXINIT, got msg=%u len=%u\n",
                n ? (unsigned)payload[0] : 0u, (unsigned)n);
        (void)ssh_send_disconnect(g_cli,
            "expected SSH_MSG_KEXINIT after version exchange");
        g_state = SSH_SENT_DISCONNECT;
        return;
    }

    size_t o = 1u + 16u;
    const uint8_t *kex, *hostkey, *enc_c2s, *enc_s2c, *mac_c2s, *mac_s2c;
    const uint8_t *comp_c2s, *comp_s2c, *lang_c2s, *lang_s2c;
    size_t kex_n, hostkey_n, enc_c2s_n, enc_s2c_n, mac_c2s_n, mac_s2c_n;
    size_t comp_c2s_n, comp_s2c_n, lang_c2s_n, lang_s2c_n;

    if (!read_string(payload, n, &o, &kex, &kex_n) ||
        !read_string(payload, n, &o, &hostkey, &hostkey_n) ||
        !read_string(payload, n, &o, &enc_c2s, &enc_c2s_n) ||
        !read_string(payload, n, &o, &enc_s2c, &enc_s2c_n) ||
        !read_string(payload, n, &o, &mac_c2s, &mac_c2s_n) ||
        !read_string(payload, n, &o, &mac_s2c, &mac_s2c_n) ||
        !read_string(payload, n, &o, &comp_c2s, &comp_c2s_n) ||
        !read_string(payload, n, &o, &comp_s2c, &comp_s2c_n) ||
        !read_string(payload, n, &o, &lang_c2s, &lang_c2s_n) ||
        !read_string(payload, n, &o, &lang_s2c, &lang_s2c_n) ||
        o + 5u > n) {
        kprintf("[ssh] malformed KEXINIT len=%u\n", (unsigned)n);
        (void)ssh_send_disconnect(g_cli, "malformed SSH_MSG_KEXINIT");
        g_state = SSH_SENT_DISCONNECT;
        return;
    }
    (void)lang_c2s;
    (void)lang_s2c;
    (void)lang_c2s_n;
    (void)lang_s2c_n;

    bool first_follows = payload[o] != 0;
    bool have_kex = namelist_has(kex, kex_n, "curve25519-sha256") ||
                    namelist_has(kex, kex_n, "curve25519-sha256@libssh.org");
    bool have_hostkey = namelist_has(hostkey, hostkey_n, SSH_HOSTKEY_ALGS);
    bool have_enc_c2s = namelist_has(enc_c2s, enc_c2s_n, SSH_CIPHER_ALGS);
    bool have_enc_s2c = namelist_has(enc_s2c, enc_s2c_n, SSH_CIPHER_ALGS);
    bool have_mac_c2s = namelist_has(mac_c2s, mac_c2s_n, SSH_MAC_ALGS);
    bool have_mac_s2c = namelist_has(mac_s2c, mac_s2c_n, SSH_MAC_ALGS);
    bool have_comp_c2s = namelist_has(comp_c2s, comp_c2s_n, SSH_COMP_ALGS);
    bool have_comp_s2c = namelist_has(comp_s2c, comp_s2c_n, SSH_COMP_ALGS);

    ssh_log_namelist("kex", kex, kex_n);
    ssh_log_namelist("hostkey", hostkey, hostkey_n);
    ssh_log_namelist("cipher c2s", enc_c2s, enc_c2s_n);

    kprintf("[ssh] KEXINIT overlap: kex=%d hostkey=%d enc=%d/%d "
            "mac=%d/%d comp=%d/%d first_follows=%d\n",
            have_kex, have_hostkey, have_enc_c2s, have_enc_s2c,
            have_mac_c2s, have_mac_s2c, have_comp_c2s, have_comp_s2c,
            first_follows);

    if (!have_kex || !have_hostkey || !have_enc_c2s || !have_enc_s2c ||
        !have_comp_c2s || !have_comp_s2c) {
        (void)ssh_send_disconnect_code(g_cli, SSH_DISCONNECT_KEY_EXCHANGE_FAILED,
            "no mutually supported SSH transport algorithms");
        g_state = SSH_SENT_DISCONNECT;
        return;
    }

    if (n <= sizeof(g_client_kexinit)) {
        memcpy(g_client_kexinit, payload, n);
        g_client_kexinit_len = n;
    } else {
        (void)ssh_send_disconnect(g_cli, "KEXINIT transcript too large");
        g_state = SSH_SENT_DISCONNECT;
        return;
    }

    kprintf("[ssh] waiting for Curve25519 ECDH_INIT\n");
    g_state = SSH_WAIT_ECDH_INIT;
}

static void ssh_handle_ecdh_init_payload(const uint8_t *payload, size_t n) {
    if (n < 1u || payload[0] != SSH_MSG_KEX_ECDH_INIT) {
        kprintf("[ssh] expected ECDH_INIT, got msg=%u len=%u\n",
                n ? (unsigned)payload[0] : 0u, (unsigned)n);
        (void)ssh_send_disconnect(g_cli,
            "expected SSH_MSG_KEX_ECDH_INIT after KEXINIT");
        g_state = SSH_SENT_DISCONNECT;
        return;
    }

    size_t o = 1u;
    const uint8_t *client_pub;
    size_t client_pub_len;
    if (!read_string(payload, n, &o, &client_pub, &client_pub_len) ||
        client_pub_len != SSH_CRYPTO_X25519_KEY_LEN || o != n) {
        (void)ssh_send_disconnect(g_cli, "malformed SSH_MSG_KEX_ECDH_INIT");
        g_state = SSH_SENT_DISCONNECT;
        return;
    }

    uint8_t server_secret[SSH_CRYPTO_X25519_KEY_LEN];
    uint8_t server_pub[SSH_CRYPTO_X25519_KEY_LEN];
    uint8_t shared[SSH_CRYPTO_X25519_KEY_LEN];
    rng_fill(server_secret, sizeof(server_secret));
    ssh_crypto_x25519_public(server_pub, server_secret);
    ssh_crypto_x25519_shared(shared, server_secret, client_pub);

    bool shared_nonzero = false;
    for (size_t i = 0; i < sizeof(shared); i++) shared_nonzero |= shared[i] != 0;
    if (!shared_nonzero) {
        ssh_crypto_wipe(server_secret, sizeof(server_secret));
        ssh_crypto_wipe(shared, sizeof(shared));
        (void)ssh_send_disconnect_code(g_cli, SSH_DISCONNECT_KEY_EXCHANGE_FAILED,
                                       "invalid Curve25519 public key");
        g_state = SSH_SENT_DISCONNECT;
        return;
    }

    uint8_t hostkey[64];
    size_t hostkey_len = 0;
    uint8_t sig[SSH_CRYPTO_ED25519_SIG_LEN];
    uint8_t sig_blob[96];
    size_t sig_blob_len = 0;
    uint8_t exchange_hash[SHA256_DIGEST_LEN];

    if (!build_hostkey_blob(hostkey, sizeof(hostkey), &hostkey_len)) {
        ssh_crypto_wipe(server_secret, sizeof(server_secret));
        ssh_crypto_wipe(shared, sizeof(shared));
        (void)ssh_send_disconnect(g_cli, "host key unavailable");
        g_state = SSH_SENT_DISCONNECT;
        return;
    }

    struct sha256_ctx h;
    sha256_init(&h);
    hash_ssh_string(&h, g_line, g_line_len);
    hash_ssh_string(&h, SSH_IDENT_RAW, strlen(SSH_IDENT_RAW));
    hash_ssh_string(&h, g_client_kexinit, g_client_kexinit_len);
    hash_ssh_string(&h, g_server_kexinit, g_server_kexinit_len);
    hash_ssh_string(&h, hostkey, hostkey_len);
    hash_ssh_string(&h, client_pub, client_pub_len);
    hash_ssh_string(&h, server_pub, sizeof(server_pub));
    hash_ssh_mpint(&h, shared, sizeof(shared));
    sha256_final(&h, exchange_hash);

    if (!ssh_install_pending_keys(shared, exchange_hash)) {
        ssh_crypto_wipe(server_secret, sizeof(server_secret));
        ssh_crypto_wipe(shared, sizeof(shared));
        (void)ssh_send_disconnect(g_cli, "key derivation failed");
        g_state = SSH_SENT_DISCONNECT;
        return;
    }

    ssh_hostkey_sign(sig, exchange_hash, sizeof(exchange_hash));
    if (!build_signature_blob(sig_blob, sizeof(sig_blob), &sig_blob_len, sig)) {
        ssh_crypto_wipe(server_secret, sizeof(server_secret));
        ssh_crypto_wipe(shared, sizeof(shared));
        ssh_crypto_wipe(sig, sizeof(sig));
        (void)ssh_send_disconnect(g_cli, "signature blob build failed");
        g_state = SSH_SENT_DISCONNECT;
        return;
    }

    uint8_t reply[256];
    o = 0;
    bool ok = payload_put_u8(reply, sizeof(reply), &o, SSH_MSG_KEX_ECDH_REPLY) &&
              payload_put_string_raw(reply, sizeof(reply), &o,
                                     hostkey, hostkey_len) &&
              payload_put_string_raw(reply, sizeof(reply), &o,
                                     server_pub, sizeof(server_pub)) &&
              payload_put_string_raw(reply, sizeof(reply), &o,
                                     sig_blob, sig_blob_len);

    ssh_crypto_wipe(server_secret, sizeof(server_secret));
    ssh_crypto_wipe(shared, sizeof(shared));
    ssh_crypto_wipe(sig, sizeof(sig));

    if (!ok || !ssh_send_packet(g_cli, reply, o) || !ssh_send_newkeys(g_cli)) {
        ssh_close_client();
        return;
    }
    g_out_cipher.active = true;

    kprintf("[ssh] sent ECDH_REPLY and NEWKEYS; encrypted/auth layer next\n");
    g_state = SSH_WAIT_NEWKEYS;
}

static void ssh_handle_service_request_payload(const uint8_t *payload,
                                               size_t n) {
    if (n < 1u || payload[0] != SSH_MSG_SERVICE_REQUEST) {
        (void)ssh_send_disconnect(g_cli,
            "expected SSH_MSG_SERVICE_REQUEST after NEWKEYS");
        g_state = SSH_SENT_DISCONNECT;
        return;
    }

    size_t o = 1u;
    const uint8_t *service;
    size_t service_n;
    if (!read_string(payload, n, &o, &service, &service_n) || o != n) {
        (void)ssh_send_disconnect(g_cli, "malformed SSH_MSG_SERVICE_REQUEST");
        g_state = SSH_SENT_DISCONNECT;
        return;
    }

    if (!ssh_string_eq(service, service_n, SSH_SERVICE_USERAUTH)) {
        (void)ssh_send_disconnect_code(g_cli,
            SSH_DISCONNECT_SERVICE_NOT_AVAILABLE,
            "only ssh-userauth service is available");
        g_state = SSH_SENT_DISCONNECT;
        return;
    }

    if (!ssh_send_service_accept(g_cli, SSH_SERVICE_USERAUTH)) {
        ssh_close_client();
        return;
    }
    kprintf("[ssh] accepted ssh-userauth service\n");
    g_state = SSH_WAIT_USERAUTH_REQUEST;
}

static void ssh_handle_userauth_request_payload(const uint8_t *payload,
                                                size_t n) {
    if (n < 1u || payload[0] != SSH_MSG_USERAUTH_REQUEST) {
        (void)ssh_send_disconnect(g_cli,
            "expected SSH_MSG_USERAUTH_REQUEST");
        g_state = SSH_SENT_DISCONNECT;
        return;
    }

    size_t o = 1u;
    const uint8_t *user, *service, *method;
    size_t user_n, service_n, method_n;
    if (!read_string(payload, n, &o, &user, &user_n) ||
        !read_string(payload, n, &o, &service, &service_n) ||
        !read_string(payload, n, &o, &method, &method_n)) {
        (void)ssh_send_disconnect(g_cli, "malformed SSH_MSG_USERAUTH_REQUEST");
        g_state = SSH_SENT_DISCONNECT;
        return;
    }

    char name[USER_NAME_MAX];
    copy_ssh_string(name, sizeof(name), user, user_n);

    bool service_ok = ssh_string_eq(service, service_n,
                                    SSH_SERVICE_CONNECTION);
    bool method_none = ssh_string_eq(method, method_n, "none");
    const struct user *u = service_ok && method_none
        ? users_lookup_by_name(name)
        : 0;

    if (!service_ok) {
        kprintf("[ssh] auth rejected for user '%s': service mismatch\n",
                name);
    } else if (!method_none) {
        bool change = false;
        if (ssh_string_eq(method, method_n, "password")) {
            const uint8_t *password;
            size_t password_n;
            (void)read_bool(payload, n, &o, &change);
            (void)read_string(payload, n, &o, &password, &password_n);
            (void)change;
            (void)password;
            (void)password_n;
        }
        kprintf("[ssh] auth rejected for user '%s': method unsupported\n",
                name);
    } else if (!u) {
        kprintf("[ssh] auth rejected for unknown user '%s'\n", name);
    }

    if (!u) {
        if (!ssh_send_userauth_failure(g_cli)) ssh_close_client();
        return;
    }

    copy_ssh_string(g_auth_user, sizeof(g_auth_user), user, user_n);
    if (!ssh_send_userauth_success(g_cli)) {
        ssh_close_client();
        return;
    }
    kprintf("[ssh] user '%s' authenticated with method=none uid=%d gid=%d\n",
            g_auth_user, u->uid, u->gid);
    g_state = SSH_AUTHENTICATED;
}

static void ssh_handle_global_request_payload(const uint8_t *payload,
                                              size_t n) {
    size_t o = 1u;
    const uint8_t *name;
    size_t name_n;
    bool want_reply = false;
    if (!read_string(payload, n, &o, &name, &name_n) ||
        !read_bool(payload, n, &o, &want_reply)) {
        (void)ssh_send_disconnect(g_cli, "malformed SSH_MSG_GLOBAL_REQUEST");
        g_state = SSH_SENT_DISCONNECT;
        return;
    }

    char tmp[64];
    copy_ssh_string(tmp, sizeof(tmp), name, name_n);
    kprintf("[ssh] global request '%s' unsupported\n", tmp);
    if (want_reply && !ssh_send_request_failure(g_cli)) ssh_close_client();
}

static void ssh_handle_channel_open_payload(const uint8_t *payload,
                                            size_t n) {
    size_t o = 1u;
    const uint8_t *kind;
    size_t kind_n;
    uint32_t sender, win, maxpkt;
    if (!read_string(payload, n, &o, &kind, &kind_n) || o + 12u > n) {
        (void)ssh_send_disconnect(g_cli, "malformed SSH_MSG_CHANNEL_OPEN");
        g_state = SSH_SENT_DISCONNECT;
        return;
    }
    sender = get_u32_be(payload + o); o += 4u;
    win = get_u32_be(payload + o); o += 4u;
    maxpkt = get_u32_be(payload + o); o += 4u;

    if (!ssh_string_eq(kind, kind_n, "session") || g_chan.active) {
        (void)ssh_send_channel_open_failure(g_cli, sender,
            SSH_OPEN_ADMINISTRATIVELY_PROHIBITED,
            "only one session channel is available");
        return;
    }

    /* The pty and the shell are created on the "shell"/"exec" request, not
     * here: a client may open the channel and never ask for either. */
    memset(&g_chan, 0, sizeof(g_chan));
    g_chan.pty_idx = -1;
    g_chan.child_pid = -1;
    g_chan.active = true;
    g_chan.local_id = 0;
    g_chan.remote_id = sender;
    g_chan.remote_window = win;
    g_chan.remote_max_packet = maxpkt ? maxpkt : SSH_CHANNEL_PACKET;

    if (!ssh_send_channel_open_confirmation(g_cli)) {
        ssh_close_client();
        return;
    }
    kprintf("[ssh] session channel open remote=%u window=%u maxpkt=%u\n",
            (unsigned)sender, (unsigned)win, (unsigned)maxpkt);
    g_state = SSH_CHANNEL_OPEN;
}

static void ssh_handle_channel_request_payload(const uint8_t *payload,
                                               size_t n) {
    size_t o = 1u;
    uint32_t recipient;
    const uint8_t *type;
    size_t type_n;
    bool want_reply;
    if (o + 4u > n) goto malformed;
    recipient = get_u32_be(payload + o); o += 4u;
    if (!read_string(payload, n, &o, &type, &type_n) ||
        !read_bool(payload, n, &o, &want_reply))
        goto malformed;

    if (!g_chan.active || recipient != g_chan.local_id) {
        if (want_reply) (void)ssh_send_channel_failure(g_cli, recipient);
        return;
    }

    if (ssh_string_eq(type, type_n, "pty-req")) {
        /* TERM string, then cols/rows in characters. Keep the geometry so the
         * slave's winsize is right before the shell starts -- `less` and every
         * other full-screen program sizes itself from it. */
        {
            const uint8_t *term_s; size_t term_n;
            if (read_string(payload, n, &o, &term_s, &term_n) && o + 8u <= n) {
                g_chan.win_cols = (uint16_t)get_u32_be(payload + o);
                g_chan.win_rows = (uint16_t)get_u32_be(payload + o + 4u);
            }
        }
        if (want_reply && !ssh_send_channel_success(g_cli, recipient)) {
            ssh_close_client();
            return;
        }
        kprintf("[ssh] pty-req accepted for '%s' (%ux%u)\n",
                g_auth_user, g_chan.win_cols, g_chan.win_rows);
        return;
    }

    if (ssh_string_eq(type, type_n, "shell")) {
        g_chan.term_started = true;
        g_chan.exec_mode = false;
        g_chan.exec_cmd[0] = 0;
        g_chan.want_spawn = true;      /* ssh_poll spawns it on a pid-0 tick */
        if (want_reply && !ssh_send_channel_success(g_cli, recipient)) {
            ssh_close_client();
            return;
        }
        kprintf("[ssh] shell request accepted for '%s'\n", g_auth_user);
        return;
    }

    if (ssh_string_eq(type, type_n, "exec")) {
        const uint8_t *cmd;
        size_t cmd_n;
        if (!read_string(payload, n, &o, &cmd, &cmd_n)) goto malformed;
        g_chan.term_started = true;
        g_chan.exec_mode = true;
        /* Handed to `tsh -c` verbatim, so `ssh host 'a | b'` gets the
         * same parser as an interactive line, not a second dialect. */
        /* REFUSE an over-long command; never truncate one.
         *
         * This used to clamp to the buffer and run the fragment. The
         * buffer was 256 bytes, so `ssh host "...; cat c.txt"` at 262
         * characters arrived as `...; ca` and the shell reported
         * "failed to launch /bin/ca" -- which at least LOOKS wrong.
         * The dangerous shape is the one that still parses: a command
         * cut at a path separator or mid-argument runs something the
         * caller never wrote, and reports success for it. Silence is
         * the bug here, not the limit.
         *
         * The limit itself is now 4 KiB, which is past where real
         * command lines sit; anything longer is refused out loud. */
        if (cmd_n >= sizeof(g_chan.exec_cmd)) {
            kprintf("[ssh] exec command refused: %lu bytes, limit %lu\n",
                    (unsigned long)cmd_n,
                    (unsigned long)(sizeof(g_chan.exec_cmd) - 1u));
            g_chan.exec_mode = false;
            g_chan.term_started = false;
            if (want_reply) (void)ssh_send_channel_failure(g_cli, recipient);
            return;
        }
        memcpy(g_chan.exec_cmd, cmd, cmd_n);
        g_chan.exec_cmd[cmd_n] = 0;
        g_chan.want_spawn = true;
        if (want_reply && !ssh_send_channel_success(g_cli, recipient)) {
            ssh_close_client();
            return;
        }
        kprintf("[ssh] exec request accepted for '%s'\n", g_auth_user);
        return;
    }

    if (ssh_string_eq(type, type_n, "window-change")) {
        if (o + 8u <= n) {
            g_chan.win_cols = (uint16_t)get_u32_be(payload + o);
            g_chan.win_rows = (uint16_t)get_u32_be(payload + o + 4u);
            if (g_chan.ptym)
                pty_set_winsize(g_chan.ptym, g_chan.win_cols, g_chan.win_rows);
        }
        if (want_reply && !ssh_send_channel_success(g_cli, recipient)) {
            ssh_close_client();
            return;
        }
        return;
    }

    if (ssh_string_eq(type, type_n, "env")) {
        if (want_reply && !ssh_send_channel_success(g_cli, recipient)) {
            ssh_close_client();
            return;
        }
        return;
    }

    char tmp[64];
    copy_ssh_string(tmp, sizeof(tmp), type, type_n);
    kprintf("[ssh] channel request '%s' unsupported\n", tmp);
    if (want_reply && !ssh_send_channel_failure(g_cli, recipient))
        ssh_close_client();
    return;

malformed:
    (void)ssh_send_disconnect(g_cli, "malformed SSH_MSG_CHANNEL_REQUEST");
    g_state = SSH_SENT_DISCONNECT;
}

static void ssh_handle_channel_data_payload(const uint8_t *payload,
                                            size_t n) {
    size_t o = 1u;
    uint32_t recipient;
    const uint8_t *data;
    size_t data_n;
    if (o + 4u > n) goto malformed;
    recipient = get_u32_be(payload + o); o += 4u;
    if (!read_string(payload, n, &o, &data, &data_n)) goto malformed;
    if (!g_chan.active || recipient != g_chan.local_id || !g_chan.ptym)
        return;
    /* Straight into the line discipline: no filtering. term.c dropped every
     * control character, which is why nothing interactive could be driven
     * over SSH -- no arrows, no Ctrl-C, no Tab. */
    (void)pty_master_write(g_chan.ptym, (const char *)data, data_n);
    return;

malformed:
    (void)ssh_send_disconnect(g_cli, "malformed SSH_MSG_CHANNEL_DATA");
    g_state = SSH_SENT_DISCONNECT;
}

static void ssh_handle_channel_control_payload(const uint8_t *payload,
                                               size_t n) {
    uint8_t msg = payload[0];
    size_t o = 1u;
    uint32_t recipient;
    if (o + 4u > n) {
        (void)ssh_send_disconnect(g_cli, "malformed SSH channel control");
        g_state = SSH_SENT_DISCONNECT;
        return;
    }
    recipient = get_u32_be(payload + o); o += 4u;
    if (!g_chan.active || recipient != g_chan.local_id) return;

    if (msg == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
        if (o + 4u <= n) {
            uint32_t add = get_u32_be(payload + o);
            if (0xffffffffu - g_chan.remote_window < add)
                g_chan.remote_window = 0xffffffffu;
            else
                g_chan.remote_window += add;
        }
        return;
    }

    if (msg == SSH_MSG_CHANNEL_EOF) {
        return;
    }

    if (msg == SSH_MSG_CHANNEL_CLOSE) {
        (void)ssh_send_channel_close(g_cli);
        ssh_channel_teardown();
        g_chan.active = false;
        return;
    }
}

/* Start /bin/tsh on a fresh pty for this channel.
 *
 * Called ONLY from a tick where current_proc() is pid 0 -- see the note on
 * ssh_channel.want_spawn. Everything the child inherits (caps, cwd, uid) then
 * comes from a known parent instead of whichever process last made a syscall.
 *
 * Reference counting, which this codebase has been bitten by before:
 * install_initial_fds() CLONES every struct file it is handed, so those three
 * clones belong to the child and the one pty_open_slave() returned is still
 * ours. Dropping it here leaves slave_refs == 3, all owned by the child, so
 * its exit takes them to zero -- which is exactly the EOF the pump below
 * watches for to close the channel. */
static bool ssh_spawn_shell(void) {
    g_chan.want_spawn = false;

    struct file *m = pty_open_master();
    if (!m) {
        kprintf("[ssh] no free pty for '%s'\n", g_auth_user);
        return false;
    }
    int idx = pty_index_of(m);
    struct file *sl = (idx >= 0) ? pty_open_slave(idx) : 0;
    if (!sl) {
        pty_close(m); kfree(m);
        kprintf("[ssh] pty slave open failed\n");
        return false;
    }
    if (g_chan.win_cols || g_chan.win_rows)
        pty_set_winsize(m, g_chan.win_cols, g_chan.win_rows);

    char a0[] = "tsh", a1[] = "-c";
    char *argv[3];
    int   argc = 1;
    argv[0] = a0;
    if (g_chan.exec_mode && g_chan.exec_cmd[0]) {
        argv[1] = a1;
        argv[2] = g_chan.exec_cmd;
        argc = 3;
    }

    struct proc_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.path = "/bin/tsh";
    spec.name = "tsh-ssh";
    spec.fd0 = sl; spec.fd1 = sl; spec.fd2 = sl;
    spec.argc = argc;
    spec.argv = argv;
    spec.cwd  = "/";

    int pid = proc_spawn(&spec);
    pty_close(sl); kfree(sl);        /* the child holds its own clones now */

    if (pid < 0) {
        pty_close(m); kfree(m);
        kprintf("[ssh] spawn of /bin/tsh FAILED rc=%d\n", pid);
        return false;
    }

    /* Run as the account that authenticated, not as pid 0's root.
     * Safe to set here: this is a cooperative kernel, we are on pid 0,
     * and the child exists but has not been scheduled yet. Without this
     * every SSH login would silently be root. */
    {
        struct proc *cp = proc_lookup(pid);
        const struct user *u = users_lookup_by_name(g_auth_user);
        if (cp && u) { cp->uid = u->uid; cp->gid = u->gid; }
    }

    g_chan.ptym = m;
    g_chan.pty_idx = idx;
    g_chan.child_pid = pid;
    kprintf("[ssh] /bin/tsh pid=%d on pty%d for '%s'%s\n",
            pid, idx, g_auth_user, g_chan.exec_mode ? " (exec)" : "");
    return true;
}

/* Move shell output to the client, and notice when the shell is gone.
 *
 * pty_master_read_nb separates "nothing yet" (-2) from "the slave end is
 * fully closed" (0). That distinction is the whole exit path: the child
 * holds the only three slave references, so when it exits they drop to zero
 * and this read reports EOF. That works for an interactive shell as well as
 * for exec, which the old term-session version could not do -- it only ever
 * closed the channel in exec mode, because an empty ring and a finished
 * command looked identical to it. */
static void ssh_drain_channel_output(void) {
    if (!g_cli || !g_chan.active || !g_chan.ptym || g_chan.close_sent)
        return;

    for (int rounds = 0; rounds < 4; rounds++) {
        if (g_chan.remote_window == 0) return;

        /* Throttle on our OWN buffer as well as the peer's window. Reading
         * the pty faster than the wire drains would overflow g_tx, and a
         * dropped SSH packet is unrecoverable. Leave headroom for the packet
         * framing ssh_send_channel_data adds around the payload. */
        size_t room = ssh_tx_space();
        if (room < 1024u) return;
        char out[512];
        size_t cap = sizeof(out);
        if (cap > room - 512u) cap = room - 512u;
        if (cap > g_chan.remote_window) cap = g_chan.remote_window;
        if (cap > g_chan.remote_max_packet && g_chan.remote_max_packet > 0)
            cap = g_chan.remote_max_packet;
        if (cap == 0) return;

        long n = pty_master_read_nb(g_chan.ptym, out, cap);
        if (n == -2) return;                    /* nothing buffered yet */
        if (n < 0) {                            /* bad handle: give up */
            (void)ssh_send_channel_close(g_cli);
            ssh_channel_teardown();
            g_chan.active = false;
            return;
        }
        if (n == 0) {
            /* The shell exited and the pty drained. Report its status and
             * close, whether this was `ssh host cmd` or an interactive
             * session the user typed `exit` into. */
            int code = 0;
            struct proc *cp = (g_chan.child_pid > 0)
                            ? proc_lookup(g_chan.child_pid) : 0;
            if (cp) code = cp->exit_code;
            kprintf("[ssh] session shell pid=%d finished rc=%d\n",
                    g_chan.child_pid, code);
            if (!ssh_send_channel_exit_status(g_cli, (uint32_t)code) ||
                !ssh_send_channel_eof(g_cli) ||
                !ssh_send_channel_close(g_cli)) {
                ssh_close_client();
                return;
            }
            ssh_channel_teardown();
            g_chan.active = false;
            return;
        }

        if (!ssh_send_channel_data(g_cli, out, (size_t)n)) {
            ssh_close_client();
            return;
        }
        g_chan.remote_window -= (uint32_t)n;
    }
}

static void ssh_handle_packet(const uint8_t *payload, size_t payload_len) {
    if (payload_len == 0) return;

    uint8_t msg = payload[0];
    kprintf("[ssh] packet seq=%u msg=%u payload=%u\n",
            (unsigned)g_recv_seq, (unsigned)msg, (unsigned)payload_len);
    g_recv_seq++;

    switch (g_state) {
    case SSH_WAIT_KEXINIT:
        ssh_handle_kexinit_payload(payload, payload_len);
        break;
    case SSH_WAIT_ECDH_INIT:
        ssh_handle_ecdh_init_payload(payload, payload_len);
        break;
    case SSH_WAIT_NEWKEYS:
        if (msg == SSH_MSG_NEWKEYS) {
            g_in_cipher.active = true;
            kprintf("[ssh] encrypted packet mode active; waiting service\n");
            g_state = SSH_WAIT_SERVICE_REQUEST;
        } else {
            (void)ssh_send_disconnect(g_cli, "expected SSH_MSG_NEWKEYS");
            g_state = SSH_SENT_DISCONNECT;
        }
        break;
    case SSH_WAIT_SERVICE_REQUEST:
        ssh_handle_service_request_payload(payload, payload_len);
        break;
    case SSH_WAIT_USERAUTH_REQUEST:
        ssh_handle_userauth_request_payload(payload, payload_len);
        break;
    case SSH_AUTHENTICATED:
        if (msg == SSH_MSG_GLOBAL_REQUEST) {
            ssh_handle_global_request_payload(payload, payload_len);
        } else if (msg == SSH_MSG_CHANNEL_OPEN) {
            ssh_handle_channel_open_payload(payload, payload_len);
        } else {
            (void)ssh_send_disconnect(g_cli,
                "expected SSH_MSG_CHANNEL_OPEN after userauth");
            g_state = SSH_SENT_DISCONNECT;
        }
        break;
    case SSH_CHANNEL_OPEN:
        if (msg == SSH_MSG_CHANNEL_REQUEST) {
            ssh_handle_channel_request_payload(payload, payload_len);
        } else if (msg == SSH_MSG_CHANNEL_DATA) {
            ssh_handle_channel_data_payload(payload, payload_len);
        } else if (msg == SSH_MSG_CHANNEL_WINDOW_ADJUST ||
                   msg == SSH_MSG_CHANNEL_EOF ||
                   msg == SSH_MSG_CHANNEL_CLOSE) {
            ssh_handle_channel_control_payload(payload, payload_len);
        } else if (msg == SSH_MSG_GLOBAL_REQUEST) {
            ssh_handle_global_request_payload(payload, payload_len);
        } else {
            kprintf("[ssh] ignoring channel-state msg=%u\n", (unsigned)msg);
        }
        break;
    default:
        (void)ssh_send_disconnect(g_cli, "unexpected SSH packet");
        g_state = SSH_SENT_DISCONNECT;
        break;
    }
}

static void ssh_rx_packet_bytes(const uint8_t *buf, size_t n) {
    if (n > SSH_RX_MAX - g_rx_len) {
        kprintf("[ssh] RX packet buffer overflow\n");
        ssh_close_client();
        return;
    }
    memcpy(g_rx + g_rx_len, buf, n);
    g_rx_len += n;

    for (;;) {
        if (g_rx_len < 5u) return;

        if (g_in_cipher.active) {
            uint8_t plain_len[4];
            ssh_chachapoly_len_crypt(&g_in_cipher, g_recv_seq,
                                     plain_len, g_rx);
            uint32_t packet_len = get_u32_be(plain_len);
            if (packet_len < 5u || packet_len > SSH_RX_MAX - 4u) {
                kprintf("[ssh] invalid encrypted packet_length=%u\n",
                        (unsigned)packet_len);
                ssh_close_client();
                return;
            }

            size_t total = (size_t)packet_len + 4u +
                           SSH_CRYPTO_POLY1305_TAG_LEN;
            if (g_rx_len < total) return;

            uint8_t expected_tag[SSH_CRYPTO_POLY1305_TAG_LEN];
            ssh_chachapoly_tag(&g_in_cipher, g_recv_seq,
                               g_rx, (size_t)packet_len + 4u,
                               expected_tag);
            if (!ssh_chachapoly_tag_ok(expected_tag,
                    g_rx + 4u + packet_len)) {
                kprintf("[ssh] encrypted packet MAC failure seq=%u\n",
                        (unsigned)g_recv_seq);
                ssh_close_client();
                return;
            }

            uint8_t *plain_body = g_packet_plain;
            ssh_chachapoly_payload_crypt(&g_in_cipher, g_recv_seq,
                                         plain_body, g_rx + 4u,
                                         packet_len);
            uint8_t pad_len = plain_body[0];
            if (pad_len < 4u || pad_len + 1u > packet_len) {
                kprintf("[ssh] invalid encrypted padding_length=%u "
                        "packet_length=%u\n",
                        (unsigned)pad_len, (unsigned)packet_len);
                ssh_close_client();
                return;
            }

            size_t payload_len = (size_t)packet_len - (size_t)pad_len - 1u;
            ssh_handle_packet(plain_body + 1u, payload_len);
            ssh_crypto_wipe(g_packet_plain, sizeof(g_packet_plain));
            if (!g_cli || g_state == SSH_SENT_DISCONNECT) return;

            size_t remaining = g_rx_len - total;
            if (remaining) memmove(g_rx, g_rx + total, remaining);
            g_rx_len = remaining;

            if (!g_cli || g_state == SSH_SENT_DISCONNECT) return;
            continue;
        }

        uint32_t packet_len = get_u32_be(g_rx);
        if (packet_len < 5u || packet_len > SSH_RX_MAX - 4u) {
            kprintf("[ssh] invalid packet_length=%u\n",
                    (unsigned)packet_len);
            ssh_close_client();
            return;
        }

        size_t total = (size_t)packet_len + 4u;
        if (g_rx_len < total) return;

        uint8_t pad_len = g_rx[4];
        if (pad_len < 4u || pad_len + 1u > packet_len) {
            kprintf("[ssh] invalid padding_length=%u packet_length=%u\n",
                    (unsigned)pad_len, (unsigned)packet_len);
            ssh_close_client();
            return;
        }

        size_t payload_len = (size_t)packet_len - (size_t)pad_len - 1u;
        ssh_handle_packet(g_rx + 5u, payload_len);
        if (!g_cli || g_state == SSH_SENT_DISCONNECT) return;

        size_t remaining = g_rx_len - total;
        if (remaining) memmove(g_rx, g_rx + total, remaining);
        g_rx_len = remaining;

        if (!g_cli || g_state == SSH_SENT_DISCONNECT) return;
    }
}

static void ssh_handle_line(const uint8_t *extra, size_t extra_len) {
    g_line[g_line_len] = '\0';

    if (g_line_len < 4 || memcmp(g_line, "SSH-", 4) != 0) {
        kprintf("[ssh] non-SSH client banner '%s'\n", g_line);
        ssh_close_client();
        return;
    }

    kprintf("[ssh] client banner: %s\n", g_line);

    if (!ssh_send_kexinit(g_cli)) {
        ssh_close_client();
        return;
    }
    g_state = SSH_WAIT_KEXINIT;
    if (extra_len) ssh_rx_packet_bytes(extra, extra_len);
}

void ssh_init(void) {
    if (!net_is_up() || g_my_ip == 0) return;
    if (g_lsn) return;

    if (!ssh_hostkey_ready() && ssh_hostkey_init() != 0) {
        kprintf("[ssh] host key unavailable; TCP/%u not started\n",
                (unsigned)SSH_PORT);
        return;
    }

    g_lsn = tcp_listen(htons((uint16_t)SSH_PORT), 2);
    if (!g_lsn) {
        kprintf("[ssh] listen TCP/%u failed (busy or no slot)\n",
                (unsigned)SSH_PORT);
        return;
    }

    kprintf("[ssh] listening on TCP port %u (transport scaffold)\n",
            (unsigned)SSH_PORT);
}

/* Drive the live client session. WORKER CONTEXT ONLY -- see ssh_worker().
 *
 * This body used to be ssh_poll(), called straight from net_service_tick()
 * on pid 0's idle loop, and that is what made the tsh rewrite unsafe. The
 * session does TCP waits, which drop the BKL by design (tcp_poll_until says
 * so explicitly, to avoid starving pid 0), so the tick was not atomic; and
 * it touches pty rings that the spawned shell manipulates from another CPU.
 * On -smp 4 the guest wedged at random with "pid 0 idle loop stalled", while
 * a byte-identical binary ran 6/6 clean on -smp 1. Every attempt to
 * instrument the window made it disappear.
 *
 * So the work moved off the idle loop instead of being chased. A ring-0
 * worker is an ordinary schedulable proc: if it blocks, or even wedges,
 * ONLY IT stops. pid 0 keeps beating, the watchdog stays quiet and the
 * machine stays usable. Same model as http_async.c and ws.c. */
static void ssh_service(void) {
    if (!g_lsn || !net_is_up() || !g_cli) return;

    ssh_tx_flush();

    /* No pid-0 dance needed any more: the worker IS a fixed, known parent,
     * so the shell's caps, cwd and identity no longer depend on whichever
     * process happened to make the last syscall. */
    if (g_chan.want_spawn && g_chan.active) {
        if (!ssh_spawn_shell()) {
            (void)ssh_send_channel_close(g_cli);
            g_chan.active = false;
        }
    }

    /* A real timeout is affordable here, and wanted. In the old idle-tick
     * context any wait froze the machine, so this was 0 and the loop spun;
     * on the worker tcp_yield_wait turns the wait into a yield, which paces
     * the loop and cuts round-trip latency at the same time. */
    uint8_t buf[128];
    long n = tcp_recv_quiet(g_cli, buf, sizeof(buf), 20);
    if (n > 0) {
        if (g_state == SSH_WAIT_IDENT) {
            for (long i = 0; i < n; i++) {
                char ch = (char)buf[i];
                if (ch == '\r') continue;
                if (ch == '\n') {
                    size_t used = (size_t)i + 1u;
                    ssh_handle_line(buf + used, (size_t)n - used);
                    break;
                }
                if (g_line_len + 1u >= SSH_LINE_MAX) {
                    kprintf("[ssh] client banner too long\n");
                    ssh_close_client();
                    break;
                }
                g_line[g_line_len++] = ch;
            }
        } else {
            ssh_rx_packet_bytes(buf, (size_t)n);
        }
        ssh_drain_channel_output();
        return;
    }

    ssh_drain_channel_output();

    if (n < 0) {
        kprintf("[ssh] client disconnected/error\n");
        ssh_close_client();
        return;
    }

    tcp_state_t st = tcp_state(g_cli);
    if (g_state == SSH_SENT_DISCONNECT ||
        (st != TCP_ESTABLISHED && st != TCP_CLOSE_WAIT)) {
        ssh_close_client();
    }
}

/* The ring-0 worker. Spawned on the first client and then long-lived. */
static void ssh_worker(void) {
    /* First entry arrives through the context switch with no BKL. */
    if (!bkl_held()) bkl_enter();
    struct proc *self = current_proc();
    if (self) self->tcp_yield_wait = true;   /* yield, never hlt, in tcp waits */
    kprintf("[ssh] worker up (pid=%d)\n", self ? self->pid : -1);

    for (;;) {
        ssh_service();
        /* Never yield holding the BKL. */
        bkl_exit();
        sched_yield();
        bkl_enter();
    }
}

/* Tick context (net_service_tick, pid 0). Deliberately does ONE cheap,
 * non-blocking thing: notice a new connection and hand it to the worker.
 * Nothing here may block or touch a pty. */
void ssh_poll(void) {
    if (!g_lsn || !net_is_up()) return;

    if (g_cli) {
        /* Already serving one session -- this server has exactly one.
         *
         * Do NOT just leave the next connection sitting in the backlog: the
         * peer has already completed the TCP handshake, so from its side it
         * is CONNECTED and waiting for a banner that will never arrive. It
         * then hangs for its whole timeout ("Connection timed out during
         * banner exchange"), which is what two sessions in quick succession
         * looked like -- the second arrives while the first is still tearing
         * down. Close it instead: the client gets an immediate, honest
         * failure it can retry, rather than a stall. */
        struct tcp_conn *extra = tcp_accept(g_lsn, 0);
        if (extra) {
            static const char busy[] =
                "SSH-2.0-tobyOS_busy\r\nonly one session at a time\r\n";
            (void)tcp_send_nb(extra, busy, sizeof busy - 1);
            tcp_close_nowait(extra);
            kprintf("[ssh] refused a second connection (one session at a "
                    "time)\n");
        }
        return;
    }

    struct tcp_conn *c = tcp_accept(g_lsn, 0);
    if (!c) return;

    g_state = SSH_WAIT_IDENT;
    g_line_len = 0;
    g_line[0] = '\0';
    g_rx_len = 0;
    g_client_kexinit_len = 0;
    g_server_kexinit_len = 0;
    g_have_session_id = false;
    memset(g_session_id, 0, sizeof(g_session_id));
    memset(&g_in_cipher, 0, sizeof(g_in_cipher));
    memset(&g_out_cipher, 0, sizeof(g_out_cipher));
    memset(g_auth_user, 0, sizeof(g_auth_user));
    memset(&g_chan, 0, sizeof(g_chan));
    g_chan.pty_idx = -1;
    g_chan.child_pid = -1;
    g_tx_head = g_tx_tail = 0;
    g_send_seq = 0;
    g_recv_seq = 0;
    g_cli = c;

    kprintf("[ssh] client connected\n");
    if (!ssh_send_all(g_cli, SSH_IDENT, strlen(SSH_IDENT))) {
        ssh_close_client();
        return;
    }

    if (g_worker_pid < 0) {
        g_worker_pid = proc_create_kernel(ssh_worker, "sshd");
        if (g_worker_pid < 0)
            kprintf("[ssh] could not start worker; session dropped\n");
    }
}
