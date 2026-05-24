/* h264_decode.c -- Minimal H.264 NAL unit parser + placeholder decoder.
 *
 * Parses NAL unit headers (type, nal_ref_idc) and extracts width/height
 * from SPS NAL units.  Actual frame decode produces a solid-colour
 * placeholder frame at the parsed dimensions.
 */

#include "libtoby_internal.h"
#include <toby/video_decode.h>
#include <stdlib.h>
#include <string.h>

/* NAL unit types we care about. */
#define NAL_SLICE_NON_IDR   1
#define NAL_SLICE_IDR       5
#define NAL_SEI             6
#define NAL_SPS             7
#define NAL_PPS             8

struct video_decoder {
    int      codec;
    int      width;
    int      height;
    int64_t  frame_count;
    uint8_t *fb;             /* reusable framebuffer */
    size_t   fb_size;
};

/* Minimal Exp-Golomb unsigned integer reader. */
struct bitstream {
    const uint8_t *data;
    size_t         len;
    size_t         bit_pos;
};

static int bs_read1(struct bitstream *bs)
{
    if (bs->bit_pos / 8 >= bs->len) return 0;
    int val = (bs->data[bs->bit_pos / 8] >> (7 - (bs->bit_pos & 7))) & 1;
    bs->bit_pos++;
    return val;
}

static unsigned bs_read_bits(struct bitstream *bs, int n)
{
    unsigned val = 0;
    for (int i = 0; i < n; i++)
        val = (val << 1) | (unsigned)bs_read1(bs);
    return val;
}

static unsigned bs_read_ue(struct bitstream *bs)
{
    int leading_zeros = 0;
    while (bs_read1(bs) == 0 && leading_zeros < 31)
        leading_zeros++;
    if (leading_zeros == 0) return 0;
    unsigned val = (1u << leading_zeros) - 1 + bs_read_bits(bs, leading_zeros);
    return val;
}

/* Parse SPS to extract width and height. */
static void parse_sps(struct video_decoder *dec,
                      const uint8_t *data, size_t len)
{
    if (len < 4) return;

    struct bitstream bs;
    bs.data    = data;
    bs.len     = len;
    bs.bit_pos = 0;

    unsigned profile_idc = bs_read_bits(&bs, 8);
    bs_read_bits(&bs, 8);  /* constraint flags + reserved */
    bs_read_bits(&bs, 8);  /* level_idc */
    bs_read_ue(&bs);       /* seq_parameter_set_id */

    if (profile_idc == 100 || profile_idc == 110 ||
        profile_idc == 122 || profile_idc == 244 ||
        profile_idc == 44  || profile_idc == 83  ||
        profile_idc == 86  || profile_idc == 118 ||
        profile_idc == 128) {
        unsigned chroma = bs_read_ue(&bs);
        if (chroma == 3) bs_read1(&bs);
        bs_read_ue(&bs);  /* bit_depth_luma_minus8 */
        bs_read_ue(&bs);  /* bit_depth_chroma_minus8 */
        bs_read1(&bs);    /* qpprime_y_zero_transform_bypass_flag */
        int scaling_present = bs_read1(&bs);
        if (scaling_present) {
            int n = (chroma != 3) ? 8 : 12;
            for (int i = 0; i < n; i++) {
                if (bs_read1(&bs)) {
                    int size = (i < 6) ? 16 : 64;
                    int last = 8, next = 8;
                    for (int j = 0; j < size; j++) {
                        if (next != 0) {
                            /* delta_scale is signed Exp-Golomb */
                            unsigned ue = bs_read_ue(&bs);
                            int se = (ue & 1) ? (int)((ue + 1) / 2) : -(int)(ue / 2);
                            next = (last + se + 256) % 256;
                        }
                        last = (next == 0) ? last : next;
                    }
                }
            }
        }
    }

    bs_read_ue(&bs);  /* log2_max_frame_num_minus4 */
    unsigned pic_order_cnt_type = bs_read_ue(&bs);
    if (pic_order_cnt_type == 0) {
        bs_read_ue(&bs);  /* log2_max_pic_order_cnt_lsb_minus4 */
    } else if (pic_order_cnt_type == 1) {
        bs_read1(&bs);    /* delta_pic_order_always_zero_flag */
        bs_read_ue(&bs);  /* offset_for_non_ref_pic */
        bs_read_ue(&bs);  /* offset_for_top_to_bottom_field */
        unsigned n = bs_read_ue(&bs);
        for (unsigned i = 0; i < n && i < 256; i++)
            bs_read_ue(&bs);
    }

    bs_read_ue(&bs);  /* max_num_ref_frames */
    bs_read1(&bs);    /* gaps_in_frame_num_value_allowed_flag */

    unsigned pic_width_in_mbs    = bs_read_ue(&bs) + 1;
    unsigned pic_height_in_mbs   = bs_read_ue(&bs) + 1;

    int w = (int)(pic_width_in_mbs  * 16);
    int h = (int)(pic_height_in_mbs * 16);

    int frame_mbs_only = bs_read1(&bs);
    if (!frame_mbs_only) {
        h *= 2;
        bs_read1(&bs);  /* mb_adaptive_frame_field_flag */
    }

    /* Cropping */
    int cropping = bs_read1(&bs);
    if (cropping) {
        unsigned cl = bs_read_ue(&bs);
        unsigned cr = bs_read_ue(&bs);
        unsigned ct = bs_read_ue(&bs);
        unsigned cb = bs_read_ue(&bs);
        w -= (int)(cl + cr) * 2;
        h -= (int)(ct + cb) * 2;
    }

    if (w > 0 && w <= 7680 && h > 0 && h <= 4320) {
        dec->width  = w;
        dec->height = h;
    }
}

struct video_decoder *video_decoder_create(int codec)
{
    struct video_decoder *dec = (struct video_decoder *)malloc(sizeof(*dec));
    if (!dec) return NULL;
    memset(dec, 0, sizeof(*dec));
    dec->codec  = codec;
    dec->width  = 320;
    dec->height = 240;
    return dec;
}

int video_decoder_decode(struct video_decoder *dec,
                         const uint8_t *nal, size_t nal_len,
                         struct video_frame *out)
{
    if (!dec || !nal || nal_len < 1 || !out) return -1;

    /* Parse NAL header */
    int forbidden    = (nal[0] >> 7) & 1;
    int nal_ref_idc  = (nal[0] >> 5) & 3;
    int nal_type     = nal[0] & 0x1F;
    (void)forbidden;
    (void)nal_ref_idc;

    if (dec->codec == VIDEO_CODEC_H264) {
        if (nal_type == NAL_SPS && nal_len > 1) {
            parse_sps(dec, nal + 1, nal_len - 1);
        }

        if (nal_type == NAL_SLICE_IDR || nal_type == NAL_SLICE_NON_IDR) {
            size_t needed = (size_t)(dec->width * dec->height * 4);
            if (dec->fb_size < needed) {
                free(dec->fb);
                dec->fb = (uint8_t *)malloc(needed);
                if (!dec->fb) { dec->fb_size = 0; return -1; }
                dec->fb_size = needed;
            }

            /* Produce placeholder: cycling solid colour per frame. */
            uint32_t colors[] = {
                0xFF4040FFu, 0xFF40C040u, 0xFF4080FFu,
                0xFFC04040u, 0xFFC0C040u, 0xFF8040C0u
            };
            uint32_t c = colors[dec->frame_count % 6];
            uint32_t *px = (uint32_t *)dec->fb;
            int npx = dec->width * dec->height;
            for (int i = 0; i < npx; i++) px[i] = c;

            out->data   = dec->fb;
            out->width  = dec->width;
            out->height = dec->height;
            out->pts_ms = dec->frame_count * 33;  /* ~30 fps */
            dec->frame_count++;
            return 0;
        }

        return 1;  /* non-picture NAL, no frame produced */
    }

    /* VP9 stub */
    return -1;
}

void video_decoder_destroy(struct video_decoder *dec)
{
    if (!dec) return;
    free(dec->fb);
    free(dec);
}
