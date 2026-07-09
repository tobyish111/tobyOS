/* tls.c -- Minimal TLS 1.3 client (ChaCha20-Poly1305 + X25519).
 *
 * Implements just enough of RFC 8446 to establish encrypted connections
 * to modern HTTPS servers. Uses tobyOS's existing TCP stack underneath
 * and Monocypher for cryptographic primitives.
 *
 * Cipher suite: TLS_CHACHA20_POLY1305_SHA256 (0x1303)
 * Key exchange: X25519 (group 0x001D)
 *
 * Certificate validation is SKIPPED (all certs accepted). This is
 * appropriate for a hobby OS demo but not for production use.
 */

#include <tobyos/tls.h>
#include <tobyos/tcp.h>
#include <tobyos/sec.h>
#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>
#include <tobyos/rng.h>
#include <tobyos/rtc.h>

#include "monocypher.h"
#include <bearssl.h>       /* stage 13H: X.509 chain validation */

/* Trust anchors (Mozilla roots) from src/tls_trust.c. */
const br_x509_trust_anchor *tls_trust_anchors(size_t *count);

/* TLS randomness comes from the kernel CSPRNG (rng.c), which feature-
 * detects RDRAND via CPUID. The previous private helper here executed a
 * raw RDRAND unconditionally -- an instant #UD kernel panic on any CPU
 * without the instruction (QEMU's default qemu64 model; first hit when
 * the browser went HTTPS-first) -- and substituted hardcoded constants
 * when RDRAND reported failure, which is not randomness at all. */
static void random_bytes(uint8_t *buf, size_t n) {
    rng_fill(buf, n);
}

/* ---- TLS record types and constants ----------------------------- */

#define TLS_RT_CHANGE_CIPHER  20
#define TLS_RT_ALERT          21
#define TLS_RT_HANDSHAKE      22
#define TLS_RT_APPLICATION    23

#define TLS_HS_CLIENT_HELLO    1
#define TLS_HS_SERVER_HELLO    2
#define TLS_HS_NEW_SESSION     4
#define TLS_HS_ENCRYPTED_EXT   8
#define TLS_HS_CERTIFICATE    11
#define TLS_HS_CERT_VERIFY    15
#define TLS_HS_FINISHED       20

#define TLS_EXT_SNI                 0x0000
#define TLS_EXT_SUPPORTED_GROUPS    0x000A
#define TLS_EXT_SIGNATURE_ALGOS     0x000D
#define TLS_EXT_ALPN                0x0010
#define TLS_EXT_SUPPORTED_VERSIONS  0x002B
#define TLS_EXT_KEY_SHARE           0x0033

#define TLS_VERSION_12  0x0303
#define TLS_VERSION_13  0x0304

#define TLS_GROUP_X25519  0x001D

#define TLS_MAX_RECORD  16384 + 256

/* ---- Connection state ------------------------------------------- */

struct tls_conn {
    struct tcp_conn *tcp;
    uint32_t timeout_ms;

    /* Handshake transcript hash (rolling SHA-256) */
    struct sha256_ctx transcript;

    /* Traffic keys */
    uint8_t client_key[32];
    uint8_t server_key[32];
    uint8_t client_iv[12];
    uint8_t server_iv[12];

    /* Sequence numbers for nonce construction */
    uint64_t client_seq;
    uint64_t server_seq;

    /* State flags */
    int handshake_done;
    int closed;

    /* ALPN (stage 13G): offer_h2 advertises ["h2","http/1.1"] in the
     * ClientHello; alpn holds what the server selected in
     * EncryptedExtensions ("" if none / not offered). */
    int  offer_h2;
    char alpn[16];

    /* Receive buffer for partial records */
    uint8_t  rx_buf[TLS_MAX_RECORD + 5];
    size_t   rx_len;

    /* Decrypted app data buffer */
    uint8_t  app_buf[TLS_MAX_RECORD];
    size_t   app_len;
    size_t   app_off;
};

/* ---- HKDF (RFC 5869, SHA-256) ----------------------------------- */

static void hmac_sha256(const uint8_t *key, size_t key_len,
                        const uint8_t *msg, size_t msg_len,
                        uint8_t out[32]) {
    struct hmac_sha256_ctx ctx;
    hmac_sha256_init(&ctx, key, key_len);
    hmac_sha256_update(&ctx, msg, msg_len);
    hmac_sha256_final(&ctx, out);
}

static void hkdf_extract(const uint8_t *salt, size_t salt_len,
                         const uint8_t *ikm, size_t ikm_len,
                         uint8_t out[32]) {
    if (!salt || salt_len == 0) {
        uint8_t zero_salt[32];
        memset(zero_salt, 0, 32);
        hmac_sha256(zero_salt, 32, ikm, ikm_len, out);
    } else {
        hmac_sha256(salt, salt_len, ikm, ikm_len, out);
    }
}

static void hkdf_expand(const uint8_t prk[32],
                        const uint8_t *info, size_t info_len,
                        uint8_t *out, size_t out_len) {
    uint8_t t[32];
    size_t  t_len = 0;
    uint8_t counter = 1;
    size_t  done = 0;

    while (done < out_len) {
        struct hmac_sha256_ctx ctx;
        hmac_sha256_init(&ctx, prk, 32);
        if (t_len > 0) hmac_sha256_update(&ctx, t, t_len);
        hmac_sha256_update(&ctx, info, info_len);
        hmac_sha256_update(&ctx, &counter, 1);
        hmac_sha256_final(&ctx, t);
        t_len = 32;

        size_t copy = out_len - done;
        if (copy > 32) copy = 32;
        memcpy(out + done, t, copy);
        done += copy;
        counter++;
    }
}

/* TLS 1.3 HKDF-Expand-Label:
 * HKDF-Expand(Secret, HkdfLabel, Length) where HkdfLabel is:
 *   uint16 length
 *   opaque label<7..255> = "tls13 " + Label
 *   opaque context<0..255> = Hash.value
 */
static void hkdf_expand_label(const uint8_t secret[32],
                              const char *label, size_t label_len,
                              const uint8_t *context, size_t ctx_len,
                              uint8_t *out, size_t out_len) {
    uint8_t info[256];
    size_t pos = 0;

    /* uint16 length */
    info[pos++] = (uint8_t)(out_len >> 8);
    info[pos++] = (uint8_t)(out_len);

    /* label length (including "tls13 " prefix) */
    size_t full_label_len = 6 + label_len;
    info[pos++] = (uint8_t)full_label_len;

    /* "tls13 " prefix */
    info[pos++] = 't'; info[pos++] = 'l'; info[pos++] = 's';
    info[pos++] = '1'; info[pos++] = '3'; info[pos++] = ' ';

    /* actual label */
    memcpy(info + pos, label, label_len);
    pos += label_len;

    /* context length + context */
    info[pos++] = (uint8_t)ctx_len;
    if (ctx_len > 0) {
        memcpy(info + pos, context, ctx_len);
        pos += ctx_len;
    }

    hkdf_expand(secret, info, pos, out, out_len);
}

/* Derive-Secret(Secret, Label, Messages) = HKDF-Expand-Label(Secret, Label, Hash(Messages), 32) */
static void derive_secret(const uint8_t secret[32],
                          const char *label, size_t label_len,
                          const uint8_t transcript_hash[32],
                          uint8_t out[32]) {
    hkdf_expand_label(secret, label, label_len, transcript_hash, 32, out, 32);
}

/* ---- Wire helpers ----------------------------------------------- */

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_u24(uint8_t *p, uint32_t v) { p[0]=(uint8_t)(v>>16); p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)v; }
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get_u24(const uint8_t *p) { return ((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2]; }

/* ---- RFC 8439 IETF ChaCha20-Poly1305 ----------------------------- *
 * TLS_CHACHA20_POLY1305_SHA256 uses the IETF AEAD with a 12-byte
 * per-record nonce. Monocypher's crypto_aead_lock/unlock are the
 * XChaCha20 variant taking a nonce[24] -- feeding them the 12-byte TLS
 * nonce both overread the stack and computed a cipher no TLS peer
 * speaks (every record decrypt failed against real servers). Compose
 * the IETF construction from the low-level primitives instead. */

static void tls_poly1305_aead_mac(uint8_t mac[16], const uint8_t poly_key[32],
                                  const uint8_t *ad, size_t ad_len,
                                  const uint8_t *ct, size_t ct_len) {
    static const uint8_t zeros[16] = {0};
    crypto_poly1305_ctx ctx;
    crypto_poly1305_init(&ctx, poly_key);
    crypto_poly1305_update(&ctx, ad, ad_len);
    if (ad_len % 16) crypto_poly1305_update(&ctx, zeros, 16 - ad_len % 16);
    crypto_poly1305_update(&ctx, ct, ct_len);
    if (ct_len % 16) crypto_poly1305_update(&ctx, zeros, 16 - ct_len % 16);
    uint8_t lens[16];
    for (int i = 0; i < 8; i++) {
        lens[i]     = (uint8_t)((uint64_t)ad_len >> (8 * i));
        lens[8 + i] = (uint8_t)((uint64_t)ct_len >> (8 * i));
    }
    crypto_poly1305_update(&ctx, lens, 16);
    crypto_poly1305_final(&ctx, mac);
}

static void tls_aead_encrypt(uint8_t *ct, uint8_t mac[16],
                             const uint8_t key[32], const uint8_t nonce[12],
                             const uint8_t *ad, size_t ad_len,
                             const uint8_t *plain, size_t len) {
    uint8_t block0[64];
    crypto_chacha20_ietf(block0, NULL, 64, key, nonce, 0);   /* poly key */
    crypto_chacha20_ietf(ct, plain, len, key, nonce, 1);
    tls_poly1305_aead_mac(mac, block0, ad, ad_len, ct, len);
    crypto_wipe(block0, sizeof(block0));
}

/* Returns 0 on success, -1 on MAC mismatch (same contract as
 * crypto_aead_unlock). */
static int tls_aead_decrypt(uint8_t *plain, const uint8_t mac[16],
                            const uint8_t key[32], const uint8_t nonce[12],
                            const uint8_t *ad, size_t ad_len,
                            const uint8_t *ct, size_t len) {
    uint8_t block0[64];
    crypto_chacha20_ietf(block0, NULL, 64, key, nonce, 0);
    uint8_t expect[16];
    tls_poly1305_aead_mac(expect, block0, ad, ad_len, ct, len);
    crypto_wipe(block0, sizeof(block0));
    if (crypto_verify16(expect, mac) != 0) return -1;
    crypto_chacha20_ietf(plain, ct, len, key, nonce, 1);
    return 0;
}

/* ---- Record I/O ------------------------------------------------- */

static long tls_send_record(struct tls_conn *c, uint8_t type,
                            const uint8_t *data, size_t len) {
    uint8_t hdr[5];
    hdr[0] = type;
    put_u16(hdr + 1, TLS_VERSION_12);
    put_u16(hdr + 3, (uint16_t)len);

    long s = tcp_send(c->tcp, hdr, 5);
    if (s != 5) return TLS_ERR_SEND;
    if (len > 0) {
        s = tcp_send(c->tcp, data, len);
        if (s != (long)len) return TLS_ERR_SEND;
    }
    return (long)(5 + len);
}

static long tls_send_encrypted(struct tls_conn *c, uint8_t inner_type,
                               const uint8_t *plain, size_t plain_len) {
    /* TLS 1.3 encrypted record:
     * content_type = application_data (23)
     * legacy_version = 0x0303
     * length = plaintext + 1 (content type) + 16 (tag)
     * encrypted: [plaintext | content_type_byte] + 16-byte tag */
    size_t payload_len = plain_len + 1;  /* +1 for inner content type */
    size_t record_payload = payload_len + 16;  /* +16 for poly1305 tag */

    uint8_t *buf = (uint8_t *)kmalloc(record_payload + 5);
    if (!buf) return TLS_ERR_NOMEM;

    /* Build the plaintext to encrypt: data + content_type byte */
    uint8_t *ptxt = (uint8_t *)kmalloc(payload_len);
    if (!ptxt) { kfree(buf); return TLS_ERR_NOMEM; }
    if (plain_len > 0) memcpy(ptxt, plain, plain_len);
    ptxt[plain_len] = inner_type;

    /* Nonce: XOR sequence number into IV */
    uint8_t nonce[12];
    memcpy(nonce, c->client_iv, 12);
    for (int i = 0; i < 8; i++)
        nonce[4 + i] ^= (uint8_t)(c->client_seq >> (56 - 8*i));

    /* Additional data: record header */
    uint8_t ad[5];
    ad[0] = TLS_RT_APPLICATION;
    put_u16(ad + 1, TLS_VERSION_12);
    put_u16(ad + 3, (uint16_t)record_payload);

    /* Encrypt */
    tls_aead_encrypt(buf + 5, buf + 5 + payload_len,  /* ciphertext, mac */
                     c->client_key, nonce,
                     ad, 5,
                     ptxt, payload_len);

    /* Record header */
    memcpy(buf, ad, 5);

    c->client_seq++;

    long s = tcp_send(c->tcp, buf, 5 + record_payload);
    kfree(ptxt);
    kfree(buf);
    return (s == (long)(5 + record_payload)) ? (long)plain_len : TLS_ERR_SEND;
}

/* Read a full TLS record from TCP. Returns content type, fills *out_data, *out_len.
 * A TCP FIN between records returns TLS_ERR_CLOSED: many servers (Google's
 * GFE among them) end a Connection:-close response with a bare FIN and no
 * close_notify alert. Conflating that with TLS_ERR_RECV made http.c report
 * every completed no-Content-Length HTTPS body as a TIMEOUT and discard it. */
static int tls_read_record(struct tls_conn *c, uint8_t *out_type,
                           uint8_t **out_data, size_t *out_len) {
    /* Read 5-byte header */
    uint8_t hdr[5];
    size_t hdr_got = 0;
    while (hdr_got < 5) {
        long n = tcp_recv(c->tcp, hdr + hdr_got, 5 - hdr_got, c->timeout_ms);
        if (n == -1) return TLS_ERR_CLOSED;   /* FIN (clean between records) */
        if (n == -2) return TLS_ERR_CLOSED;   /* RST -- treat as abrupt close */
        if (n <= 0) return TLS_ERR_RECV;      /* timeout */
        hdr_got += (size_t)n;
    }

    *out_type = hdr[0];
    uint16_t rec_len = get_u16(hdr + 3);
    if (rec_len > TLS_MAX_RECORD) return TLS_ERR_RECORD;

    uint8_t *data = (uint8_t *)kmalloc(rec_len);
    if (!data) return TLS_ERR_NOMEM;

    size_t got = 0;
    while (got < rec_len) {
        long n = tcp_recv(c->tcp, data + got, rec_len - got, c->timeout_ms);
        if (n == -1 || n == -2) { kfree(data); return TLS_ERR_CLOSED; }
        if (n <= 0) { kfree(data); return TLS_ERR_RECV; }
        got += (size_t)n;
    }

    *out_data = data;
    *out_len = rec_len;
    return TLS_OK;
}

/* Decrypt an encrypted record (type 23 wrapper). Returns inner content type. */
static int tls_decrypt_record(struct tls_conn *c, uint8_t *data, size_t len,
                              uint8_t *inner_type, uint8_t **out_plain, size_t *out_len) {
    if (len < 17) return TLS_ERR_RECORD;  /* at least 1 byte + 16 tag */

    size_t ciphertext_len = len - 16;
    uint8_t *mac = data + ciphertext_len;

    /* Nonce: XOR sequence number into server IV */
    uint8_t nonce[12];
    memcpy(nonce, c->server_iv, 12);
    for (int i = 0; i < 8; i++)
        nonce[4 + i] ^= (uint8_t)(c->server_seq >> (56 - 8*i));

    /* Additional data */
    uint8_t ad[5];
    ad[0] = TLS_RT_APPLICATION;
    put_u16(ad + 1, TLS_VERSION_12);
    put_u16(ad + 3, (uint16_t)len);

    uint8_t *plain = (uint8_t *)kmalloc(ciphertext_len);
    if (!plain) return TLS_ERR_NOMEM;

    if (tls_aead_decrypt(plain, mac, c->server_key, nonce,
                           ad, 5, data, ciphertext_len) != 0) {
        kfree(plain);
        return TLS_ERR_RECORD;
    }

    c->server_seq++;

    /* Strip padding zeros and find inner content type (last non-zero byte) */
    size_t plen = ciphertext_len;
    while (plen > 0 && plain[plen - 1] == 0) plen--;
    if (plen == 0) { kfree(plain); return TLS_ERR_RECORD; }

    *inner_type = plain[plen - 1];
    *out_plain = plain;
    *out_len = plen - 1;
    return TLS_OK;
}

/* ---- ClientHello construction ----------------------------------- */

static size_t build_client_hello(uint8_t *buf, size_t cap,
                                 const uint8_t client_random[32],
                                 const uint8_t pubkey[32],
                                 const char *hostname,
                                 int offer_h2) {
    size_t pos = 0;
    size_t hostname_len = 0;
    if (hostname) while (hostname[hostname_len]) hostname_len++;

    /* RFC 6066: literal IP addresses are not permitted in SNI; strict
     * servers reject them. Send no SNI for a dotted-quad host. */
    if (hostname_len > 0) {
        int only_ip = 1;
        for (size_t i = 0; i < hostname_len; i++) {
            char ch = hostname[i];
            if (!((ch >= '0' && ch <= '9') || ch == '.')) { only_ip = 0; break; }
        }
        if (only_ip) hostname_len = 0;
    }

    /* We'll build the handshake message body first, then wrap it.
     * Start after the 4-byte handshake header placeholder. */
    size_t body_start = 4;
    pos = body_start;

    /* client_version = TLS 1.2 (legacy) */
    put_u16(buf + pos, TLS_VERSION_12); pos += 2;

    /* random[32] */
    memcpy(buf + pos, client_random, 32); pos += 32;

    /* session_id: empty (TLS 1.3) */
    buf[pos++] = 0;

    /* cipher_suites: just TLS_CHACHA20_POLY1305_SHA256 (0x1303) */
    put_u16(buf + pos, 2); pos += 2;  /* length */
    put_u16(buf + pos, 0x1303); pos += 2;

    /* compression_methods: null only */
    buf[pos++] = 1;   /* length */
    buf[pos++] = 0;   /* null */

    /* Extensions */
    size_t ext_len_pos = pos;
    pos += 2;  /* placeholder for extensions length */

    /* Extension: supported_versions (mandatory for TLS 1.3) */
    put_u16(buf + pos, TLS_EXT_SUPPORTED_VERSIONS); pos += 2;
    put_u16(buf + pos, 3); pos += 2;  /* ext data length */
    buf[pos++] = 2;                    /* list length */
    put_u16(buf + pos, TLS_VERSION_13); pos += 2;

    /* Extension: supported_groups */
    put_u16(buf + pos, TLS_EXT_SUPPORTED_GROUPS); pos += 2;
    put_u16(buf + pos, 4); pos += 2;  /* ext data length */
    put_u16(buf + pos, 2); pos += 2;  /* named_group_list length */
    put_u16(buf + pos, TLS_GROUP_X25519); pos += 2;

    /* Extension: key_share (X25519 public key). RFC 8446 4.2.8: the
     * KeyShareEntry is group(2) + key_exchange length(2) + key(32) = 36
     * bytes, so client_shares = 36 and ext data = 38. These were written
     * two bytes short (34/36) -- spec-strict servers (OpenSSL, Cloudflare)
     * answered every ClientHello with a fatal decode_error(50) alert. */
    put_u16(buf + pos, TLS_EXT_KEY_SHARE); pos += 2;
    put_u16(buf + pos, 38); pos += 2; /* ext data length */
    put_u16(buf + pos, 36); pos += 2; /* client_shares length */
    put_u16(buf + pos, TLS_GROUP_X25519); pos += 2;
    put_u16(buf + pos, 32); pos += 2; /* key_exchange length */
    memcpy(buf + pos, pubkey, 32); pos += 32;

    /* Extension: signature_algorithms (required). Advertise exactly the
     * schemes our CertificateVerify path verifies (stage 13H): ECDSA
     * P-256/P-384 and RSA-PSS SHA-256/384 -- the forms TLS 1.3 uses. */
    put_u16(buf + pos, TLS_EXT_SIGNATURE_ALGOS); pos += 2;
    put_u16(buf + pos, 10); pos += 2;  /* ext data length */
    put_u16(buf + pos, 8); pos += 2;   /* list length */
    put_u16(buf + pos, 0x0403); pos += 2;  /* ecdsa_secp256r1_sha256 */
    put_u16(buf + pos, 0x0503); pos += 2;  /* ecdsa_secp384r1_sha384 */
    put_u16(buf + pos, 0x0804); pos += 2;  /* rsa_pss_rsae_sha256 */
    put_u16(buf + pos, 0x0805); pos += 2;  /* rsa_pss_rsae_sha384 */

    /* Extension: SNI (server_name) if hostname provided */
    if (hostname_len > 0) {
        put_u16(buf + pos, TLS_EXT_SNI); pos += 2;
        uint16_t sni_ext_len = (uint16_t)(hostname_len + 5);
        put_u16(buf + pos, sni_ext_len); pos += 2;
        put_u16(buf + pos, (uint16_t)(hostname_len + 3)); pos += 2;  /* server_name_list length */
        buf[pos++] = 0;  /* host_name type */
        put_u16(buf + pos, (uint16_t)hostname_len); pos += 2;
        memcpy(buf + pos, hostname, hostname_len); pos += hostname_len;
    }

    /* Extension: ALPN (RFC 7301) -- advertise HTTP/2 then HTTP/1.1 so the
     * server can pick h2. Only when the caller can speak h2 (offer_h2);
     * otherwise the byte layout is identical to the proven h1.1 path. */
    if (offer_h2) {
        put_u16(buf + pos, TLS_EXT_ALPN); pos += 2;
        put_u16(buf + pos, 14); pos += 2;   /* ext data length */
        put_u16(buf + pos, 12); pos += 2;   /* ProtocolNameList length */
        buf[pos++] = 2; buf[pos++] = 'h'; buf[pos++] = '2';
        buf[pos++] = 8;
        memcpy(buf + pos, "http/1.1", 8); pos += 8;
    }

    /* Patch extensions length */
    put_u16(buf + ext_len_pos, (uint16_t)(pos - ext_len_pos - 2));

    /* Handshake header */
    buf[0] = TLS_HS_CLIENT_HELLO;
    put_u24(buf + 1, (uint32_t)(pos - body_start));

    (void)cap;
    return pos;
}

/* ---- Handshake processing --------------------------------------- */

static int parse_server_hello(const uint8_t *data, size_t len,
                              uint8_t server_random[32],
                              uint8_t server_pubkey[32],
                              int *is_tls13) {
    if (len < 38) return TLS_ERR_HANDSHAKE;

    size_t pos = 0;

    /* server_version (legacy: 0x0303) */
    pos += 2;

    /* random */
    memcpy(server_random, data + pos, 32); pos += 32;

    /* session_id */
    uint8_t sid_len = data[pos++];
    pos += sid_len;

    /* cipher_suite */
    if (pos + 2 > len) return TLS_ERR_HANDSHAKE;
    uint16_t cipher = get_u16(data + pos); pos += 2;
    if (cipher != 0x1303) {
        kprintf("[tls] server chose unsupported cipher 0x%04x\n", cipher);
        return TLS_ERR_HANDSHAKE;
    }

    /* compression_method */
    if (pos >= len) return TLS_ERR_HANDSHAKE;
    pos++;  /* must be 0 */

    /* Extensions */
    *is_tls13 = 0;
    server_pubkey[0] = 0;

    if (pos + 2 <= len) {
        uint16_t ext_total = get_u16(data + pos); pos += 2;
        size_t ext_end = pos + ext_total;
        if (ext_end > len) ext_end = len;

        while (pos + 4 <= ext_end) {
            uint16_t ext_type = get_u16(data + pos); pos += 2;
            uint16_t ext_len = get_u16(data + pos); pos += 2;
            if (pos + ext_len > ext_end) break;

            if (ext_type == TLS_EXT_SUPPORTED_VERSIONS) {
                if (ext_len >= 2) {
                    uint16_t ver = get_u16(data + pos);
                    if (ver == TLS_VERSION_13) *is_tls13 = 1;
                }
            } else if (ext_type == TLS_EXT_KEY_SHARE) {
                if (ext_len >= 36) {
                    /* uint16 group, uint16 key_len, key[32] */
                    uint16_t group = get_u16(data + pos);
                    uint16_t klen = get_u16(data + pos + 2);
                    if (group == TLS_GROUP_X25519 && klen == 32) {
                        memcpy(server_pubkey, data + pos + 4, 32);
                    }
                }
            }
            pos += ext_len;
        }
    }

    if (!*is_tls13) return TLS_ERR_VERSION;
    return TLS_OK;
}

/* Derive handshake traffic keys from the shared secret */
static void derive_handshake_keys(const uint8_t shared_secret[32],
                                  const uint8_t transcript_hash[32],
                                  uint8_t client_hs_key[32], uint8_t client_hs_iv[12],
                                  uint8_t server_hs_key[32], uint8_t server_hs_iv[12],
                                  uint8_t handshake_secret[32]) {
    /* early_secret = HKDF-Extract(salt=0, IKM=0) */
    uint8_t zero_ikm[32];
    memset(zero_ikm, 0, 32);
    uint8_t early_secret[32];
    hkdf_extract(NULL, 0, zero_ikm, 32, early_secret);

    /* derived_secret = Derive-Secret(early_secret, "derived", Hash("")) */
    uint8_t empty_hash[32];
    sha256_buf(NULL, 0, empty_hash);
    uint8_t derived[32];
    derive_secret(early_secret, "derived", 7, empty_hash, derived);

    /* handshake_secret = HKDF-Extract(derived, shared_secret) */
    hkdf_extract(derived, 32, shared_secret, 32, handshake_secret);

    /* client_handshake_traffic_secret */
    uint8_t c_hs_secret[32];
    derive_secret(handshake_secret, "c hs traffic", 12, transcript_hash, c_hs_secret);

    /* server_handshake_traffic_secret */
    uint8_t s_hs_secret[32];
    derive_secret(handshake_secret, "s hs traffic", 12, transcript_hash, s_hs_secret);

    /* Derive keys and IVs */
    hkdf_expand_label(c_hs_secret, "key", 3, NULL, 0, client_hs_key, 32);
    hkdf_expand_label(c_hs_secret, "iv", 2, NULL, 0, client_hs_iv, 12);
    hkdf_expand_label(s_hs_secret, "key", 3, NULL, 0, server_hs_key, 32);
    hkdf_expand_label(s_hs_secret, "iv", 2, NULL, 0, server_hs_iv, 12);
}

/* Derive application traffic keys */
static void derive_app_keys(const uint8_t handshake_secret[32],
                            const uint8_t transcript_hash[32],
                            uint8_t client_app_key[32], uint8_t client_app_iv[12],
                            uint8_t server_app_key[32], uint8_t server_app_iv[12]) {
    /* derived = Derive-Secret(handshake_secret, "derived", Hash("")) */
    uint8_t empty_hash[32];
    sha256_buf(NULL, 0, empty_hash);
    uint8_t derived[32];
    derive_secret(handshake_secret, "derived", 7, empty_hash, derived);

    /* master_secret = HKDF-Extract(derived, 0) */
    uint8_t zero_ikm[32];
    memset(zero_ikm, 0, 32);
    uint8_t master_secret[32];
    hkdf_extract(derived, 32, zero_ikm, 32, master_secret);

    /* traffic secrets */
    uint8_t c_app_secret[32], s_app_secret[32];
    derive_secret(master_secret, "c ap traffic", 12, transcript_hash, c_app_secret);
    derive_secret(master_secret, "s ap traffic", 12, transcript_hash, s_app_secret);

    /* keys and IVs */
    hkdf_expand_label(c_app_secret, "key", 3, NULL, 0, client_app_key, 32);
    hkdf_expand_label(c_app_secret, "iv", 2, NULL, 0, client_app_iv, 12);
    hkdf_expand_label(s_app_secret, "key", 3, NULL, 0, server_app_key, 32);
    hkdf_expand_label(s_app_secret, "iv", 2, NULL, 0, server_app_iv, 12);
}

/* Compute Finished verify_data */
static void compute_finished(const uint8_t base_key[32],
                             const uint8_t transcript_hash[32],
                             uint8_t out[32]) {
    uint8_t finished_key[32];
    hkdf_expand_label(base_key, "finished", 8, NULL, 0, finished_key, 32);
    hmac_sha256(finished_key, 32, transcript_hash, 32, out);
}

/* ---- Main handshake --------------------------------------------- */

/* ---- stage 13H: certificate chain validation ----------------------- *
 * X.509 chain validation (BearSSL x509_minimal) proves the leaf chains to
 * a trusted root, matches the hostname (SAN/CN) and is unexpired. But that
 * ALONE is worthless: a MITM can replay any real chain. The binding to the
 * live handshake is CertificateVerify -- the server signs the transcript
 * with the leaf's private key. We do both: validate the chain to recover
 * the leaf public key, then verify CertificateVerify with it. Fail closed. */

/* Validate the TLS 1.3 Certificate message. On success copies the leaf
 * public key (stable) into out_pk/pk_buf. Returns 0 or a TLS_ERR_*. */
static int tls_validate_chain(const uint8_t *msg, size_t msglen,
                              const char *hostname,
                              br_x509_pkey *out_pk,
                              unsigned char *pk_buf, size_t pk_buf_cap) {
    if (msglen < 4) return TLS_ERR_CERT;
    size_t p = 0;
    size_t ctx_len = msg[p];              /* certificate_request_context */
    p += 1 + ctx_len;
    if (p + 3 > msglen) return TLS_ERR_CERT;
    size_t list_len = get_u24(msg + p); p += 3;
    if (p + list_len > msglen) return TLS_ERR_CERT;

    br_x509_minimal_context *xc =
        (br_x509_minimal_context *)kmalloc(sizeof(*xc));
    if (!xc) return TLS_ERR_NOMEM;

    size_t nta;
    const br_x509_trust_anchor *tas = tls_trust_anchors(&nta);
    br_x509_minimal_init(xc, &br_sha256_vtable, tas, nta);
    br_x509_minimal_set_rsa(xc, &br_rsa_i31_pkcs1_vrfy);
    br_x509_minimal_set_ecdsa(xc, &br_ec_prime_i31, &br_ecdsa_i31_vrfy_asn1);
    br_x509_minimal_set_hash(xc, br_md5_ID, &br_md5_vtable);
    br_x509_minimal_set_hash(xc, br_sha1_ID, &br_sha1_vtable);
    br_x509_minimal_set_hash(xc, br_sha224_ID, &br_sha224_vtable);
    br_x509_minimal_set_hash(xc, br_sha256_ID, &br_sha256_vtable);
    br_x509_minimal_set_hash(xc, br_sha384_ID, &br_sha384_vtable);
    br_x509_minimal_set_hash(xc, br_sha512_ID, &br_sha512_vtable);
    uint64_t ut = rtc_unix_time();
    br_x509_minimal_set_time(xc, (uint32_t)(ut / 86400) + 719528,
                                 (uint32_t)(ut % 86400));

    const br_x509_class **xcc = &xc->vtable;
    (*xcc)->start_chain(xcc, (hostname && hostname[0]) ? hostname : NULL);
    size_t q = p, end = p + list_len;
    while (q + 3 <= end) {
        size_t clen = get_u24(msg + q); q += 3;
        if (q + clen > end) break;
        (*xcc)->start_cert(xcc, (uint32_t)clen);
        (*xcc)->append(xcc, msg + q, clen);
        (*xcc)->end_cert(xcc);
        q += clen;
        if (q + 2 > end) break;
        q += 2 + get_u16(msg + q);         /* skip this entry's extensions */
    }
    unsigned err = (*xcc)->end_chain(xcc);
    int rc = TLS_ERR_CERT;
    if (err != 0) {
        kprintf("[tls] CERT chain INVALID: x509 error %u\n", err);
    } else {
        const br_x509_pkey *pk = (*xcc)->get_pkey(xcc, NULL);
        if (pk && pk->key_type == BR_KEYTYPE_RSA) {
            size_t nl = pk->key.rsa.nlen, el = pk->key.rsa.elen;
            if (nl + el <= pk_buf_cap) {
                memcpy(pk_buf, pk->key.rsa.n, nl);
                memcpy(pk_buf + nl, pk->key.rsa.e, el);
                *out_pk = *pk;
                out_pk->key.rsa.n = pk_buf;
                out_pk->key.rsa.e = pk_buf + nl;
                rc = 0;
            }
        } else if (pk && pk->key_type == BR_KEYTYPE_EC) {
            size_t ql = pk->key.ec.qlen;
            if (ql <= pk_buf_cap) {
                memcpy(pk_buf, pk->key.ec.q, ql);
                *out_pk = *pk;
                out_pk->key.ec.q = pk_buf;
                rc = 0;
            }
        }
    }
    kfree(xc);
    return rc;
}

/* Verify the TLS 1.3 CertificateVerify signature (RFC 8446 4.4.3): the
 * server signed 64*0x20 || "TLS 1.3, server CertificateVerify" || 0x00 ||
 * transcript_hash(ClientHello..Certificate) with the leaf key. Returns 1
 * if valid. Supports ECDSA (P-256/384/521) and RSA-PSS (the only RSA form
 * TLS 1.3 allows) with SHA-256/384/512. */
static int tls_verify_cv(uint16_t scheme, const uint8_t *sig, size_t siglen,
                         const uint8_t thash[32], const br_x509_pkey *pk) {
    static const char label[] = "TLS 1.3, server CertificateVerify";
    uint8_t content[64 + 33 + 1 + 32];
    memset(content, 0x20, 64);
    memcpy(content + 64, label, 33);
    content[97] = 0x00;
    memcpy(content + 98, thash, 32);

    const br_hash_class *hc;
    size_t hlen;
    switch (scheme) {
    case 0x0403: case 0x0804: hc = &br_sha256_vtable; hlen = 32; break;
    case 0x0503: case 0x0805: hc = &br_sha384_vtable; hlen = 48; break;
    case 0x0603: case 0x0806: hc = &br_sha512_vtable; hlen = 64; break;
    default:
        kprintf("[tls] CertificateVerify: unsupported scheme 0x%04x\n", scheme);
        return 0;
    }
    uint8_t digest[64];
    br_hash_compat_context hctx;
    hc->init(&hctx.vtable);
    hc->update(&hctx.vtable, content, sizeof content);
    hc->out(&hctx.vtable, digest);

    if (scheme == 0x0403 || scheme == 0x0503 || scheme == 0x0603) {
        if (pk->key_type != BR_KEYTYPE_EC) return 0;
        return (int)br_ecdsa_i31_vrfy_asn1(&br_ec_prime_i31, digest, hlen,
                                           &pk->key.ec, sig, siglen);
    }
    if (pk->key_type != BR_KEYTYPE_RSA) return 0;      /* PSS schemes */
    return (int)br_rsa_i31_pss_vrfy(sig, siglen, hc, hc, digest, hlen,
                                    &pk->key.rsa);
}

static int tls_do_handshake(struct tls_conn *c, const char *hostname) {
    uint8_t client_random[32];
    uint8_t secret_key[32];
    uint8_t public_key[32];

    random_bytes(client_random, 32);
    random_bytes(secret_key, 32);
    crypto_x25519_public_key(public_key, secret_key);

    /* Build ClientHello */
    uint8_t ch_buf[512];
    size_t ch_len = build_client_hello(ch_buf, sizeof(ch_buf),
                                       client_random, public_key, hostname,
                                       c->offer_h2);

    /* Initialize transcript with ClientHello */
    sha256_init(&c->transcript);
    sha256_update(&c->transcript, ch_buf, ch_len);

    /* Send ClientHello as a Handshake record */
    int rc = (int)tls_send_record(c, TLS_RT_HANDSHAKE, ch_buf, ch_len);
    if (rc < 0) return rc;

    /* Read ServerHello */
    uint8_t rec_type;
    uint8_t *rec_data;
    size_t rec_len;

    rc = tls_read_record(c, &rec_type, &rec_data, &rec_len);
    if (rc != TLS_OK) return rc;
    if (rec_type != TLS_RT_HANDSHAKE || rec_len < 4) {
        if (rec_type == TLS_RT_ALERT && rec_len >= 2)
            kprintf("[tls] server alert before ServerHello: level=%d desc=%d\n",
                    rec_data[0], rec_data[1]);
        else
            kprintf("[tls] expected ServerHello record, got type=%d len=%u\n",
                    rec_type, (unsigned)rec_len);
        kfree(rec_data);
        return TLS_ERR_HANDSHAKE;
    }

    /* Parse ServerHello (skip the 4-byte handshake header) */
    uint8_t server_random[32];
    uint8_t server_pubkey[32];
    int is_tls13 = 0;

    /* The record may contain just ServerHello or multiple messages */
    if (rec_data[0] != TLS_HS_SERVER_HELLO) {
        kfree(rec_data);
        return TLS_ERR_HANDSHAKE;
    }
    uint32_t sh_len = get_u24(rec_data + 1);
    if (4 + sh_len > rec_len) { kfree(rec_data); return TLS_ERR_HANDSHAKE; }

    rc = parse_server_hello(rec_data + 4, sh_len, server_random, server_pubkey, &is_tls13);
    if (rc != TLS_OK) { kfree(rec_data); return rc; }

    /* Update transcript with ServerHello */
    sha256_update(&c->transcript, rec_data, 4 + sh_len);
    kfree(rec_data);

    /* Compute shared secret via X25519 */
    uint8_t shared_secret[32];
    crypto_x25519(shared_secret, secret_key, server_pubkey);

    /* Get transcript hash at this point (after CH + SH) */
    uint8_t transcript_hash_hs[32];
    {
        struct sha256_ctx tmp = c->transcript;
        sha256_final(&tmp, transcript_hash_hs);
    }

    /* Derive handshake traffic keys */
    uint8_t hs_client_key[32], hs_client_iv[12];
    uint8_t hs_server_key[32], hs_server_iv[12];
    uint8_t handshake_secret[32];
    derive_handshake_keys(shared_secret, transcript_hash_hs,
                          hs_client_key, hs_client_iv,
                          hs_server_key, hs_server_iv,
                          handshake_secret);

    /* From here, server sends encrypted handshake messages.
     * We need to decrypt them using the handshake server key/iv. */
    uint64_t server_hs_seq = 0;

    /* Read encrypted handshake messages until we get Finished */
    int got_finished = 0;
    uint8_t s_hs_traffic_secret[32];
    {
        uint8_t empty_hash[32];
        sha256_buf(NULL, 0, empty_hash);
        uint8_t derived[32];
        derive_secret((uint8_t[32]){0}, "derived", 7, empty_hash, derived);
        /* Actually recompute properly */
    }
    derive_secret(handshake_secret, "s hs traffic", 12, transcript_hash_hs, s_hs_traffic_secret);

    /* Also need c hs traffic secret for Finished */
    uint8_t c_hs_traffic_secret[32];
    derive_secret(handshake_secret, "c hs traffic", 12, transcript_hash_hs, c_hs_traffic_secret);

    /* stage 13H: cert-validation state carried across the flight. */
    br_x509_pkey ee_pk;
    unsigned char ee_pk_buf[BR_X509_BUFSIZE_KEY];
    int cert_ok = 0, cv_ok = 0;
    uint8_t thash_cv[32];

    while (!got_finished) {
        rc = tls_read_record(c, &rec_type, &rec_data, &rec_len);
        if (rc != TLS_OK) return rc;

        if (rec_type == TLS_RT_CHANGE_CIPHER) {
            /* TLS 1.3 compatibility: ignore CCS */
            kfree(rec_data);
            continue;
        }

        if (rec_type != TLS_RT_APPLICATION) {
            /* Unexpected record type during handshake. A plaintext
             * alert here is the server rejecting our flight -- name it. */
            if (rec_type == TLS_RT_ALERT && rec_len >= 2)
                kprintf("[tls] plaintext alert during handshake: level=%d desc=%d\n",
                        rec_data[0], rec_data[1]);
            else
                kprintf("[tls] unexpected record type %d (len=%u) during handshake\n",
                        rec_type, (unsigned)rec_len);
            kfree(rec_data);
            return TLS_ERR_HANDSHAKE;
        }

        /* Decrypt with handshake server key */
        if (rec_len < 17) { kfree(rec_data); return TLS_ERR_RECORD; }
        size_t ct_len = rec_len - 16;
        uint8_t *mac = rec_data + ct_len;

        uint8_t nonce[12];
        memcpy(nonce, hs_server_iv, 12);
        for (int i = 0; i < 8; i++)
            nonce[4 + i] ^= (uint8_t)(server_hs_seq >> (56 - 8*i));

        uint8_t ad[5];
        ad[0] = TLS_RT_APPLICATION;
        put_u16(ad + 1, TLS_VERSION_12);
        put_u16(ad + 3, (uint16_t)rec_len);

        uint8_t *plain = (uint8_t *)kmalloc(ct_len);
        if (!plain) { kfree(rec_data); return TLS_ERR_NOMEM; }

        if (tls_aead_decrypt(plain, mac, hs_server_key, nonce,
                               ad, 5, rec_data, ct_len) != 0) {
            kfree(plain); kfree(rec_data);
            kprintf("[tls] handshake decrypt failed at seq %lu\n", (unsigned long)server_hs_seq);
            return TLS_ERR_RECORD;
        }
        server_hs_seq++;
        kfree(rec_data);

        /* Strip trailing zeros and content type */
        size_t plen = ct_len;
        while (plen > 0 && plain[plen - 1] == 0) plen--;
        if (plen == 0) { kfree(plain); return TLS_ERR_RECORD; }
        uint8_t inner_type = plain[plen - 1];
        plen--;

        if (inner_type != TLS_RT_HANDSHAKE) {
            if (inner_type == TLS_RT_ALERT) {
                kprintf("[tls] alert during handshake: %d %d\n",
                        plen >= 1 ? plain[0] : -1,
                        plen >= 2 ? plain[1] : -1);
                kfree(plain);
                return TLS_ERR_ALERT;
            }
            kfree(plain);
            continue;
        }

        /* Process handshake messages in this record */
        size_t mpos = 0;
        while (mpos + 4 <= plen) {
            uint8_t hs_type = plain[mpos];
            uint32_t hs_len = get_u24(plain + mpos + 1);
            if (mpos + 4 + hs_len > plen) break;

            /* CertificateVerify signs the transcript BEFORE itself, so
             * snapshot the pre-CV hash (ClientHello..Certificate). */
            if (hs_type == TLS_HS_CERT_VERIFY) {
                struct sha256_ctx tmp = c->transcript;
                sha256_final(&tmp, thash_cv);
            }

            /* Add to transcript (all except Finished) */
            if (hs_type != TLS_HS_FINISHED) {
                sha256_update(&c->transcript, plain + mpos, 4 + hs_len);
            }

            /* stage 13H: validate the certificate chain, then bind it to
             * this handshake via CertificateVerify. Fail closed. */
            if (hs_type == TLS_HS_CERTIFICATE) {
                int vr = tls_validate_chain(plain + mpos + 4, hs_len,
                                            hostname, &ee_pk,
                                            ee_pk_buf, sizeof ee_pk_buf);
                if (vr != 0) { kfree(plain); return vr; }
                cert_ok = 1;
            }
            if (hs_type == TLS_HS_CERT_VERIFY) {
                const uint8_t *cv = plain + mpos + 4;
                if (!cert_ok || hs_len < 4) { kfree(plain); return TLS_ERR_CERT; }
                uint16_t scheme = (uint16_t)get_u16(cv);
                size_t sl = get_u16(cv + 2);
                if (4 + sl > hs_len ||
                    !tls_verify_cv(scheme, cv + 4, sl, thash_cv, &ee_pk)) {
                    kprintf("[tls] CertificateVerify FAILED\n");
                    kfree(plain);
                    return TLS_ERR_CERT;
                }
                cv_ok = 1;
                kprintf("[tls] certificate chain + CertificateVerify OK\n");
            }

            /* EncryptedExtensions: pull out the server's ALPN choice.
             * Body is a 2-byte extensions_length then ext(type,len,data);
             * ALPN data is a 2-byte name-list length, then len-prefixed
             * protocol strings (the server returns exactly one). */
            if (hs_type == TLS_HS_ENCRYPTED_EXT && c->offer_h2) {
                const uint8_t *ee = plain + mpos + 4;
                if (hs_len >= 2) {
                    size_t elist = get_u16(ee);
                    size_t ep = 2, eend = 2 + elist;
                    if (eend > hs_len) eend = hs_len;
                    while (ep + 4 <= eend) {
                        uint16_t et = (uint16_t)get_u16(ee + ep);
                        uint16_t edl = (uint16_t)get_u16(ee + ep + 2);
                        const uint8_t *ed = ee + ep + 4;
                        if (ep + 4 + edl > eend) break;
                        if (et == TLS_EXT_ALPN && edl >= 3) {
                            size_t nlen = ed[2];   /* first (only) protocol */
                            if (nlen > 0 && nlen < sizeof(c->alpn) &&
                                3 + nlen <= edl) {
                                memcpy(c->alpn, ed + 3, nlen);
                                c->alpn[nlen] = 0;
                                kprintf("[tls] ALPN selected: %s\n", c->alpn);
                            }
                        }
                        ep += 4 + edl;
                    }
                }
            }

            if (hs_type == TLS_HS_FINISHED) {
                /* Verify server Finished */
                uint8_t transcript_before_sf[32];
                {
                    struct sha256_ctx tmp = c->transcript;
                    sha256_final(&tmp, transcript_before_sf);
                }

                uint8_t expected_verify[32];
                compute_finished(s_hs_traffic_secret, transcript_before_sf, expected_verify);

                if (hs_len >= 32 && memcmp(plain + mpos + 4, expected_verify, 32) == 0) {
                    /* Server Finished verified! */
                    sha256_update(&c->transcript, plain + mpos, 4 + hs_len);
                    got_finished = 1;
                } else {
                    kprintf("[tls] server Finished verify failed\n");
                    kfree(plain);
                    return TLS_ERR_HANDSHAKE;
                }
            }

            mpos += 4 + hs_len;
        }
        kfree(plain);
    }

    /* stage 13H: a server must have presented a chain AND proven it holds
     * the leaf key (no PSK path here). Reject a flight that skipped them. */
    if (!cert_ok || !cv_ok) {
        kprintf("[tls] handshake missing valid Certificate/CertificateVerify\n");
        return TLS_ERR_CERT;
    }

    /* Get transcript hash after all server handshake messages */
    uint8_t transcript_hash_sf[32];
    {
        struct sha256_ctx tmp = c->transcript;
        sha256_final(&tmp, transcript_hash_sf);
    }

    /* Send client Finished */
    uint8_t client_verify[32];
    compute_finished(c_hs_traffic_secret, transcript_hash_sf, client_verify);

    uint8_t client_finished_msg[36];
    client_finished_msg[0] = TLS_HS_FINISHED;
    put_u24(client_finished_msg + 1, 32);
    memcpy(client_finished_msg + 4, client_verify, 32);

    /* Send encrypted with client handshake key */
    {
        size_t payload_len = 36 + 1;
        size_t record_payload = payload_len + 16;

        uint8_t *ptxt = (uint8_t *)kmalloc(payload_len);
        if (!ptxt) return TLS_ERR_NOMEM;
        memcpy(ptxt, client_finished_msg, 36);
        ptxt[36] = TLS_RT_HANDSHAKE;

        uint8_t nonce[12];
        memcpy(nonce, hs_client_iv, 12);
        /* client_seq is 0 for handshake */

        uint8_t ad[5];
        ad[0] = TLS_RT_APPLICATION;
        put_u16(ad + 1, TLS_VERSION_12);
        put_u16(ad + 3, (uint16_t)record_payload);

        uint8_t *out_buf = (uint8_t *)kmalloc(5 + record_payload);
        if (!out_buf) { kfree(ptxt); return TLS_ERR_NOMEM; }
        memcpy(out_buf, ad, 5);
        tls_aead_encrypt(out_buf + 5, out_buf + 5 + payload_len,
                         hs_client_key, nonce,
                         ad, 5, ptxt, payload_len);

        long s = tcp_send(c->tcp, out_buf, 5 + record_payload);
        kfree(ptxt);
        kfree(out_buf);
        if (s != (long)(5 + record_payload)) return TLS_ERR_SEND;
    }

    /* Derive application traffic keys */
    /* Need transcript hash including client Finished for app keys */
    sha256_update(&c->transcript, client_finished_msg, 36);
    uint8_t transcript_hash_final[32];
    {
        struct sha256_ctx tmp = c->transcript;
        sha256_final(&tmp, transcript_hash_final);
    }

    derive_app_keys(handshake_secret, transcript_hash_sf,
                    c->client_key, c->client_iv,
                    c->server_key, c->server_iv);

    c->client_seq = 0;
    c->server_seq = 0;
    c->handshake_done = 1;

    kprintf("[tls] handshake complete (TLS 1.3, ChaCha20-Poly1305)\n");
    return TLS_OK;
}

/* ---- Public API ------------------------------------------------- */

struct tls_conn *tls_connect(uint32_t dst_ip_be, uint16_t dst_port_be,
                             const char *hostname,
                             uint32_t timeout_ms, int offer_h2, int *out_err) {
    struct tcp_conn *tcp = tcp_connect(dst_ip_be, dst_port_be, timeout_ms);
    if (!tcp) {
        if (out_err) *out_err = TLS_ERR_CONNECT;
        return NULL;
    }

    struct tls_conn *c = (struct tls_conn *)kmalloc(sizeof(struct tls_conn));
    if (!c) {
        tcp_close(tcp);
        if (out_err) *out_err = TLS_ERR_NOMEM;
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    c->tcp = tcp;
    c->offer_h2 = offer_h2;
    c->timeout_ms = timeout_ms ? timeout_ms : 5000;

    int rc = tls_do_handshake(c, hostname);
    if (rc != TLS_OK) {
        kprintf("[tls] handshake failed: %s\n", tls_strerror(rc));
        tcp_abort(tcp);            /* RST, not a TIME_WAIT-lingering close */
        kfree(c);
        if (out_err) *out_err = rc;
        return NULL;
    }

    if (out_err) *out_err = TLS_OK;
    return c;
}

long tls_send(struct tls_conn *c, const void *buf, size_t len) {
    if (!c || c->closed) return TLS_ERR_CLOSED;
    if (len == 0) return 0;

    /* Fragment into max-record-size chunks */
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        size_t chunk = len - sent;
        if (chunk > 16000) chunk = 16000;
        long rc = tls_send_encrypted(c, TLS_RT_APPLICATION, p + sent, chunk);
        if (rc < 0) return rc;
        sent += chunk;
    }
    return (long)sent;
}

long tls_recv(struct tls_conn *c, void *buf, size_t cap, uint32_t timeout_ms) {
    if (!c || c->closed) return TLS_ERR_CLOSED;
    if (cap == 0) return 0;

    /* Return buffered app data first */
    if (c->app_len > c->app_off) {
        size_t avail = c->app_len - c->app_off;
        size_t copy = avail < cap ? avail : cap;
        memcpy(buf, c->app_buf + c->app_off, copy);
        c->app_off += copy;
        return (long)copy;
    }

    /* Read and decrypt records until we get application data */
    uint32_t tm = timeout_ms ? timeout_ms : c->timeout_ms;
    for (;;) {
        uint8_t rec_type;
        uint8_t *rec_data;
        size_t rec_len;

        /* Temporarily override timeout */
        uint32_t old_tm = c->timeout_ms;
        c->timeout_ms = tm;
        int rc = tls_read_record(c, &rec_type, &rec_data, &rec_len);
        c->timeout_ms = old_tm;
        if (rc == TLS_ERR_CLOSED) { c->closed = 1; return TLS_ERR_CLOSED; }
        if (rc != TLS_OK) return (long)rc;

        if (rec_type == TLS_RT_CHANGE_CIPHER) {
            kfree(rec_data);
            continue;
        }

        if (rec_type == TLS_RT_ALERT) {
            c->closed = 1;
            kfree(rec_data);
            return TLS_ERR_ALERT;
        }

        if (rec_type != TLS_RT_APPLICATION) {
            kfree(rec_data);
            continue;
        }

        /* Decrypt */
        uint8_t inner_type;
        uint8_t *plain;
        size_t plain_len;
        rc = tls_decrypt_record(c, rec_data, rec_len, &inner_type, &plain, &plain_len);
        kfree(rec_data);
        if (rc != TLS_OK) return (long)rc;

        if (inner_type == TLS_RT_ALERT) {
            c->closed = 1;
            kfree(plain);
            return 0;  /* clean close */
        }

        if (inner_type == TLS_RT_APPLICATION && plain_len > 0) {
            size_t copy = plain_len < cap ? plain_len : cap;
            memcpy(buf, plain, copy);

            /* Buffer remainder */
            if (plain_len > copy) {
                size_t remaining = plain_len - copy;
                if (remaining > sizeof(c->app_buf)) remaining = sizeof(c->app_buf);
                memcpy(c->app_buf, plain + copy, remaining);
                c->app_len = remaining;
                c->app_off = 0;
            }
            kfree(plain);
            return (long)copy;
        }

        kfree(plain);
    }
}

const char *tls_alpn(struct tls_conn *c) {
    return c ? c->alpn : "";
}

void tls_close(struct tls_conn *c) {
    if (!c) return;
    if (!c->closed && c->handshake_done) {
        /* Send close_notify alert */
        uint8_t alert[2] = { 1, 0 };  /* warning, close_notify */
        tls_send_encrypted(c, TLS_RT_ALERT, alert, 2);
    }
    tcp_close(c->tcp);
    kfree(c);
}

const char *tls_strerror(int err) {
    switch (err) {
    case TLS_OK:            return "ok";
    case TLS_ERR_CONNECT:   return "TCP connect failed";
    case TLS_ERR_HANDSHAKE: return "TLS handshake failed";
    case TLS_ERR_SEND:      return "send failed";
    case TLS_ERR_RECV:      return "receive failed/timeout";
    case TLS_ERR_CLOSED:    return "connection closed";
    case TLS_ERR_NOMEM:     return "out of memory";
    case TLS_ERR_ALERT:     return "peer alert";
    case TLS_ERR_RECORD:    return "bad record";
    case TLS_ERR_VERSION:   return "unsupported TLS version";
    case TLS_ERR_CERT:      return "certificate validation failed";
    default:                return "unknown TLS error";
    }
}
