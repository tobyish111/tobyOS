/* audio_mixer.c -- software audio mixer.
 *
 * Mixes multiple audio streams into a single output buffer for the
 * HDA (or future) audio output driver. Features:
 *
 *   - Up to 16 simultaneous audio streams
 *   - Per-stream volume (0-100) and master volume
 *   - 16-bit and 32-bit PCM format support
 *   - Nearest-neighbor sample rate conversion
 *   - Per-app volume tracking via PID association
 */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/spinlock.h>

/* ---- constants --------------------------------------------------- */

#define MIXER_MAX_STREAMS   16
#define MIXER_SAMPLE_RATE   48000
#define MIXER_CHANNELS      2
#define MIXER_BUF_SAMPLES   1024

#define MIXER_FMT_S16       0
#define MIXER_FMT_S32       1

/* ---- mixer stream ------------------------------------------------ */

struct mixer_stream {
    bool     active;
    int      pid;
    uint8_t  volume;            /* 0..100 */
    uint8_t  format;            /* MIXER_FMT_S16 or MIXER_FMT_S32 */
    uint32_t sample_rate;       /* source sample rate */

    /* Ring buffer of samples (interleaved stereo).
     * Stored as int32_t regardless of source format for uniform mixing. */
    int32_t  buf[MIXER_BUF_SAMPLES * MIXER_CHANNELS];
    uint32_t write_pos;
    uint32_t read_pos;
    uint32_t available;         /* samples (frames * channels) available */

    /* Sample rate conversion state: fractional accumulator. */
    uint64_t src_frac;
};

/* ---- mixer state ------------------------------------------------- */

static struct mixer_stream g_streams[MIXER_MAX_STREAMS];
static uint8_t  g_master_volume = 100;
static bool     g_mixer_muted;
static spinlock_t g_mixer_lock = SPINLOCK_INIT;
static uint64_t g_mixer_frames_mixed;

/* ---- internal helpers -------------------------------------------- */

static int32_t clamp16(int64_t v) {
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int32_t)v;
}

static inline int32_t apply_volume(int32_t sample, uint8_t vol) {
    return (int32_t)(((int64_t)sample * vol) / 100);
}

/* ---- public API -------------------------------------------------- */

int mixer_open_stream(int pid, uint32_t sample_rate, uint8_t format) {
    uint64_t flags = spin_lock_irqsave(&g_mixer_lock);
    for (int i = 0; i < MIXER_MAX_STREAMS; i++) {
        if (!g_streams[i].active) {
            memset(&g_streams[i], 0, sizeof(g_streams[i]));
            g_streams[i].active = true;
            g_streams[i].pid = pid;
            g_streams[i].volume = 100;
            g_streams[i].format = format;
            g_streams[i].sample_rate = sample_rate ? sample_rate : MIXER_SAMPLE_RATE;
            spin_unlock_irqrestore(&g_mixer_lock, flags);
            kprintf("[mixer] stream %d opened (pid=%d rate=%u fmt=%s)\n",
                    i, pid, g_streams[i].sample_rate,
                    format == MIXER_FMT_S16 ? "s16" : "s32");
            return i;
        }
    }
    spin_unlock_irqrestore(&g_mixer_lock, flags);
    return -1;
}

void mixer_close_stream(int handle) {
    if (handle < 0 || handle >= MIXER_MAX_STREAMS) return;
    uint64_t flags = spin_lock_irqsave(&g_mixer_lock);
    g_streams[handle].active = false;
    spin_unlock_irqrestore(&g_mixer_lock, flags);
}

void mixer_set_stream_volume(int handle, uint8_t vol) {
    if (handle < 0 || handle >= MIXER_MAX_STREAMS) return;
    if (vol > 100) vol = 100;
    g_streams[handle].volume = vol;
}

uint8_t mixer_get_stream_volume(int handle) {
    if (handle < 0 || handle >= MIXER_MAX_STREAMS) return 0;
    return g_streams[handle].volume;
}

void mixer_set_master_volume(uint8_t vol) {
    if (vol > 100) vol = 100;
    g_master_volume = vol;
}

uint8_t mixer_get_master_volume(void) {
    return g_master_volume;
}

void mixer_set_mute(bool muted) {
    g_mixer_muted = muted;
}

bool mixer_get_mute(void) {
    return g_mixer_muted;
}

/* Write PCM data into a stream's ring buffer. `data` points to
 * interleaved stereo samples in the stream's native format. `frames`
 * is the number of stereo frames. Returns the number of frames
 * actually written (may be less if the ring is full). */
uint32_t mixer_write(int handle, const void *data, uint32_t frames) {
    if (handle < 0 || handle >= MIXER_MAX_STREAMS) return 0;
    struct mixer_stream *s = &g_streams[handle];
    if (!s->active) return 0;

    uint64_t flags = spin_lock_irqsave(&g_mixer_lock);
    uint32_t capacity = MIXER_BUF_SAMPLES * MIXER_CHANNELS;
    uint32_t written = 0;

    for (uint32_t f = 0; f < frames && s->available < capacity - 1; f++) {
        int32_t left, right;

        if (s->format == MIXER_FMT_S16) {
            const int16_t *src = (const int16_t *)data + f * 2;
            left  = (int32_t)src[0];
            right = (int32_t)src[1];
        } else {
            const int32_t *src = (const int32_t *)data + f * 2;
            left  = src[0] >> 16;
            right = src[1] >> 16;
        }

        s->buf[s->write_pos]     = left;
        s->buf[s->write_pos + 1] = right;
        s->write_pos = (s->write_pos + 2) % capacity;
        s->available += 2;
        written++;
    }

    spin_unlock_irqrestore(&g_mixer_lock, flags);
    return written;
}

/* Mix all active streams into a 16-bit stereo output buffer.
 * `out` must have room for `frames * 2` int16_t values.
 * Returns the number of frames actually produced. */
uint32_t mixer_mix(int16_t *out, uint32_t frames) {
    uint64_t flags = spin_lock_irqsave(&g_mixer_lock);
    uint32_t capacity = MIXER_BUF_SAMPLES * MIXER_CHANNELS;

    for (uint32_t f = 0; f < frames; f++) {
        int64_t left_acc  = 0;
        int64_t right_acc = 0;
        for (int i = 0; i < MIXER_MAX_STREAMS; i++) {
            struct mixer_stream *s = &g_streams[i];
            if (!s->active || s->available < 2) continue;

            uint64_t step = ((uint64_t)s->sample_rate << 32) / MIXER_SAMPLE_RATE;
            s->src_frac += step;
            uint32_t advance = (uint32_t)(s->src_frac >> 32);
            s->src_frac &= 0xFFFFFFFFULL;

            int32_t left = 0, right = 0;
            for (uint32_t a = 0; a < advance && s->available >= 2; a++) {
                left  = s->buf[s->read_pos];
                right = s->buf[s->read_pos + 1];
                s->read_pos = (s->read_pos + 2) % capacity;
                s->available -= 2;
            }

            left  = apply_volume(left,  s->volume);
            right = apply_volume(right, s->volume);
            left_acc  += left;
            right_acc += right;
        }

        if (g_mixer_muted || g_master_volume == 0) {
            out[f * 2]     = 0;
            out[f * 2 + 1] = 0;
        } else {
            left_acc  = apply_volume((int32_t)left_acc,  g_master_volume);
            right_acc = apply_volume((int32_t)right_acc, g_master_volume);
            out[f * 2]     = (int16_t)clamp16(left_acc);
            out[f * 2 + 1] = (int16_t)clamp16(right_acc);
        }
    }

    g_mixer_frames_mixed += frames;
    spin_unlock_irqrestore(&g_mixer_lock, flags);
    return frames;
}

/* ---- per-app volume tracking ------------------------------------- */

void mixer_set_app_volume(int pid, uint8_t vol) {
    if (vol > 100) vol = 100;
    uint64_t flags = spin_lock_irqsave(&g_mixer_lock);
    for (int i = 0; i < MIXER_MAX_STREAMS; i++) {
        if (g_streams[i].active && g_streams[i].pid == pid)
            g_streams[i].volume = vol;
    }
    spin_unlock_irqrestore(&g_mixer_lock, flags);
}

uint8_t mixer_get_app_volume(int pid) {
    for (int i = 0; i < MIXER_MAX_STREAMS; i++) {
        if (g_streams[i].active && g_streams[i].pid == pid)
            return g_streams[i].volume;
    }
    return 0;
}

/* ---- diagnostics ------------------------------------------------- */

int mixer_active_stream_count(void) {
    int count = 0;
    for (int i = 0; i < MIXER_MAX_STREAMS; i++) {
        if (g_streams[i].active) count++;
    }
    return count;
}

uint64_t mixer_total_frames(void) {
    return g_mixer_frames_mixed;
}

/* ---- init -------------------------------------------------------- */

void audio_mixer_init(void) {
    memset(g_streams, 0, sizeof(g_streams));
    g_master_volume = 100;
    g_mixer_muted = false;
    g_mixer_frames_mixed = 0;
    kprintf("[mixer] software audio mixer initialized (%d streams max, %u Hz)\n",
            MIXER_MAX_STREAMS, MIXER_SAMPLE_RATE);
}
