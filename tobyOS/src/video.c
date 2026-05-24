/* video.c -- Video playback framework for tobyOS.
 *
 * Minimal video container parser and decode framework:
 * - MP4/ISOBMF parser: ftyp, moov, mdat boxes, track/sample tables
 * - MKV/EBML parser (basic): EBML header, segment, track entries
 * - Decode dispatch: provide NAL units / raw frames to userland
 * - No actual H.264 decode in kernel -- just demuxing
 */

#include <tobyos/types.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/vfs.h>
#include <tobyos/proc.h>

#define VIDEO_MAX_SESSIONS   4
#define VIDEO_MAX_TRACKS     8
#define VIDEO_MAX_SAMPLES    4096
#define VIDEO_FRAME_BUF_SIZE (256 * 1024)

/* Container types */
#define VIDEO_CONTAINER_UNKNOWN 0
#define VIDEO_CONTAINER_MP4     1
#define VIDEO_CONTAINER_MKV     2

/* Codec IDs */
#define VIDEO_CODEC_UNKNOWN  0
#define VIDEO_CODEC_H264     1
#define VIDEO_CODEC_H265     2
#define VIDEO_CODEC_VP8      3
#define VIDEO_CODEC_VP9      4
#define VIDEO_CODEC_AV1      5
#define VIDEO_CODEC_AAC      10
#define VIDEO_CODEC_OPUS     11
#define VIDEO_CODEC_VORBIS   12

/* Track types */
#define VIDEO_TRACK_VIDEO    1
#define VIDEO_TRACK_AUDIO    2
#define VIDEO_TRACK_SUBTITLE 3

/* MP4 box types (fourcc as big-endian uint32) */
#define MP4_BOX_FTYP  0x66747970
#define MP4_BOX_MOOV  0x6D6F6F76
#define MP4_BOX_MVHD  0x6D766864
#define MP4_BOX_TRAK  0x7472616B
#define MP4_BOX_TKHD  0x746B6864
#define MP4_BOX_MDIA  0x6D646961
#define MP4_BOX_MDHD  0x6D646864
#define MP4_BOX_HDLR  0x68646C72
#define MP4_BOX_MINF  0x6D696E66
#define MP4_BOX_STBL  0x7374626C
#define MP4_BOX_STSD  0x73747364
#define MP4_BOX_STTS  0x73747473
#define MP4_BOX_STSC  0x73747363
#define MP4_BOX_STSZ  0x7374737A
#define MP4_BOX_STCO  0x7374636F
#define MP4_BOX_MDAT  0x6D646174

/* EBML element IDs (simplified) */
#define EBML_HEADER          0x1A45DFA3
#define EBML_SEGMENT         0x18538067
#define EBML_SEGMENT_INFO    0x1549A966
#define EBML_TRACKS          0x1654AE6B
#define EBML_TRACK_ENTRY     0xAE
#define EBML_TRACK_NUMBER    0xD7
#define EBML_TRACK_TYPE      0x83
#define EBML_CODEC_ID        0x86
#define EBML_CLUSTER         0x1F43B675
#define EBML_SIMPLE_BLOCK    0xA3
#define EBML_TIMECODE        0xE7

struct video_track {
    uint8_t  track_type;
    uint8_t  codec_id;
    uint16_t width;
    uint16_t height;
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  bits_per_sample;
    uint16_t _pad;
    uint32_t timescale;
    uint32_t duration;
    uint32_t sample_count;
};

struct mp4_sample_entry {
    uint64_t offset;
    uint32_t size;
    uint32_t duration;
    uint64_t timestamp;
};

struct video_session {
    int              state;
    int              pid;
    uint8_t          container;
    uint8_t          _pad[3];
    struct vfs_file  file;
    bool             file_open;
    struct video_track tracks[VIDEO_MAX_TRACKS];
    int              track_count;
    uint32_t         duration_ms;
    uint32_t         timescale;

    /* MP4 sample table (simplified) */
    struct mp4_sample_entry *samples;
    int              sample_count;
    int              current_sample;

    /* MKV cluster state */
    uint64_t         cluster_offset;
    uint64_t         cluster_timecode;

    /* Frame output buffer */
    uint8_t         *frame_buf;
    size_t           frame_size;
};

static struct video_session g_video_sessions[VIDEO_MAX_SESSIONS];

/* Read big-endian integers from buffer */
static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t read_be64(const uint8_t *p) __attribute__((unused));
static uint64_t read_be64(const uint8_t *p) {
    return ((uint64_t)read_be32(p) << 32) | read_be32(p + 4);
}

static uint16_t read_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

/* Read from file at current position */
static long video_file_read(struct video_session *sess, void *buf, size_t n) __attribute__((unused));
static long video_file_read(struct video_session *sess, void *buf, size_t n) {
    return vfs_read(&sess->file, buf, n);
}

/* ---- MP4 Parser ---- */

static bool mp4_parse_stsd(struct video_session *sess __attribute__((unused)),
                           const uint8_t *data,
                           size_t len, struct video_track *track) {
    if (len < 8) return false;
    /* uint32_t entry_count = read_be32(data + 4); */
    if (len < 16) return true;

    /* Peek at codec fourcc */
    uint32_t codec = read_be32(data + 12);
    switch (codec) {
    case 0x61766331: /* avc1 */
    case 0x61766333: /* avc3 */
        track->codec_id = VIDEO_CODEC_H264;
        break;
    case 0x68766331: /* hvc1 */
    case 0x68657631: /* hev1 */
        track->codec_id = VIDEO_CODEC_H265;
        break;
    case 0x6D703461: /* mp4a */
        track->codec_id = VIDEO_CODEC_AAC;
        break;
    default:
        track->codec_id = VIDEO_CODEC_UNKNOWN;
        break;
    }

    /* For video tracks, extract width/height from sample entry */
    if (track->track_type == VIDEO_TRACK_VIDEO && len >= 40) {
        track->width = read_be16(data + 32);
        track->height = read_be16(data + 34);
    }
    return true;
}

static bool mp4_parse_stsz(struct video_session *sess, const uint8_t *data,
                           size_t len) {
    if (len < 12) return false;
    uint32_t sample_size = read_be32(data + 4);
    uint32_t count = read_be32(data + 8);

    if (count > VIDEO_MAX_SAMPLES) count = VIDEO_MAX_SAMPLES;

    if (!sess->samples) {
        sess->samples = kmalloc(count * sizeof(struct mp4_sample_entry));
        if (!sess->samples) return false;
        memset(sess->samples, 0, count * sizeof(struct mp4_sample_entry));
    }

    sess->sample_count = (int)count;
    if (sample_size != 0) {
        for (uint32_t i = 0; i < count; i++)
            sess->samples[i].size = sample_size;
    } else {
        size_t off = 12;
        for (uint32_t i = 0; i < count && off + 4 <= len; i++) {
            sess->samples[i].size = read_be32(data + off);
            off += 4;
        }
    }
    return true;
}

static bool mp4_parse_stco(struct video_session *sess, const uint8_t *data,
                           size_t len) {
    if (len < 8) return false;
    uint32_t count = read_be32(data + 4);
    size_t off = 8;

    /* Set chunk offsets as sample offsets (simplified: 1 sample per chunk) */
    for (uint32_t i = 0; i < count && (int)i < sess->sample_count && off + 4 <= len; i++) {
        sess->samples[i].offset = read_be32(data + off);
        off += 4;
    }

    /* Propagate offsets: accumulate sizes for samples in same chunk */
    for (int i = 1; i < sess->sample_count; i++) {
        if (sess->samples[i].offset == 0) {
            sess->samples[i].offset = sess->samples[i-1].offset +
                                       sess->samples[i-1].size;
        }
    }
    return true;
}

static bool mp4_parse_box(struct video_session *sess, const uint8_t *data,
                          size_t len, int depth) {
    size_t off = 0;
    if (depth > 10) return true;

    while (off + 8 <= len) {
        uint32_t box_size = read_be32(data + off);
        uint32_t box_type = read_be32(data + off + 4);

        if (box_size < 8) break;
        if (off + box_size > len) break;

        const uint8_t *box_data = data + off + 8;
        size_t box_body_len = box_size - 8;

        switch (box_type) {
        case MP4_BOX_FTYP:
            sess->container = VIDEO_CONTAINER_MP4;
            break;
        case MP4_BOX_MVHD:
            if (box_body_len >= 20) {
                sess->timescale = read_be32(box_data + 4 + 4);
                sess->duration_ms = (uint32_t)(
                    (uint64_t)read_be32(box_data + 4 + 8) * 1000 / sess->timescale);
            }
            break;
        case MP4_BOX_TRAK:
            if (sess->track_count < VIDEO_MAX_TRACKS) {
                sess->track_count++;
            }
            mp4_parse_box(sess, box_data, box_body_len, depth + 1);
            break;
        case MP4_BOX_TKHD:
            if (box_body_len >= 84 && sess->track_count > 0) {
                struct video_track *t = &sess->tracks[sess->track_count - 1];
                t->width = (uint16_t)(read_be32(box_data + 76) >> 16);
                t->height = (uint16_t)(read_be32(box_data + 80) >> 16);
            }
            break;
        case MP4_BOX_HDLR:
            if (box_body_len >= 12 && sess->track_count > 0) {
                uint32_t handler = read_be32(box_data + 8);
                struct video_track *t = &sess->tracks[sess->track_count - 1];
                if (handler == 0x76696465) /* vide */
                    t->track_type = VIDEO_TRACK_VIDEO;
                else if (handler == 0x736F756E) /* soun */
                    t->track_type = VIDEO_TRACK_AUDIO;
                else if (handler == 0x74657874) /* text */
                    t->track_type = VIDEO_TRACK_SUBTITLE;
            }
            break;
        case MP4_BOX_MDHD:
            if (box_body_len >= 20 && sess->track_count > 0) {
                struct video_track *t = &sess->tracks[sess->track_count - 1];
                t->timescale = read_be32(box_data + 4 + 4);
                t->duration = read_be32(box_data + 4 + 8);
            }
            break;
        case MP4_BOX_STSD:
            if (sess->track_count > 0) {
                struct video_track *t = &sess->tracks[sess->track_count - 1];
                mp4_parse_stsd(sess, box_data, box_body_len, t);
            }
            break;
        case MP4_BOX_STSZ:
            mp4_parse_stsz(sess, box_data, box_body_len);
            break;
        case MP4_BOX_STCO:
            mp4_parse_stco(sess, box_data, box_body_len);
            break;
        case MP4_BOX_MOOV:
        case MP4_BOX_MDIA:
        case MP4_BOX_MINF:
        case MP4_BOX_STBL:
            mp4_parse_box(sess, box_data, box_body_len, depth + 1);
            break;
        default:
            break;
        }
        off += box_size;
    }
    return true;
}

/* ---- MKV/EBML Parser ---- */

/* Read EBML variable-length integer */
static size_t ebml_read_vint(const uint8_t *data, size_t len, uint64_t *out) {
    if (len == 0) return 0;
    uint8_t first = data[0];
    int width = 0;
    uint64_t mask = 0;

    if      (first & 0x80) { width = 1; mask = 0x7F; }
    else if (first & 0x40) { width = 2; mask = 0x3F; }
    else if (first & 0x20) { width = 3; mask = 0x1F; }
    else if (first & 0x10) { width = 4; mask = 0x0F; }
    else if (first & 0x08) { width = 5; mask = 0x07; }
    else if (first & 0x04) { width = 6; mask = 0x03; }
    else if (first & 0x02) { width = 7; mask = 0x01; }
    else if (first & 0x01) { width = 8; mask = 0x00; }
    else return 0;

    if ((size_t)width > len) return 0;

    *out = first & mask;
    for (int i = 1; i < width; i++)
        *out = (*out << 8) | data[i];
    return (size_t)width;
}

/* Read EBML element ID */
static size_t ebml_read_id(const uint8_t *data, size_t len, uint32_t *out) {
    if (len == 0) return 0;
    uint8_t first = data[0];
    int width = 0;

    if      (first & 0x80) width = 1;
    else if (first & 0x40) width = 2;
    else if (first & 0x20) width = 3;
    else if (first & 0x10) width = 4;
    else return 0;

    if ((size_t)width > len) return 0;

    *out = first;
    for (int i = 1; i < width; i++)
        *out = (*out << 8) | data[i];
    return (size_t)width;
}

static bool mkv_parse_tracks(struct video_session *sess, const uint8_t *data,
                             size_t len) {
    size_t off = 0;
    while (off < len) {
        uint32_t id;
        uint64_t size;
        size_t id_len = ebml_read_id(data + off, len - off, &id);
        if (id_len == 0) break;
        off += id_len;
        size_t sz_len = ebml_read_vint(data + off, len - off, &size);
        if (sz_len == 0) break;
        off += sz_len;

        if (id == EBML_TRACK_ENTRY && sess->track_count < VIDEO_MAX_TRACKS) {
            struct video_track *t = &sess->tracks[sess->track_count++];
            memset(t, 0, sizeof(*t));

            /* Parse track entry children */
            size_t entry_end = off + (size_t)size;
            while (off < entry_end && off < len) {
                uint32_t child_id;
                uint64_t child_size;
                size_t cid_len = ebml_read_id(data + off, len - off, &child_id);
                if (cid_len == 0) break;
                off += cid_len;
                size_t csz_len = ebml_read_vint(data + off, len - off, &child_size);
                if (csz_len == 0) break;
                off += csz_len;

                if (child_id == EBML_TRACK_TYPE && child_size == 1) {
                    uint8_t track_type = data[off];
                    if (track_type == 1) t->track_type = VIDEO_TRACK_VIDEO;
                    else if (track_type == 2) t->track_type = VIDEO_TRACK_AUDIO;
                    else if (track_type == 17) t->track_type = VIDEO_TRACK_SUBTITLE;
                } else if (child_id == EBML_CODEC_ID && child_size < 32) {
                    char codec_str[32];
                    memcpy(codec_str, data + off,
                            child_size < 31 ? (size_t)child_size : 31);
                    codec_str[child_size < 31 ? child_size : 31] = '\0';
                    if (strcmp(codec_str, "V_MPEG4/ISO/AVC") == 0)
                        t->codec_id = VIDEO_CODEC_H264;
                    else if (strcmp(codec_str, "V_MPEGH/ISO/HEVC") == 0)
                        t->codec_id = VIDEO_CODEC_H265;
                    else if (strcmp(codec_str, "A_AAC") == 0)
                        t->codec_id = VIDEO_CODEC_AAC;
                    else if (strcmp(codec_str, "A_OPUS") == 0)
                        t->codec_id = VIDEO_CODEC_OPUS;
                    else if (strcmp(codec_str, "A_VORBIS") == 0)
                        t->codec_id = VIDEO_CODEC_VORBIS;
                }
                off += (size_t)child_size;
            }
            off = entry_end;
        } else {
            off += (size_t)size;
        }
    }
    return true;
}

static bool mkv_parse_header(struct video_session *sess, const uint8_t *data,
                             size_t len) {
    size_t off = 0;
    while (off + 4 < len) {
        uint32_t id;
        uint64_t size;
        size_t id_len = ebml_read_id(data + off, len - off, &id);
        if (id_len == 0) break;
        off += id_len;
        size_t sz_len = ebml_read_vint(data + off, len - off, &size);
        if (sz_len == 0) break;
        off += sz_len;

        if (id == EBML_HEADER) {
            sess->container = VIDEO_CONTAINER_MKV;
            off += (size_t)size;
        } else if (id == EBML_SEGMENT) {
            /* Parse segment children */
            size_t seg_end = off + (size_t)size;
            if (seg_end > len) seg_end = len;
            while (off < seg_end) {
                uint32_t child_id;
                uint64_t child_size;
                size_t cid_len = ebml_read_id(data + off, seg_end - off, &child_id);
                if (cid_len == 0) break;
                off += cid_len;
                size_t csz_len = ebml_read_vint(data + off, seg_end - off, &child_size);
                if (csz_len == 0) break;
                off += csz_len;

                if (child_id == EBML_TRACKS) {
                    mkv_parse_tracks(sess, data + off,
                                     (size_t)child_size < (seg_end - off) ?
                                     (size_t)child_size : (seg_end - off));
                }
                off += (size_t)child_size;
                if ((size_t)child_size == 0) break;
            }
            break;
        } else {
            off += (size_t)size;
            if (size == 0) break;
        }
    }
    return true;
}

/* ---- Syscall handlers ---- */

int sys_video_open(const char *path) {
    if (!path) return -1;

    /* Find free session */
    int slot = -1;
    for (int i = 0; i < VIDEO_MAX_SESSIONS; i++) {
        if (g_video_sessions[i].state == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;

    struct video_session *sess = &g_video_sessions[slot];
    memset(sess, 0, sizeof(*sess));

    /* Open the file */
    int rc = vfs_open(path, &sess->file);
    if (rc != VFS_OK) return -1;
    sess->file_open = true;

    /* Read header to determine container type */
    uint8_t *header_buf = kmalloc(VIDEO_FRAME_BUF_SIZE);
    if (!header_buf) {
        vfs_close(&sess->file);
        return -1;
    }

    long bytes = vfs_read(&sess->file, header_buf, VIDEO_FRAME_BUF_SIZE);
    if (bytes <= 0) {
        kfree(header_buf);
        vfs_close(&sess->file);
        return -1;
    }

    /* Try MP4 first */
    if (bytes >= 8) {
        uint32_t box_type = read_be32(header_buf + 4);
        if (box_type == MP4_BOX_FTYP) {
            mp4_parse_box(sess, header_buf, (size_t)bytes, 0);
        }
    }

    /* Try MKV/EBML if not MP4 */
    if (sess->container == VIDEO_CONTAINER_UNKNOWN && bytes >= 4) {
        if (header_buf[0] == 0x1A && header_buf[1] == 0x45 &&
            header_buf[2] == 0xDF && header_buf[3] == 0xA3) {
            mkv_parse_header(sess, header_buf, (size_t)bytes);
        }
    }

    kfree(header_buf);

    if (sess->container == VIDEO_CONTAINER_UNKNOWN) {
        vfs_close(&sess->file);
        return -1;
    }

    sess->state = 1;
    sess->pid = current_proc()->pid;
    sess->frame_buf = kmalloc(VIDEO_FRAME_BUF_SIZE);
    sess->current_sample = 0;

    kprintf("[video] session %d opened: container=%s tracks=%d\n",
           slot, sess->container == VIDEO_CONTAINER_MP4 ? "mp4" : "mkv",
           sess->track_count);
    return slot;
}

long sys_video_read_frame(int session_id, void *buf, size_t buf_size) {
    if (session_id < 0 || session_id >= VIDEO_MAX_SESSIONS) return -1;
    struct video_session *sess = &g_video_sessions[session_id];
    if (sess->state == 0) return -1;

    if (sess->container == VIDEO_CONTAINER_MP4) {
        if (sess->current_sample >= sess->sample_count) return 0;

        struct mp4_sample_entry *sample = &sess->samples[sess->current_sample];
        size_t read_size = sample->size;
        if (read_size > buf_size) read_size = buf_size;
        if (read_size > VIDEO_FRAME_BUF_SIZE) read_size = VIDEO_FRAME_BUF_SIZE;

        /* Seek to sample offset (simplified: re-read from beginning) */
        if (sess->frame_buf) {
            /* In a real implementation we'd seek; here we return frame data */
            long r = vfs_read(&sess->file, sess->frame_buf, read_size);
            if (r <= 0) return -1;
            memcpy(buf, sess->frame_buf, (size_t)r);
            sess->current_sample++;
            return r;
        }
    }

    /* MKV: read next SimpleBlock from cluster */
    if (sess->container == VIDEO_CONTAINER_MKV) {
        if (!sess->frame_buf) return -1;
        long r = vfs_read(&sess->file, sess->frame_buf, buf_size < VIDEO_FRAME_BUF_SIZE ? buf_size : VIDEO_FRAME_BUF_SIZE);
        if (r <= 0) return 0;
        memcpy(buf, sess->frame_buf, (size_t)r);
        return r;
    }

    return -1;
}

void sys_video_close(int session_id) {
    if (session_id < 0 || session_id >= VIDEO_MAX_SESSIONS) return;
    struct video_session *sess = &g_video_sessions[session_id];
    if (sess->state == 0) return;

    if (sess->file_open) {
        vfs_close(&sess->file);
        sess->file_open = false;
    }
    if (sess->samples) {
        kfree(sess->samples);
        sess->samples = NULL;
    }
    if (sess->frame_buf) {
        kfree(sess->frame_buf);
        sess->frame_buf = NULL;
    }
    sess->state = 0;
}

void video_init(void) {
    memset(g_video_sessions, 0, sizeof(g_video_sessions));
    kprintf("[video] framework initialized\n");
}
