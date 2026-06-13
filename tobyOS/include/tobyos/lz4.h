/* lz4.h -- minimal LZ4 block-format codec (for memory compression).
 *
 * LZ4 is the algorithm Linux's zram/zswap use: very fast, byte-oriented
 * LZ77 with a single hash table, no entropy stage. We use it to compress
 * 4 KiB pages before they leave physical memory (see zram.h / swap.c).
 *
 * This implements the raw LZ4 *block* format (no frame header / no
 * checksums): a stream of sequences
 *
 *     token | [extra lit len] | literals | offset(2 LE) | [extra match len]
 *
 * token high nibble = literal length (0..15, 15 => extra bytes follow),
 * token low nibble  = match length - 4 (0..15, 15 => extra bytes follow).
 * The final sequence is literals-only (no offset/match); the last 5 bytes
 * of input are always literals and matches never start in the last 12.
 * Offsets are 1..65535 (back-reference distance).
 *
 * The compressor is greedy single-candidate (not the canonical lazy
 * matcher) -- valid LZ4 output, and it still collapses the zero / repeated
 * runs that dominate real anonymous pages into a handful of sequences.
 */

#ifndef TOBYOS_LZ4_H
#define TOBYOS_LZ4_H

#include <tobyos/types.h>

/* Scratch hash table the compressor needs (caller-owned so we never put
 * 16 KiB on the stack nor kmalloc per call). Pass a buffer of at least
 * LZ4_HASHTABLE_BYTES, suitably aligned for int. */
#define LZ4_HASH_BITS      12
#define LZ4_HASH_SIZE      (1u << LZ4_HASH_BITS)
#define LZ4_HASHTABLE_BYTES (LZ4_HASH_SIZE * (int)sizeof(int))

/* Compress [src, src+slen) into [dst, dst+dcap). `hashtable` is scratch
 * (>= LZ4_HASHTABLE_BYTES). Returns the compressed length on success, or
 * <= 0 if the data is incompressible / would not fit in dcap (the caller
 * then stores the page uncompressed). */
int lz4_compress(const void *src, int slen, void *dst, int dcap,
                 void *hashtable);

/* Decompress [src, src+slen) into [dst, dst+dcap). Returns the
 * decompressed length, or < 0 on malformed input / output overflow. */
int lz4_decompress(const void *src, int slen, void *dst, int dcap);

#endif /* TOBYOS_LZ4_H */
