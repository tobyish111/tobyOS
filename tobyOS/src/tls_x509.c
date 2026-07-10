/* tls_x509.c -- transport-independent TLS 1.3 certificate authentication.
 *
 * X.509 chain validation + CertificateVerify verification, lifted from
 * tls.c (stage 13, HTTP/3 slice 4f) so TLS-over-TCP and QUIC share it.
 * See tls_x509.h. Pure move from tls.c -- behavior-identical. */

#include <tobyos/tls_x509.h>
#include <tobyos/tls13.h>       /* tls_get_u16/u24 wire helpers */
#include <tobyos/klibc.h>
#include <tobyos/printk.h>
#include <tobyos/heap.h>
#include <tobyos/rtc.h>

#include <bearssl.h>

/* Trust anchors (Mozilla roots) from src/tls_trust.c. */
const br_x509_trust_anchor *tls_trust_anchors(size_t *count);

/* Negative failure code (kept distinct so callers can map it). */
#define TLS_X509_ERR   -10   /* == TLS_ERR_CERT */
#define TLS_X509_NOMEM -6    /* == TLS_ERR_NOMEM */

int tls_x509_validate_chain(const uint8_t *msg, size_t msglen,
                            const char *hostname,
                            br_x509_pkey *out_pk,
                            unsigned char *pk_buf, size_t pk_buf_cap) {
    if (msglen < 4) return TLS_X509_ERR;
    size_t p = 0;
    size_t ctx_len = msg[p];              /* certificate_request_context */
    p += 1 + ctx_len;
    if (p + 3 > msglen) return TLS_X509_ERR;
    size_t list_len = tls_get_u24(msg + p); p += 3;
    if (p + list_len > msglen) return TLS_X509_ERR;

    br_x509_minimal_context *xc =
        (br_x509_minimal_context *)kmalloc(sizeof(*xc));
    if (!xc) return TLS_X509_NOMEM;

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
        size_t clen = tls_get_u24(msg + q); q += 3;
        if (q + clen > end) break;
        (*xcc)->start_cert(xcc, (uint32_t)clen);
        (*xcc)->append(xcc, msg + q, clen);
        (*xcc)->end_cert(xcc);
        q += clen;
        if (q + 2 > end) break;
        q += 2 + tls_get_u16(msg + q);     /* skip this entry's extensions */
    }
    unsigned err = (*xcc)->end_chain(xcc);
    int rc = TLS_X509_ERR;
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

int tls_x509_verify_cv(uint16_t scheme, const uint8_t *sig, size_t siglen,
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
