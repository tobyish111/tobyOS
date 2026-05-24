/* youtube.h -- YouTube playback integration for tobyOS browser.
 *
 * Handles extracting video/audio stream URLs from YouTube pages,
 * parsing DASH manifests, and managing adaptive bitrate playback.
 */

#ifndef TOBY_YOUTUBE_H
#define TOBY_YOUTUBE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stream quality levels */
enum yt_quality {
    YT_QUALITY_144P = 0,
    YT_QUALITY_240P,
    YT_QUALITY_360P,
    YT_QUALITY_480P,
    YT_QUALITY_720P,
    YT_QUALITY_1080P
};

/* Stream types */
enum yt_stream_type {
    YT_STREAM_VIDEO = 0,
    YT_STREAM_AUDIO,
    YT_STREAM_MUXED   /* combined video+audio */
};

/* Codec types */
enum yt_codec {
    YT_CODEC_H264 = 0,
    YT_CODEC_VP9,
    YT_CODEC_AV1,
    YT_CODEC_AAC,
    YT_CODEC_OPUS,
    YT_CODEC_VORBIS
};

/* A single media stream from YouTube */
struct yt_stream {
    enum yt_stream_type type;
    enum yt_codec codec;
    enum yt_quality quality;
    int width, height;           /* video only */
    int bitrate;                 /* bits per second */
    int sample_rate;             /* audio only */
    int channels;                /* audio only */
    char url[2048];              /* direct stream URL */
    char mime_type[64];
    int64_t content_length;
};

#define YT_MAX_STREAMS 32

/* Video metadata */
struct yt_video_info {
    char video_id[16];
    char title[256];
    char author[128];
    int64_t duration_ms;
    int stream_count;
    struct yt_stream streams[YT_MAX_STREAMS];
    int thumbnail_width, thumbnail_height;
    char thumbnail_url[512];
};

/* Extract video info from a YouTube URL or video ID.
 * Fetches the page, parses player response, extracts streams.
 * Returns 0 on success, -1 on failure. */
int yt_get_video_info(const char *url_or_id, struct yt_video_info *info);

/* Select best video stream for a given quality preference */
const struct yt_stream *yt_select_video(const struct yt_video_info *info,
                                         enum yt_quality max_quality);

/* Select best audio stream */
const struct yt_stream *yt_select_audio(const struct yt_video_info *info);

/* ---- DASH manifest parsing ---- */

struct dash_segment {
    int64_t start_byte;
    int64_t end_byte;
    int64_t duration_ms;
};

struct dash_representation {
    enum yt_stream_type type;
    enum yt_codec codec;
    int bandwidth;
    int width, height;
    int sample_rate;
    char base_url[2048];
    struct dash_segment segments[256];
    int segment_count;
};

#define DASH_MAX_REPS 16

struct dash_manifest {
    int64_t duration_ms;
    struct dash_representation reps[DASH_MAX_REPS];
    int rep_count;
};

/* Parse a DASH/MPD manifest */
int dash_parse(const char *mpd_xml, size_t len, struct dash_manifest *out);

/* ---- Playback control ---- */

struct yt_player;

/* Create a YouTube player instance */
struct yt_player *yt_player_create(void);

/* Load a video by URL or ID */
int yt_player_load(struct yt_player *p, const char *url_or_id);

/* Playback control */
void yt_player_play(struct yt_player *p);
void yt_player_pause(struct yt_player *p);
void yt_player_seek(struct yt_player *p, int64_t position_ms);
void yt_player_set_quality(struct yt_player *p, enum yt_quality q);

/* Get current state */
int64_t yt_player_position(struct yt_player *p);
int64_t yt_player_duration(struct yt_player *p);
int yt_player_is_playing(struct yt_player *p);
int yt_player_is_buffering(struct yt_player *p);

/* Get current video frame (for rendering) */
struct video_frame;
struct video_frame *yt_player_get_frame(struct yt_player *p);

/* Destroy player */
void yt_player_destroy(struct yt_player *p);

#ifdef __cplusplus
}
#endif

#endif /* TOBY_YOUTUBE_H */
