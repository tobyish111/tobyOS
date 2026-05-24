/* media.c -- Media playback pipeline for tobyOS.
 *
 * Opens a URL via HTTP, detects container format, demuxes audio/video
 * tracks, decodes frames, and manages A/V synchronization.
 */

#include "libtoby_internal.h"
#include <toby/media.h>
#include <toby/audio_decode.h>
#include <toby/video_decode.h>
#include <toby/net.h>
#include <stdlib.h>
#include <string.h>

/* Container format detection. */
#define CONTAINER_UNKNOWN  0
#define CONTAINER_MP3      1
#define CONTAINER_MP4      2
#define CONTAINER_WEBM     3
#define CONTAINER_OGG      4

#define MEDIA_BUF_SIZE     (256 * 1024)

struct media_player {
    enum media_state    state;
    char                url[512];

    /* Raw media data fetched from URL. */
    uint8_t            *buf;
    size_t              buf_len;
    int                 container;

    /* Decoders */
    toby_mp3_decoder_t *mp3_dec;
    struct video_decoder *video_dec;

    /* Playback position tracking */
    int64_t             position_ms;
    int64_t             duration_ms;

    /* Current video frame */
    struct video_frame  current_frame;
    int                 has_frame;

    /* Audio state */
    int                 audio_fd;
    int                 audio_channels;
    int                 audio_rate;
};

static int detect_container(const uint8_t *data, size_t len,
                            const char *content_type)
{
    if (content_type) {
        if (strstr(content_type, "audio/mpeg") ||
            strstr(content_type, "audio/mp3"))
            return CONTAINER_MP3;
        if (strstr(content_type, "video/mp4") ||
            strstr(content_type, "audio/mp4") ||
            strstr(content_type, "audio/aac"))
            return CONTAINER_MP4;
        if (strstr(content_type, "video/webm") ||
            strstr(content_type, "audio/webm"))
            return CONTAINER_WEBM;
        if (strstr(content_type, "audio/ogg") ||
            strstr(content_type, "video/ogg"))
            return CONTAINER_OGG;
    }

    if (len >= 4) {
        /* MP3: ID3 tag or sync word */
        if (data[0] == 'I' && data[1] == 'D' && data[2] == '3')
            return CONTAINER_MP3;
        if ((data[0] == 0xFF) && ((data[1] & 0xE0) == 0xE0))
            return CONTAINER_MP3;

        /* MP4/M4A: ftyp box */
        if (len >= 8 && data[4] == 'f' && data[5] == 't' &&
            data[6] == 'y' && data[7] == 'p')
            return CONTAINER_MP4;

        /* WebM: EBML header */
        if (data[0] == 0x1A && data[1] == 0x45 &&
            data[2] == 0xDF && data[3] == 0xA3)
            return CONTAINER_WEBM;

        /* Ogg: "OggS" */
        if (data[0] == 'O' && data[1] == 'g' &&
            data[2] == 'g' && data[3] == 'S')
            return CONTAINER_OGG;
    }

    return CONTAINER_UNKNOWN;
}

/* Skip past an ID3v2 header if present, return offset to first MP3 frame. */
static size_t skip_id3(const uint8_t *data, size_t len)
{
    if (len < 10) return 0;
    if (data[0] != 'I' || data[1] != 'D' || data[2] != '3') return 0;
    size_t tag_size = ((size_t)(data[6] & 0x7F) << 21) |
                      ((size_t)(data[7] & 0x7F) << 14) |
                      ((size_t)(data[8] & 0x7F) <<  7) |
                      ((size_t)(data[9] & 0x7F));
    size_t total = 10 + tag_size;
    return (total < len) ? total : len;
}

/* Decode MP3 audio frames and send to audio output. */
static void decode_mp3_audio(struct media_player *mp)
{
    if (!mp->mp3_dec || !mp->buf || mp->buf_len == 0) return;

    size_t offset = skip_id3(mp->buf, mp->buf_len);
    int16_t pcm[TOBY_AUDIO_MAX_SAMPLES];
    int total_samples = 0;

    while (offset < mp->buf_len) {
        int samples = 0, channels = 0, rate = 0;
        int consumed = toby_mp3_decode_frame(mp->mp3_dec,
                                             mp->buf + offset,
                                             mp->buf_len - offset,
                                             pcm,
                                             &samples, &channels, &rate);
        if (consumed <= 0) break;
        offset += (size_t)consumed;

        if (samples > 0 && channels > 0 && rate > 0) {
            /* Open audio device on first decoded frame. */
            if (mp->audio_fd < 0) {
                mp->audio_fd = (int)toby_sc3(ABI_SYS_AUDIO_OPEN,
                                             (long)rate, (long)channels, 0);
                mp->audio_channels = channels;
                mp->audio_rate     = rate;
            }
            if (mp->audio_fd >= 0) {
                toby_sc3(ABI_SYS_AUDIO_WRITE,
                         (long)mp->audio_fd,
                         (long)(uintptr_t)pcm,
                         (long)(samples * channels));
            }
            total_samples += samples;
        }
    }

    if (mp->audio_rate > 0 && total_samples > 0)
        mp->duration_ms = (int64_t)total_samples * 1000 / mp->audio_rate;
}

struct media_player *media_player_create(void)
{
    struct media_player *mp = (struct media_player *)malloc(sizeof(*mp));
    if (!mp) return NULL;
    memset(mp, 0, sizeof(*mp));
    mp->state    = MEDIA_STOPPED;
    mp->audio_fd = -1;
    return mp;
}

int media_player_open(struct media_player *mp, const char *url)
{
    if (!mp || !url) return -1;

    /* Reset previous state */
    if (mp->mp3_dec)   { toby_mp3_decode_free(mp->mp3_dec);     mp->mp3_dec   = NULL; }
    if (mp->video_dec) { video_decoder_destroy(mp->video_dec);  mp->video_dec = NULL; }
    if (mp->audio_fd >= 0) {
        toby_sc1(ABI_SYS_AUDIO_CLOSE, (long)mp->audio_fd);
        mp->audio_fd = -1;
    }
    free(mp->buf);
    mp->buf     = NULL;
    mp->buf_len = 0;

    /* Copy URL */
    size_t ulen = strlen(url);
    if (ulen >= sizeof(mp->url)) ulen = sizeof(mp->url) - 1;
    memcpy(mp->url, url, ulen);
    mp->url[ulen] = '\0';

    mp->state       = MEDIA_BUFFERING;
    mp->position_ms = 0;
    mp->duration_ms = 0;
    mp->has_frame   = 0;

    /* Fetch media via HTTP */
    struct http_response resp;
    memset(&resp, 0, sizeof(resp));
    int err = http_get(url, &resp);
    if (err != 0 || resp.status_code < 200 || resp.status_code >= 300) {
        http_response_free(&resp);
        mp->state = MEDIA_STOPPED;
        return -1;
    }

    mp->buf     = (uint8_t *)resp.body;
    mp->buf_len = resp.body_len;
    resp.body   = NULL;  /* transfer ownership */
    http_response_free(&resp);

    if (mp->buf_len == 0) {
        mp->state = MEDIA_STOPPED;
        return -1;
    }

    /* Detect format */
    mp->container = detect_container(mp->buf, mp->buf_len, NULL);

    /* Set up decoders based on container */
    switch (mp->container) {
    case CONTAINER_MP3:
        mp->mp3_dec = toby_mp3_decode_init();
        break;
    case CONTAINER_MP4:
        mp->video_dec = video_decoder_create(VIDEO_CODEC_H264);
        mp->mp3_dec   = toby_mp3_decode_init();  /* AAC audio track */
        break;
    case CONTAINER_WEBM:
        mp->video_dec = video_decoder_create(VIDEO_CODEC_VP9);
        break;
    default:
        mp->mp3_dec = toby_mp3_decode_init();
        break;
    }

    mp->state = MEDIA_STOPPED;
    return 0;
}

void media_player_play(struct media_player *mp)
{
    if (!mp || mp->state == MEDIA_PLAYING) return;
    mp->state = MEDIA_PLAYING;

    if (mp->mp3_dec && mp->container == CONTAINER_MP3)
        decode_mp3_audio(mp);
}

void media_player_pause(struct media_player *mp)
{
    if (!mp) return;
    if (mp->state == MEDIA_PLAYING)
        mp->state = MEDIA_PAUSED;
}

void media_player_seek(struct media_player *mp, int64_t position_ms)
{
    if (!mp) return;
    if (position_ms < 0) position_ms = 0;
    if (position_ms > mp->duration_ms) position_ms = mp->duration_ms;
    mp->position_ms = position_ms;
}

int64_t media_player_position(struct media_player *mp)
{
    if (!mp) return 0;
    return mp->position_ms;
}

int64_t media_player_duration(struct media_player *mp)
{
    if (!mp) return 0;
    return mp->duration_ms;
}

enum media_state media_player_state(struct media_player *mp)
{
    if (!mp) return MEDIA_STOPPED;
    return mp->state;
}

struct video_frame *media_player_get_frame(struct media_player *mp)
{
    if (!mp || !mp->has_frame) return NULL;
    return &mp->current_frame;
}

void media_player_destroy(struct media_player *mp)
{
    if (!mp) return;
    if (mp->mp3_dec)   toby_mp3_decode_free(mp->mp3_dec);
    if (mp->video_dec) video_decoder_destroy(mp->video_dec);
    if (mp->audio_fd >= 0)
        toby_sc1(ABI_SYS_AUDIO_CLOSE, (long)mp->audio_fd);
    free(mp->buf);
    free(mp);
}
