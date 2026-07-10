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

#define QUIC_FRAME_PADDING  0x00
#define QUIC_FRAME_PING     0x01
#define QUIC_FRAME_ACK      0x02
#define QUIC_FRAME_CRYPTO   0x06

/* Write a CRYPTO frame (type 0x06, offset, length, data) into out
 * (>= data_len + 20 bytes). Returns bytes written, or 0 on overflow. */
size_t quic_frame_crypto(uint8_t *out, size_t cap,
                         uint64_t offset, const uint8_t *data, size_t len);

/* One parsed frame. For CRYPTO: off/len/data point into the input.
 * For ACK: largest/ack_delay filled (ranges skipped). */
struct quic_frame {
    uint8_t  type;
    uint64_t offset;              /* CRYPTO */
    uint64_t len;                 /* CRYPTO data length */
    const uint8_t *data;          /* CRYPTO payload (into input) */
    uint64_t largest;             /* ACK largest acknowledged */
};

/* Parse the next frame at p (cap bytes). Fills *f, returns bytes
 * consumed, or 0 on malformed input. PADDING runs collapse to one
 * PADDING frame. */
size_t quic_frame_parse(const uint8_t *p, size_t cap, struct quic_frame *f);

/* ---- Initial packets (long header, RFC 9001) -------------------- */

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

/* ---- Self-test -------------------------------------------------- */

/* Build + protect an Initial packet and check it byte-for-byte
 * (via SHA-256) against the aioquic reference; then open it back and
 * verify the recovered payload + parsed CRYPTO frame. Prints
 * "[quicpkt] ..." and returns the PASS count. */
#define QUIC_PACKET_SELFTEST_N 4
int quic_packet_selftest(void);

#endif /* TOBYOS_QUIC_PACKET_H */
