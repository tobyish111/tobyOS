/* audio_engine.c -- Kernel audio engine for tobyOS.
 *
 * Implements an audio graph with source nodes, mixer, and output.
 * Provides software sample rate conversion, format conversion,
 * ring buffer for DMA output to HDA controller, and per-process
 * audio stream management.
 */

#include <tobyos/types.h>
#include <tobyos/heap.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/proc.h>
#include <tobyos/pit.h>

#define AUDIO_MAX_STREAMS       16
#define AUDIO_RING_SIZE         (64 * 1024)  /* 64KB ring buffer */
#define AUDIO_MIX_BUF_SAMPLES  1024
#define AUDIO_DEFAULT_RATE      48000
#define AUDIO_DEFAULT_CHANNELS  2
#define AUDIO_DEFAULT_BITS      16
#define AUDIO_MAX_VOLUME        100

/* Audio sample formats */
#define AUDIO_FMT_PCM8          0
#define AUDIO_FMT_PCM16         1
#define AUDIO_FMT_PCM24         2
#define AUDIO_FMT_PCM32         3
#define AUDIO_FMT_FLOAT32       4

/* Stream states */
#define AUDIO_STREAM_FREE       0
#define AUDIO_STREAM_OPEN       1
#define AUDIO_STREAM_PLAYING    2
#define AUDIO_STREAM_PAUSED     3

/* Ring buffer for DMA output */
struct audio_ring {
    int16_t *buf;
    size_t   size;
    size_t   write_pos;
    size_t   read_pos;
    size_t   count;
};

/* Per-process audio stream */
struct audio_stream {
    int      state;
    int      pid;
    uint32_t sample_rate;
    uint8_t  channels;
    uint8_t  format;
    uint8_t  volume;
    uint8_t  _pad;
    struct audio_ring ring;
};

/* Audio engine state */
static struct audio_stream g_audio_streams[AUDIO_MAX_STREAMS];
static int32_t g_audio_mix_buf[AUDIO_MIX_BUF_SAMPLES * 2];
static int16_t g_audio_out_buf[AUDIO_MIX_BUF_SAMPLES * 2];
static struct audio_ring g_audio_dma_ring;
static uint32_t g_audio_master_volume = 80;
static bool g_audio_engine_ready;

static void audio_ring_init(struct audio_ring *r, size_t samples) {
    r->size = samples;
    r->buf = kmalloc(samples * sizeof(int16_t));
    r->write_pos = 0;
    r->read_pos = 0;
    r->count = 0;
}

static size_t audio_ring_write(struct audio_ring *r, const int16_t *data, size_t n) {
    size_t written = 0;
    while (written < n && r->count < r->size) {
        r->buf[r->write_pos] = data[written];
        r->write_pos = (r->write_pos + 1) % r->size;
        r->count++;
        written++;
    }
    return written;
}

static size_t audio_ring_read(struct audio_ring *r, int16_t *data, size_t n) {
    size_t read = 0;
    while (read < n && r->count > 0) {
        data[read] = r->buf[r->read_pos];
        r->read_pos = (r->read_pos + 1) % r->size;
        r->count--;
        read++;
    }
    return read;
}

/* Convert any format to 16-bit PCM */
static void audio_convert_to_pcm16(const void *src, int16_t *dst, size_t samples,
                                   uint8_t format) {
    switch (format) {
    case AUDIO_FMT_PCM8: {
        const uint8_t *s = (const uint8_t *)src;
        for (size_t i = 0; i < samples; i++)
            dst[i] = (int16_t)((s[i] - 128) << 8);
        break;
    }
    case AUDIO_FMT_PCM16:
        memcpy(dst, src, samples * 2);
        break;
    case AUDIO_FMT_PCM24: {
        const uint8_t *s = (const uint8_t *)src;
        for (size_t i = 0; i < samples; i++) {
            int32_t val = (int32_t)(s[i*3] | (s[i*3+1] << 8) | (s[i*3+2] << 16));
            if (val & 0x800000) val |= (int32_t)0xFF000000;
            dst[i] = (int16_t)(val >> 8);
        }
        break;
    }
    case AUDIO_FMT_PCM32: {
        const int32_t *s = (const int32_t *)src;
        for (size_t i = 0; i < samples; i++)
            dst[i] = (int16_t)(s[i] >> 16);
        break;
    }
    case AUDIO_FMT_FLOAT32: {
        const uint32_t *s = (const uint32_t *)src;
        for (size_t i = 0; i < samples; i++) {
            /* Simplified float32 to int16: interpret as fixed-point approx */
            int32_t raw = (int32_t)s[i];
            int32_t exp = ((raw >> 23) & 0xFF) - 127;
            int32_t mantissa = (raw & 0x7FFFFF) | 0x800000;
            int32_t val;
            if (exp >= 0)
                val = mantissa >> (23 - exp);
            else
                val = mantissa >> (23 - exp);
            if (raw & 0x80000000) val = -val;
            if (val > 32767) val = 32767;
            if (val < -32768) val = -32768;
            dst[i] = (int16_t)val;
        }
        break;
    }
    default:
        memset(dst, 0, samples * 2);
        break;
    }
}

/* Linear interpolation sample rate conversion */
static size_t audio_resample(const int16_t *src, size_t src_samples,
                             int16_t *dst, size_t dst_samples,
                             uint32_t src_rate, uint32_t dst_rate,
                             uint8_t channels) {
    if (src_rate == dst_rate) {
        size_t copy = src_samples < dst_samples ? src_samples : dst_samples;
        memcpy(dst, src, copy * sizeof(int16_t));
        return copy;
    }

    size_t src_frames = src_samples / channels;
    size_t dst_frames = dst_samples / channels;
    uint64_t ratio = ((uint64_t)src_rate << 16) / dst_rate;
    size_t out_frames = 0;
    uint64_t pos = 0;

    for (size_t i = 0; i < dst_frames; i++) {
        size_t idx = (size_t)(pos >> 16);
        uint32_t frac = (uint32_t)(pos & 0xFFFF);

        if (idx + 1 >= src_frames) break;

        for (uint8_t ch = 0; ch < channels; ch++) {
            int32_t s0 = src[idx * channels + ch];
            int32_t s1 = src[(idx + 1) * channels + ch];
            int32_t val = s0 + ((s1 - s0) * (int32_t)frac >> 16);
            dst[i * channels + ch] = (int16_t)val;
        }
        pos += ratio;
        out_frames++;
    }
    return out_frames * channels;
}

/* Mix all active streams into the output buffer */
static size_t audio_mix(int16_t *out, size_t max_samples) {
    size_t frames = max_samples / 2;
    if (frames > AUDIO_MIX_BUF_SAMPLES) frames = AUDIO_MIX_BUF_SAMPLES;
    size_t out_samples = frames * 2;

    memset(g_audio_mix_buf, 0, out_samples * sizeof(int32_t));

    bool any_active = false;
    for (int i = 0; i < AUDIO_MAX_STREAMS; i++) {
        struct audio_stream *s = &g_audio_streams[i];
        if (s->state != AUDIO_STREAM_PLAYING) continue;
        if (s->ring.count == 0) continue;

        int16_t tmp[AUDIO_MIX_BUF_SAMPLES * 2];
        size_t read = audio_ring_read(&s->ring, tmp, out_samples);
        if (read == 0) continue;

        /* Resample if needed */
        int16_t resampled[AUDIO_MIX_BUF_SAMPLES * 2];
        int16_t *src_data = tmp;
        size_t src_count = read;

        if (s->sample_rate != AUDIO_DEFAULT_RATE) {
            src_count = audio_resample(tmp, read, resampled, out_samples,
                                       s->sample_rate, AUDIO_DEFAULT_RATE,
                                       s->channels);
            src_data = resampled;
        }

        /* Mix with volume */
        uint32_t vol = (uint32_t)s->volume * g_audio_master_volume / 100;
        for (size_t j = 0; j < src_count && j < out_samples; j++) {
            g_audio_mix_buf[j] += ((int32_t)src_data[j] * (int32_t)vol) / 100;
        }
        any_active = true;
    }

    if (!any_active) return 0;

    /* Clip and output */
    for (size_t i = 0; i < out_samples; i++) {
        int32_t val = g_audio_mix_buf[i];
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
        out[i] = (int16_t)val;
    }
    return out_samples;
}

/* ---- Syscall handlers ---- */

int sys_audio_open(uint32_t sample_rate, uint8_t channels, uint8_t format) {
    if (!g_audio_engine_ready) return -1;
    if (channels == 0 || channels > 2) return -1;
    if (format > AUDIO_FMT_FLOAT32) return -1;
    if (sample_rate < 8000 || sample_rate > 192000) return -1;

    for (int i = 0; i < AUDIO_MAX_STREAMS; i++) {
        struct audio_stream *s = &g_audio_streams[i];
        if (s->state == AUDIO_STREAM_FREE) {
            s->state = AUDIO_STREAM_OPEN;
            s->pid = current_proc()->pid;
            s->sample_rate = sample_rate;
            s->channels = channels;
            s->format = format;
            s->volume = 80;
            audio_ring_init(&s->ring, AUDIO_RING_SIZE / sizeof(int16_t));
            kprintf("[audio] stream %d opened: rate=%u ch=%u fmt=%u\n",
                   i, sample_rate, channels, format);
            return i;
        }
    }
    return -1;
}

long sys_audio_write(int stream_id, const void *samples, size_t count) {
    if (stream_id < 0 || stream_id >= AUDIO_MAX_STREAMS) return -1;
    struct audio_stream *s = &g_audio_streams[stream_id];
    if (s->state == AUDIO_STREAM_FREE) return -1;

    /* Convert to PCM16 */
    size_t sample_size = 2;
    switch (s->format) {
    case AUDIO_FMT_PCM8:  sample_size = 1; break;
    case AUDIO_FMT_PCM16: sample_size = 2; break;
    case AUDIO_FMT_PCM24: sample_size = 3; break;
    case AUDIO_FMT_PCM32: sample_size = 4; break;
    case AUDIO_FMT_FLOAT32: sample_size = 4; break;
    }

    size_t total_samples = count / sample_size;
    if (total_samples == 0) return 0;
    if (total_samples > AUDIO_MIX_BUF_SAMPLES * 2)
        total_samples = AUDIO_MIX_BUF_SAMPLES * 2;

    int16_t conv_buf[AUDIO_MIX_BUF_SAMPLES * 2];
    audio_convert_to_pcm16(samples, conv_buf, total_samples, s->format);

    size_t written = audio_ring_write(&s->ring, conv_buf, total_samples);
    if (s->state == AUDIO_STREAM_OPEN)
        s->state = AUDIO_STREAM_PLAYING;

    return (long)(written * sample_size);
}

int sys_audio_close(int stream_id) {
    if (stream_id < 0 || stream_id >= AUDIO_MAX_STREAMS) return -1;
    struct audio_stream *s = &g_audio_streams[stream_id];
    if (s->state == AUDIO_STREAM_FREE) return -1;

    if (s->ring.buf) {
        kfree(s->ring.buf);
        s->ring.buf = NULL;
    }
    s->state = AUDIO_STREAM_FREE;
    return 0;
}

int sys_audio_volume(int stream_id, uint8_t volume) {
    if (volume > AUDIO_MAX_VOLUME) volume = AUDIO_MAX_VOLUME;

    if (stream_id < 0) {
        g_audio_master_volume = volume;
        return 0;
    }
    if (stream_id >= AUDIO_MAX_STREAMS) return -1;

    struct audio_stream *s = &g_audio_streams[stream_id];
    if (s->state == AUDIO_STREAM_FREE) return -1;
    s->volume = volume;
    return 0;
}

/* Called periodically to push mixed audio to the DMA ring */
void audio_engine_pump(void) {
    if (!g_audio_engine_ready) return;

    size_t mixed = audio_mix(g_audio_out_buf, AUDIO_MIX_BUF_SAMPLES * 2);
    if (mixed > 0) {
        audio_ring_write(&g_audio_dma_ring, g_audio_out_buf, mixed);
    }
}

/* Provide audio data to the HDA DMA controller */
size_t audio_engine_get_dma_data(int16_t *buf, size_t max_samples) {
    if (!g_audio_engine_ready) return 0;
    return audio_ring_read(&g_audio_dma_ring, buf, max_samples);
}

void audio_engine_init(void) {
    memset(g_audio_streams, 0, sizeof(g_audio_streams));
    audio_ring_init(&g_audio_dma_ring, AUDIO_RING_SIZE / sizeof(int16_t));
    g_audio_engine_ready = true;
    kprintf("[audio_engine] initialized: %u Hz stereo 16-bit output\n",
           AUDIO_DEFAULT_RATE);
}
