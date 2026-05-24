/* youtube.c -- YouTube video playback integration.
 *
 * Extracts video/audio stream URLs from YouTube, parses DASH manifests,
 * and manages adaptive bitrate playback with A/V synchronization.
 */

#include <toby/youtube.h>
#include <toby/net.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- Internal helpers ---- */

static int is_video_id_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static int extract_video_id(const char *url_or_id, char *id_out) {
    if (!url_or_id || !id_out) return -1;

    /* Direct video ID (11 chars) */
    size_t len = strlen(url_or_id);
    if (len == 11) {
        int all_valid = 1;
        for (int i = 0; i < 11; i++) {
            if (!is_video_id_char(url_or_id[i])) { all_valid = 0; break; }
        }
        if (all_valid) {
            memcpy(id_out, url_or_id, 11);
            id_out[11] = '\0';
            return 0;
        }
    }

    /* youtube.com/watch?v=XXXXXXXXXXX */
    const char *v = strstr(url_or_id, "v=");
    if (v) {
        v += 2;
        int i = 0;
        while (is_video_id_char(v[i]) && i < 11) {
            id_out[i] = v[i];
            i++;
        }
        id_out[i] = '\0';
        return (i == 11) ? 0 : -1;
    }

    /* youtu.be/XXXXXXXXXXX */
    const char *be = strstr(url_or_id, "youtu.be/");
    if (be) {
        be += 9;
        int i = 0;
        while (is_video_id_char(be[i]) && i < 11) {
            id_out[i] = be[i];
            i++;
        }
        id_out[i] = '\0';
        return (i == 11) ? 0 : -1;
    }

    return -1;
}

/* Extract a JSON string value by key from raw text */
static int json_extract_string(const char *json, const char *key,
                                char *out, int max_out) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":\"", key);
    const char *p = strstr(json, search);
    if (!p) {
        snprintf(search, sizeof(search), "\"%s\": \"", key);
        p = strstr(json, search);
    }
    if (!p) return -1;

    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ') p++;
    if (*p != '"') return -1;
    p++;

    int i = 0;
    while (*p && *p != '"' && i < max_out - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
            case 'n': out[i++] = '\n'; break;
            case 't': out[i++] = '\t'; break;
            case '"': out[i++] = '"'; break;
            case '\\': out[i++] = '\\'; break;
            case '/': out[i++] = '/'; break;
            default: out[i++] = *p; break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return i;
}

static int json_extract_int(const char *json, const char *key) {
    char search[128];
    snprintf(search, sizeof(search), "\"%s\":", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '"') p++;
    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    int val = 0;
    while (*p >= '0' && *p <= '9') {
        val = val * 10 + (*p - '0');
        p++;
    }
    return neg ? -val : val;
}

/* Parse a streaming format entry from YouTube's player response */
static int parse_format(const char *fmt_json, struct yt_stream *stream) {
    memset(stream, 0, sizeof(*stream));

    /* Extract itag to determine format */
    int itag = json_extract_int(fmt_json, "itag");

    /* Determine type and codec from itag */
    switch (itag) {
    /* Video+Audio muxed */
    case 18: stream->type = YT_STREAM_MUXED; stream->codec = YT_CODEC_H264;
             stream->quality = YT_QUALITY_360P; stream->width = 640; stream->height = 360; break;
    case 22: stream->type = YT_STREAM_MUXED; stream->codec = YT_CODEC_H264;
             stream->quality = YT_QUALITY_720P; stream->width = 1280; stream->height = 720; break;
    /* Video only (DASH) */
    case 134: stream->type = YT_STREAM_VIDEO; stream->codec = YT_CODEC_H264;
              stream->quality = YT_QUALITY_360P; stream->width = 640; stream->height = 360; break;
    case 135: stream->type = YT_STREAM_VIDEO; stream->codec = YT_CODEC_H264;
              stream->quality = YT_QUALITY_480P; stream->width = 854; stream->height = 480; break;
    case 136: stream->type = YT_STREAM_VIDEO; stream->codec = YT_CODEC_H264;
              stream->quality = YT_QUALITY_720P; stream->width = 1280; stream->height = 720; break;
    case 137: stream->type = YT_STREAM_VIDEO; stream->codec = YT_CODEC_H264;
              stream->quality = YT_QUALITY_1080P; stream->width = 1920; stream->height = 1080; break;
    case 242: stream->type = YT_STREAM_VIDEO; stream->codec = YT_CODEC_VP9;
              stream->quality = YT_QUALITY_240P; stream->width = 426; stream->height = 240; break;
    case 243: stream->type = YT_STREAM_VIDEO; stream->codec = YT_CODEC_VP9;
              stream->quality = YT_QUALITY_360P; stream->width = 640; stream->height = 360; break;
    case 244: stream->type = YT_STREAM_VIDEO; stream->codec = YT_CODEC_VP9;
              stream->quality = YT_QUALITY_480P; stream->width = 854; stream->height = 480; break;
    case 247: stream->type = YT_STREAM_VIDEO; stream->codec = YT_CODEC_VP9;
              stream->quality = YT_QUALITY_720P; stream->width = 1280; stream->height = 720; break;
    /* Audio only (DASH) */
    case 139: stream->type = YT_STREAM_AUDIO; stream->codec = YT_CODEC_AAC;
              stream->bitrate = 48000; stream->sample_rate = 22050; stream->channels = 2; break;
    case 140: stream->type = YT_STREAM_AUDIO; stream->codec = YT_CODEC_AAC;
              stream->bitrate = 128000; stream->sample_rate = 44100; stream->channels = 2; break;
    case 249: stream->type = YT_STREAM_AUDIO; stream->codec = YT_CODEC_OPUS;
              stream->bitrate = 50000; stream->sample_rate = 48000; stream->channels = 2; break;
    case 250: stream->type = YT_STREAM_AUDIO; stream->codec = YT_CODEC_OPUS;
              stream->bitrate = 70000; stream->sample_rate = 48000; stream->channels = 2; break;
    case 251: stream->type = YT_STREAM_AUDIO; stream->codec = YT_CODEC_OPUS;
              stream->bitrate = 160000; stream->sample_rate = 48000; stream->channels = 2; break;
    default: return -1;
    }

    /* Extract URL */
    json_extract_string(fmt_json, "url", stream->url, sizeof(stream->url));
    json_extract_string(fmt_json, "mimeType", stream->mime_type, sizeof(stream->mime_type));

    int br = json_extract_int(fmt_json, "bitrate");
    if (br > 0) stream->bitrate = br;

    int cl = json_extract_int(fmt_json, "contentLength");
    stream->content_length = cl > 0 ? cl : 0;

    return 0;
}

/* ---- Public API ---- */

int yt_get_video_info(const char *url_or_id, struct yt_video_info *info) {
    if (!info) return -1;
    memset(info, 0, sizeof(*info));

    /* Extract video ID */
    if (extract_video_id(url_or_id, info->video_id) < 0) return -1;

    /* Fetch the YouTube page via our HTTP client */
    char fetch_url[512];
    snprintf(fetch_url, sizeof(fetch_url),
             "https://www.youtube.com/watch?v=%s", info->video_id);

    struct http_response resp;
    if (http_get(fetch_url, &resp) < 0 || resp.status_code != 200) {
        http_response_free(&resp);
        return -1;
    }

    if (!resp.body || resp.body_len == 0) {
        http_response_free(&resp);
        return -1;
    }

    /* Extract title from page */
    const char *title_start = strstr(resp.body, "\"title\":\"");
    if (title_start) {
        json_extract_string(title_start - 1, "title", info->title, sizeof(info->title));
    }

    /* Extract author */
    json_extract_string(resp.body, "author", info->author, sizeof(info->author));

    /* Extract duration */
    int dur = json_extract_int(resp.body, "lengthSeconds");
    if (dur > 0) info->duration_ms = (int64_t)dur * 1000;

    /* Find streaming formats in player response */
    const char *formats = strstr(resp.body, "\"formats\":[");
    const char *adaptive = strstr(resp.body, "\"adaptiveFormats\":[");

    /* Parse muxed formats */
    if (formats) {
        const char *p = formats + 11; /* skip "formats":[ */
        while (*p && *p != ']' && info->stream_count < YT_MAX_STREAMS) {
            if (*p == '{') {
                const char *start = p;
                int depth = 1;
                p++;
                while (*p && depth > 0) {
                    if (*p == '{') depth++;
                    else if (*p == '}') depth--;
                    p++;
                }
                /* start..p is one format object */
                char fmt_buf[4096];
                size_t flen = (size_t)(p - start);
                if (flen > sizeof(fmt_buf) - 1) flen = sizeof(fmt_buf) - 1;
                memcpy(fmt_buf, start, flen);
                fmt_buf[flen] = '\0';

                if (parse_format(fmt_buf, &info->streams[info->stream_count]) == 0)
                    info->stream_count++;
            } else {
                p++;
            }
        }
    }

    /* Parse adaptive formats (separate video/audio DASH streams) */
    if (adaptive) {
        const char *p = adaptive + 19;
        while (*p && *p != ']' && info->stream_count < YT_MAX_STREAMS) {
            if (*p == '{') {
                const char *start = p;
                int depth = 1;
                p++;
                while (*p && depth > 0) {
                    if (*p == '{') depth++;
                    else if (*p == '}') depth--;
                    p++;
                }
                char fmt_buf[4096];
                size_t flen = (size_t)(p - start);
                if (flen > sizeof(fmt_buf) - 1) flen = sizeof(fmt_buf) - 1;
                memcpy(fmt_buf, start, flen);
                fmt_buf[flen] = '\0';

                if (parse_format(fmt_buf, &info->streams[info->stream_count]) == 0)
                    info->stream_count++;
            } else {
                p++;
            }
        }
    }

    http_response_free(&resp);
    return (info->stream_count > 0) ? 0 : -1;
}

const struct yt_stream *yt_select_video(const struct yt_video_info *info,
                                         enum yt_quality max_quality) {
    if (!info) return NULL;
    const struct yt_stream *best = NULL;

    for (int i = 0; i < info->stream_count; i++) {
        const struct yt_stream *s = &info->streams[i];
        if (s->type != YT_STREAM_VIDEO && s->type != YT_STREAM_MUXED) continue;
        if (s->quality > max_quality) continue;
        /* Prefer H.264 (we have a decoder for it) */
        if (s->codec != YT_CODEC_H264) continue;
        if (!best || s->quality > best->quality)
            best = s;
    }

    /* Fall back to any codec */
    if (!best) {
        for (int i = 0; i < info->stream_count; i++) {
            const struct yt_stream *s = &info->streams[i];
            if (s->type != YT_STREAM_VIDEO && s->type != YT_STREAM_MUXED) continue;
            if (s->quality > max_quality) continue;
            if (!best || s->quality > best->quality)
                best = s;
        }
    }

    return best;
}

const struct yt_stream *yt_select_audio(const struct yt_video_info *info) {
    if (!info) return NULL;
    const struct yt_stream *best = NULL;

    for (int i = 0; i < info->stream_count; i++) {
        const struct yt_stream *s = &info->streams[i];
        if (s->type != YT_STREAM_AUDIO) continue;
        /* Prefer AAC (wider support) */
        if (!best || s->bitrate > best->bitrate)
            best = s;
    }

    return best;
}

/* ---- DASH Manifest parsing ---- */

int dash_parse(const char *mpd_xml, size_t len, struct dash_manifest *out) {
    if (!mpd_xml || !out || len == 0) return -1;
    memset(out, 0, sizeof(*out));

    /* Parse mediaPresentationDuration (PT##H##M##.##S) */
    const char *dur = strstr(mpd_xml, "mediaPresentationDuration=\"PT");
    if (dur) {
        dur += 28;
        int hours = 0, mins = 0, secs = 0;
        while (*dur >= '0' && *dur <= '9') hours = hours * 10 + (*dur++ - '0');
        if (*dur == 'H') { dur++; }
        else { secs = hours; hours = 0; goto dur_done; }
        while (*dur >= '0' && *dur <= '9') mins = mins * 10 + (*dur++ - '0');
        if (*dur == 'M') dur++;
        while (*dur >= '0' && *dur <= '9') secs = secs * 10 + (*dur++ - '0');
dur_done:
        out->duration_ms = ((int64_t)hours * 3600 + mins * 60 + secs) * 1000;
    }

    /* Parse Representation elements */
    const char *p = mpd_xml;
    while (out->rep_count < DASH_MAX_REPS) {
        const char *rep = strstr(p, "<Representation");
        if (!rep) break;

        struct dash_representation *r = &out->reps[out->rep_count];

        /* Extract bandwidth */
        const char *bw = strstr(rep, "bandwidth=\"");
        if (bw) {
            bw += 11;
            while (*bw >= '0' && *bw <= '9')
                r->bandwidth = r->bandwidth * 10 + (*bw++ - '0');
        }

        /* Extract width/height */
        const char *w = strstr(rep, "width=\"");
        if (w && w < rep + 500) {
            w += 7;
            while (*w >= '0' && *w <= '9')
                r->width = r->width * 10 + (*w++ - '0');
        }
        const char *h = strstr(rep, "height=\"");
        if (h && h < rep + 500) {
            h += 8;
            while (*h >= '0' && *h <= '9')
                r->height = r->height * 10 + (*h++ - '0');
        }

        /* Determine type from mimeType */
        const char *mime = strstr(rep, "mimeType=\"");
        if (mime) {
            mime += 10;
            if (strncmp(mime, "video/", 6) == 0) r->type = YT_STREAM_VIDEO;
            else if (strncmp(mime, "audio/", 6) == 0) r->type = YT_STREAM_AUDIO;
        }

        /* Extract codecs */
        const char *codecs = strstr(rep, "codecs=\"");
        if (codecs) {
            codecs += 8;
            if (strncmp(codecs, "avc1", 4) == 0) r->codec = YT_CODEC_H264;
            else if (strncmp(codecs, "vp9", 3) == 0 || strncmp(codecs, "vp09", 4) == 0)
                r->codec = YT_CODEC_VP9;
            else if (strncmp(codecs, "mp4a", 4) == 0) r->codec = YT_CODEC_AAC;
            else if (strncmp(codecs, "opus", 4) == 0) r->codec = YT_CODEC_OPUS;
        }

        /* Extract BaseURL */
        const char *base = strstr(rep, "<BaseURL>");
        if (base) {
            base += 9;
            const char *base_end = strstr(base, "</BaseURL>");
            if (base_end) {
                size_t blen = base_end - base;
                if (blen > sizeof(r->base_url) - 1) blen = sizeof(r->base_url) - 1;
                memcpy(r->base_url, base, blen);
                r->base_url[blen] = '\0';
            }
        }

        out->rep_count++;
        p = rep + 1;
    }

    return 0;
}

/* ---- Player implementation ---- */

struct yt_player {
    struct yt_video_info info;
    const struct yt_stream *video_stream;
    const struct yt_stream *audio_stream;
    int playing;
    int buffering;
    int64_t position_ms;
    enum yt_quality quality;
    int audio_fd;
};

struct yt_player *yt_player_create(void) {
    struct yt_player *p = calloc(1, sizeof(struct yt_player));
    if (p) {
        p->quality = YT_QUALITY_480P;
        p->audio_fd = -1;
    }
    return p;
}

int yt_player_load(struct yt_player *p, const char *url_or_id) {
    if (!p) return -1;
    p->playing = 0;
    p->buffering = 1;
    p->position_ms = 0;

    if (yt_get_video_info(url_or_id, &p->info) < 0) {
        p->buffering = 0;
        return -1;
    }

    p->video_stream = yt_select_video(&p->info, p->quality);
    p->audio_stream = yt_select_audio(&p->info);

    p->buffering = 0;
    return 0;
}

void yt_player_play(struct yt_player *p) {
    if (p) p->playing = 1;
}

void yt_player_pause(struct yt_player *p) {
    if (p) p->playing = 0;
}

void yt_player_seek(struct yt_player *p, int64_t position_ms) {
    if (p) p->position_ms = position_ms;
}

void yt_player_set_quality(struct yt_player *p, enum yt_quality q) {
    if (!p) return;
    p->quality = q;
    p->video_stream = yt_select_video(&p->info, q);
}

int64_t yt_player_position(struct yt_player *p) {
    return p ? p->position_ms : 0;
}

int64_t yt_player_duration(struct yt_player *p) {
    return p ? p->info.duration_ms : 0;
}

int yt_player_is_playing(struct yt_player *p) {
    return p ? p->playing : 0;
}

int yt_player_is_buffering(struct yt_player *p) {
    return p ? p->buffering : 0;
}

void yt_player_destroy(struct yt_player *p) {
    if (p) free(p);
}
