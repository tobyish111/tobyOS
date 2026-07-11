/* quic_packet.h -- QUIC packet + frame wire format (RFC 9000/9001).
 *
 * HTTP/3 slice 3: the packet layer on top of the QUIC crypto
 * primitives (quic_crypto.h). Builds and opens protected Initial
 * packets (long header + packet protection + header protection) and
 * encodes/parses the frames the handshake needs (CRYPTO, PADDING,
 * ACK). No UDP or connection state yet -- the state machine (slice 4)
 * drives these. Verified against the aioquic reference by
 * quic_packet_selftest(). */

#ifndef TOBYOS_QUIC_PACKET_H
#define TOBYOS_QUIC_PACKET_H

#include <tobyos/types.h>

/* ---- Frames ----------------------------------------------------- */

#define QUIC_FRAME_PADDING        0x00
#define QUIC_FRAME_PING           0x01
#define QUIC_FRAME_ACK            0x02
#define QUIC_FRAME_ACK_ECN        0x03
#define QUIC_FRAME_CRYPTO         0x06
#define QUIC_FRAME_NEW_TOKEN      0x07
#define QUIC_FRAME_NEW_CID        0x18
#define QUIC_FRAME_RETIRE_CID     0x19
#define QUIC_FRAME_CONN_CLOSE     0x1c   /* transport error */
#define QUIC_FRAME_CONN_CLOSE_APP 0x1d   /* application error */
#define QUIC_FRAME_HANDSHAKE_DONE 0x1e

/* Write a CRYPTO frame (type 0x06, offset, length, data) into out
 * (>= data_len + 20 bytes). Returns bytes written, or 0 on overflow. */
size_t quic_frame_crypto(uint8_t *out, size_t cap,
                         uint64_t offset, const uint8_t *data, size_t len);

/* Write an ACK frame (type 0x02): largest acknowledged, zero delay,
 * one range covering [largest - first_range, largest]. Returns bytes
 * written, or 0 on overflow. */
size_t quic_frame_ack(uint8_t *out, size_t cap,
                      uint64_t largest, uint64_t first_range);

/* One parsed frame. For CRYPTO: off/len/data point into the input.
 * For ACK: largest filled (ranges skipped). For CONNECTION_CLOSE:
 * err_code + data/len = the reason phrase (into input). */
struct quic_frame {
    uint8_t  type;
    uint64_t offset;              /* CRYPTO */
    uint64_t len;                 /* CRYPTO data / CLOSE reason length */
    const uint8_t *data;          /* CRYPTO payload / CLOSE reason */
    uint64_t largest;             /* ACK largest acknowledged */
    uint64_t err_code;            /* CONNECTION_CLOSE error code */
};

/* Parse the next frame at p (cap bytes). Fills *f, returns bytes
 * consumed, or 0 on malformed input. PADDING runs collapse to one
 * PADDING frame. Handled: PADDING/PING/ACK(+ECN)/CRYPTO/NEW_TOKEN/
 * NEW_CONNECTION_ID/RETIRE_CONNECTION_ID/CONNECTION_CLOSE/
 * HANDSHAKE_DONE. */
size_t quic_frame_parse(const uint8_t *p, size_t cap, struct quic_frame *f);

/* ---- Initial packets (long header, RFC 9001) -------------------- */

/* Build a protected long-header packet. type_bits selects the packet
 * type (0x00 Initial, 0x20 Handshake); an Initial carries a token
 * field, a Handshake does not (has_token). key/iv/hp are the keys for
 * this encryption level. Returns total protected bytes, or 0. */
size_t quic_build_long(uint8_t *out, size_t cap, unsigned type_bits,
                       const uint8_t *dcid, size_t dcid_len,
                       const uint8_t *scid, size_t scid_len,
                       int has_token,
                       const uint8_t *token, size_t token_len,
                       uint64_t pkt_num, unsigned pn_len,
                       const uint8_t *payload, size_t payload_len,
                       const uint8_t key[16], const uint8_t iv[12],
                       const uint8_t hp[16]);

/* Build a protected Initial packet into out (>= payload_len + 64).
 * key/iv/hp are the AES-128-GCM Initial keys for this side
 * (quic_initial_keys). pn_len is 1..4. Returns total protected bytes,
 * or 0 on overflow. */
size_t quic_build_initial(uint8_t *out, size_t cap,
                          const uint8_t *dcid, size_t dcid_len,
                          const uint8_t *scid, size_t scid_len,
                          const uint8_t *token, size_t token_len,
                          uint64_t pkt_num, unsigned pn_len,
                          const uint8_t *payload, size_t payload_len,
                          const uint8_t key[16], const uint8_t iv[12],
                          const uint8_t hp[16]);

/* Open (remove header + packet protection from) a long-header packet
 * in place. is_initial selects the Initial layout (which has a token
 * field) vs the Handshake layout (which does not). On success returns
 * the decrypted payload length, sets *out_payload to the frames region
 * (inside `pkt`), *out_pn to the packet number, and *consumed (if not
 * NULL) to the total on-wire length of this packet -- so the caller
 * can find the next coalesced packet in the same datagram. Returns -1
 * on a bad/undecryptable packet. */
long quic_open_long(uint8_t *pkt, size_t len, int is_initial,
                    const uint8_t key[16], const uint8_t iv[12],
                    const uint8_t hp[16],
                    const uint8_t **out_payload, uint64_t *out_pn,
                    size_t *consumed);

/* Initial-only wrapper (kept for the slice-3 API + self-test). */
long quic_open_initial(uint8_t *pkt, size_t len,
                       const uint8_t key[16], const uint8_t iv[12],
                       const uint8_t hp[16],
                       const uint8_t **out_payload, uint64_t *out_pn);

/* ---- Short-header (1-RTT) packets (RFC 9000 s17.3) --------------- */

/* Build a protected 1-RTT short-header packet: flags (0x40 | pn_len-1,
 * key phase 0), raw DCID, packet number, payload; AES-128-GCM packet
 * protection + header protection (low 5 bits masked). Returns total
 * protected bytes, or 0 on overflow. */
size_t quic_build_short(uint8_t *out, size_t cap,
                        const uint8_t *dcid, size_t dcid_len,
                        uint64_t pkt_num, unsigned pn_len,
                        const uint8_t *payload, size_t payload_len,
                        const uint8_t key[16], const uint8_t iv[12],
                        const uint8_t hp[16]);

/* Open a 1-RTT short-header packet in place. The short header has no
 * length field -- the packet extends to the end of the datagram, so it
 * is always the last packet in one. dcid_len is the length of OUR
 * connection ID (the peer echoes it; the header doesn't encode it).
 * Small packet numbers are taken at face value (no reconstruction
 * window yet). Returns payload length, or -1. */
long quic_open_short(uint8_t *pkt, size_t len, size_t dcid_len,
                     const uint8_t key[16], const uint8_t iv[12],
                     const uint8_t hp[16],
                     const uint8_t **out_payload, uint64_t *out_pn);

/* ---- Retry packets (RFC 9000 s17.2.5) ---------------------------- */

/* Parse a Retry packet (long header, type bits 0x30): extract the
 * server's new SCID (the client's next DCID) and the retry token.
 * Pointers point into `pkt`; the final 16 bytes are the integrity tag
 * (verify with quic_retry_tag_verify). Returns 0 on success, -1 on a
 * malformed packet. */
int quic_parse_retry(const uint8_t *pkt, size_t len,
                     const uint8_t **out_scid, size_t *out_scid_len,
                     const uint8_t **out_token, size_t *out_token_len);

/* ---- Self-test -------------------------------------------------- */

/* Build + protect an Initial packet and check it byte-for-byte
 * (via SHA-256) against the aioquic reference; then open it back and
 * verify the recovered payload + parsed CRYPTO frame. Prints
 * "[quicpkt] ..." and returns the PASS count. */
#define QUIC_PACKET_SELFTEST_N 4
int quic_packet_selftest(void);

#endif /* TOBYOS_QUIC_PACKET_H */
