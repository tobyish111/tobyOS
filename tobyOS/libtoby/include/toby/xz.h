/* libtoby: .xz container + LZMA2 codec (2026-09-04, native-parity batch).
 *
 * Decoder: a from-scratch LZMA/LZMA2 range decoder and .xz stream parser
 * (multi-stream, multi-block, CRC32/CRC64 verified; SHA-256 checks are
 * accepted but not verified; BCJ/delta filter chains are refused).
 * Encoder: a VALID .xz stream whose LZMA2 payload is uncompressed chunks
 * ("stored"): every xz decoder in the world reads it, and it is what a
 * from-scratch tool can honestly ship before an LZ match finder exists.
 * busybox's own `xz` applet is decode-only, so this is already a superset. */
#ifndef TOBY_XZ_H
#define TOBY_XZ_H
#include <stddef.h>
#include <stdint.h>

#define TOBY_XZ_CHECK_NONE   0
#define TOBY_XZ_CHECK_CRC32  1
#define TOBY_XZ_CHECK_CRC64  4
#define TOBY_XZ_CHECK_SHA256 10

/* Decode a whole .xz image. *out is malloc'd (free it); returns 0, or -1
 * with a message in err. */
int toby_xz_decode(const uint8_t *in, size_t in_len,
                   uint8_t **out, size_t *out_len, char *err, size_t errcap);

/* Encode in[] as a stored .xz stream with the given check type. */
int toby_xz_encode_stored(const uint8_t *in, size_t in_len, int check,
                          uint8_t **out, size_t *out_len);

/* Probe: does this look like an .xz stream header? */
int toby_xz_is_xz(const uint8_t *p, size_t n);

uint32_t toby_xz_crc32(uint32_t crc, const void *buf, size_t len);
uint64_t toby_xz_crc64(uint64_t crc, const void *buf, size_t len);

#endif
