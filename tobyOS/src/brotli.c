/* src/brotli.c -- kernel-side brotli decoder (stage 13G).
 *
 * Amalgamates the vendored brotli v1.0.9 reference DECODER
 * (third_party/brotli) into one translation unit: the reference files
 * carry no colliding file-scope statics, so #including them here is
 * simpler than adding a make rule per file, and keeps the vendored
 * sources pristine. The <string.h>/<stdlib.h> the reference pulls are
 * satisfied by shims on the brotli include path (see the src/brotli.o
 * make rule); everything else it needs (memcpy, kmalloc) is the kernel's.
 *
 * We expose one function, brotli_decompress(), used by src/http.c to
 * transparently decode Content-Encoding: br bodies (and later WOFF2). */

#include <tobyos/brotli.h>
#include <tobyos/heap.h>
#include <tobyos/klibc.h>

/* The reference decoder, in dependency order. Each file's own relative
 * includes ("../common/platform.h", <brotli/decode.h>) resolve via the
 * -I flags on the src/brotli.o rule. */
#include "../third_party/brotli/common/constants.c"
#include "../third_party/brotli/common/context.c"
#include "../third_party/brotli/common/dictionary.c"
#include "../third_party/brotli/common/transform.c"
#include "../third_party/brotli/common/platform.c"
#include "../third_party/brotli/dec/bit_reader.c"
#include "../third_party/brotli/dec/huffman.c"
#include "../third_party/brotli/dec/state.c"
#include "../third_party/brotli/dec/decode.c"

/* kmalloc/kfree adapters matching brotli's allocator signature. */
static void *br_alloc(void *opaque, size_t size) {
    (void)opaque;
    return kmalloc(size ? size : 1);
}
static void br_free(void *opaque, void *address) {
    (void)opaque;
    if (address) kfree(address);
}

int brotli_decompress(uint8_t *dest, unsigned long *destlen,
                      const uint8_t *src, unsigned long srclen) {
    if (!dest || !destlen || !src) return BROTLI_ERR;

    BrotliDecoderState *st = BrotliDecoderCreateInstance(br_alloc, br_free, 0);
    if (!st) return BROTLI_ERR;

    size_t avail_in  = (size_t)srclen;
    const uint8_t *next_in = src;
    size_t avail_out = (size_t)*destlen;
    uint8_t *next_out = dest;
    size_t total_out = 0;

    BrotliDecoderResult r = BrotliDecoderDecompressStream(
        st, &avail_in, &next_in, &avail_out, &next_out, &total_out);

    int ret;
    switch (r) {
    case BROTLI_DECODER_RESULT_SUCCESS:
        *destlen = (unsigned long)total_out;
        ret = BROTLI_OK;
        break;
    case BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT:
        /* output cap hit -- keep the decompressed prefix (truncate-at-cap,
         * mirroring puff's PUFF_TRUNC). */
        *destlen = (unsigned long)total_out;
        ret = BROTLI_TRUNC;
        break;
    default:
        /* NEEDS_MORE_INPUT (truncated stream) or ERROR (malformed). */
        ret = BROTLI_ERR;
        break;
    }
    BrotliDecoderDestroyInstance(st);
    return ret;
}
