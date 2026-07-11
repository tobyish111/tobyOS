/* http3.h -- HTTP/3 framing + QPACK field-section coding (RFC 9114 /
 * RFC 9204), HTTP/3 slice 5.
 *
 * The transport-independent HTTP/3 pieces: QPACK encode for a request
 * field section (static-table/literal only -- we advertise no dynamic
 * table, so this is always sufficient) and QPACK decode for a response
 * field section (indexed-static, literal-with-name-reference,
 * literal-with-literal-name, values/names optionally Huffman-coded
 * with the RFC 7541 code shared with HPACK), plus the H3 frame header
 * (varint type + varint length). The QUIC stream plumbing lives with
 * the connection (quic_conn.c); http.c integration is the next slice. */

#ifndef TOBYOS_HTTP3_H
#define TOBYOS_HTTP3_H

#include <tobyos/types.h>

/* H3 frame types (RFC 9114 s7.2). */
#define H3_FRAME_DATA     0x00
#define H3_FRAME_HEADERS  0x01
#define H3_FRAME_SETTINGS 0x04

/* H3 unidirectional stream types (RFC 9114 s6.2). */
#define H3_STREAM_CONTROL 0x00

/* Encode a GET request field section (QPACK, static/literal only):
 * :method GET, :scheme https, :path, :authority. Returns bytes
 * written, or 0 on overflow. */
size_t h3_qpack_encode_request(uint8_t *out, size_t cap,
                               const char *authority, const char *path);

/* Decode a field section encoded against an empty dynamic table (what
 * a peer sends when we advertise QPACK_MAX_TABLE_CAPACITY 0 -- our
 * SETTINGS). Calls cb for each field. Returns 0, or -1 on malformed
 * input / a dynamic-table reference. */
typedef void (*h3_hdr_cb)(void *ud, const char *name, const char *value);
int h3_qpack_decode(const uint8_t *p, size_t len, h3_hdr_cb cb, void *ud);

/* Write an H3 frame header (varint type + varint payload length).
 * Returns bytes written, or 0 on overflow. */
size_t h3_frame_hdr(uint8_t *out, size_t cap, uint64_t type, uint64_t plen);

/* Self-test: decode a pylsqpack-encoded response section (embedded
 * reference vector, Huffman literals included) + build a request
 * section (dumped as hex for offline pylsqpack validation). Prints
 * "[h3] ..." and returns the PASS count. */
#define H3_SELFTEST_N 3
int h3_selftest(void);

#endif /* TOBYOS_HTTP3_H */
