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
#include <tobyos/audio_hda.h>

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
/* (there used to be a g_audio_dma_ring here -- a 64 KiB staging ring that
 * audio_engine_pump filled and NOTHING ever read. The HDA cyclic DMA
 * buffer is the real ring; this one was pure dead weight.) */
static uint32_t g_audio_master_volume = 80;
static bool g_audio_engine_ready;

/* Both defined below; sys_audio_write (further up) calls the tick. */
size_t audio_engine_pump(void);
void   audio_engine_tick(void);

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
        /* IEEE-754 binary32 in [-1.0, 1.0] -> int16, in pure integer ops
         * (no SSE, so no FXSAVE guard needed on this path).
         *
         * The previous version was broken twice over: both arms of its
         * if/else were the SAME expression `mantissa >> (23 - exp)`, and
         * the scale was wrong -- full-scale 1.0 (exp=0, mantissa=0x800000)
         * came out as 0x800000 >> 23 == 1, so every float32 sample decoded
         * to 0 or +-1. Float32 audio was silence. It also shifted by a
         * negative amount (UB) for exp > 23.
         *
         * Correct scale: value = mantissa/2^23 * 2^exp, and we want
         * value * 2^15, so val = mantissa >> (8 - exp), i.e. a LEFT shift
         * when exp > 8. Check: 1.0 -> 0x800000 >> 8 = 32768 -> clamps to
         * 32767; 0.5 -> 0x800000 >> 9 = 16384. */
        const uint32_t *s = (const uint32_t *)src;
        for (size_t i = 0; i < samples; i++) {
            uint32_t raw = s[i];
            int32_t  exp = (int32_t)((raw >> 23) & 0xFF) - 127;
            int32_t  mantissa = (int32_t)((raw & 0x7FFFFF) | 0x800000);
            int32_t  shift = 8 - exp;
            int32_t  val;
            if (shift >= 24)      val = 0;            /* denormal/too small */
            else if (shift >= 0)  val = mantissa >> shift;
            else if (shift > -8)  val = mantissa << (-shift);
            else                  val = 32767;        /* |x| >= 256: clamp  */
            if (raw & 0x80000000u) val = -val;
            if (val > 32767)  val = 32767;
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

    /* Longest contribution from any stream. The mix used to return the
     * full out_samples no matter how little it actually read, so a stream
     * holding 100 samples produced 100 samples of audio followed by 1948
     * samples of injected silence -- a click on every short write. Only
     * the region a stream really covered may be emitted. */
    size_t valid = 0;
    bool any_active = false;
    for (int i = 0; i < AUDIO_MAX_STREAMS; i++) {
        struct audio_stream *s = &g_audio_streams[i];
        if (s->state != AUDIO_STREAM_PLAYING) continue;
        if (s->ring.count == 0) continue;

        /* static, not automatic: these are 4 KiB EACH and were declared
         * inside the loop, so a mix pass put 8 KiB on the kernel stack.
         * audio_mix has exactly one caller and runs under the BKL, so
         * file scope is safe and the stack stays shallow. */
        static int16_t tmp[AUDIO_MIX_BUF_SAMPLES * 2];
        size_t read = audio_ring_read(&s->ring, tmp, out_samples);
        if (read == 0) continue;

        /* Resample if needed */
        static int16_t resampled[AUDIO_MIX_BUF_SAMPLES * 2];
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
        size_t n = src_count < out_samples ? src_count : out_samples;
        for (size_t j = 0; j < n; j++) {
            g_audio_mix_buf[j] += ((int32_t)src_data[j] * (int32_t)vol) / 100;
        }
        if (n > valid) valid = n;
        any_active = true;
    }

    if (!any_active || valid == 0) return 0;

    /* Keep the emitted length an exact number of stereo frames; a half
     * frame would swap the channels for the rest of the stream. */
    valid &= ~(size_t)1;
    if (valid == 0) return 0;

    /* Clip and output */
    for (size_t i = 0; i < valid; i++) {
        int32_t val = g_audio_mix_buf[i];
        if (val > 32767) val = 32767;
        if (val < -32768) val = -32768;
        out[i] = (int16_t)val;
    }
    return valid;
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
            /* Bring the hardware stream up on the first open. The mixer
             * always emits AUDIO_DEFAULT_RATE stereo (per-stream rates are
             * resampled in audio_mix), so the device is opened at the
             * mixer's rate, not this stream's. */
            if (!audio_hda_pcm_is_open()) {
                if (audio_hda_pcm_open(AUDIO_DEFAULT_RATE, 2) != 0)
                    kprintf("[audio] no HDA output -- stream %d is a "
                            "silent sink\n", i);
            }
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

    /* A zero-length write is a FLUSH: mix whatever is already queued into
     * the device and return. Callers need this because the pump is driven
     * by writers, and a stream's tail has no writer left -- pid 0's
     * audio_engine_tick covers a live desktop, but anything running before
     * idle_loop exists (the whole boot-time M26A harness, for one) would
     * otherwise have its last few hundred ms silently discarded at close. */
    if (count == 0) {
        /* audio_engine_tick, not just _pump: the tick ALSO tops the ring
         * up with silence once there is nothing left to mix. Without that
         * the cyclic BDL keeps RUN set and replays its last ~85 ms for as
         * long as the stream is open -- /bin/audioplay submitted 600 ms
         * and the capture held 1.024 s, the surplus being the ring looping
         * during the drain. A paused video would loop forever. */
        audio_engine_tick();
        return 0;
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

    /* The writer is the pump: drain the mixer into the hardware ring on
     * every write, so no timer or HDA interrupt is needed and the device
     * ring's backpressure paces the caller naturally. */
    audio_engine_pump();

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

    /* Last one out stops the DAC, so an idle desktop is not holding a
     * running DMA stream (and a real codec's output stays muted). */
    for (int i = 0; i < AUDIO_MAX_STREAMS; i++)
        if (g_audio_streams[i].state != AUDIO_STREAM_FREE) return 0;
    audio_hda_pcm_close();
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

/* Drain every playing stream through the mixer and into the hardware.
 *
 * THIS IS THE JUNCTION THAT WAS MISSING. The old body mixed into
 * g_audio_dma_ring, and the only reader of that ring was
 * audio_engine_get_dma_data(), which had ZERO callers anywhere in the
 * tree -- so mixed audio accumulated in a buffer nothing ever drained
 * and no sound could physically leave the machine. The ring is now the
 * HDA cyclic DMA buffer itself.
 *
 * Returns frames pushed to the device this pass. Called from
 * sys_audio_write (a push model: the writer is the pump), so no timer or
 * IRQ is required -- the HDA ring's backpressure paces the writer. */
size_t audio_engine_pump(void) {
    if (!g_audio_engine_ready) return 0;
    if (!audio_hda_pcm_is_open()) return 0;

    size_t pushed = 0;
    /* Loop so a writer that hands us a big buffer at once still gets it
     * all in front of the DAC, rather than one mix-buffer per write.
     *
     * ASK THE DEVICE FOR SPACE FIRST. audio_mix() is destructive -- it
     * drains the per-stream rings -- so mixing a full buffer and only
     * then discovering the device is full throws that audio away with no
     * error anywhere. That is exactly what the first cut of this loop did:
     * /bin/audioplay submitted 600 ms, reported "0 short writes, 0 stalls"
     * because the per-stream ring accepted everything, and precisely one
     * device ring (16384 B, ~85 ms) ever reached the DAC. The rest was
     * mixed and dropped.
     *
     * Capping the mix at the device's free space instead means a full
     * device leaves the stream rings untouched, they back up, the short
     * return from audio_ring_write propagates out of sys_audio_write, and
     * the writer finally sees the backpressure it is supposed to pace on. */
    for (;;) {
        long free_fr = audio_hda_pcm_free();
        if (free_fr <= 0) break;                  /* device ring is full */

        size_t cap = (size_t)free_fr * 2;         /* frames -> samples   */
        if (cap > AUDIO_MIX_BUF_SAMPLES * 2) cap = AUDIO_MIX_BUF_SAMPLES * 2;

        size_t mixed = audio_mix(g_audio_out_buf, cap);
        if (mixed == 0) break;                    /* nothing left to mix */

        long w = audio_hda_pcm_write(g_audio_out_buf, mixed / 2);
        if (w <= 0) break;
        pushed += (size_t)w;
    }
    return pushed;
}

/* Frames still queued ahead of the DAC; the pacing signal for a writer. */
long audio_engine_pending(void) {
    if (!g_audio_engine_ready) return 0;
    return audio_hda_pcm_pending();
}

/* Periodic service call from pid 0's idle loop (under the BKL).
 *
 * Two jobs the push-from-write path cannot do:
 *   1. DRAIN THE TAIL. After an app's last write there is no writer left
 *      to move what is still sitting in the per-stream rings, so the end
 *      of every sound was being dropped at close.
 *   2. FEED SILENCE ON UNDERRUN. The BDL is cyclic and RUN stays set, so
 *      a starved stream does not go quiet -- the controller loops over
 *      whatever bytes are still in the ring and replays the last ~85 ms
 *      forever. Writing zeroes ahead of the hardware cursor is what makes
 *      an idle stream actually silent.
 *
 * Returns immediately when no stream is open, so an idle desktop pays
 * one predicate per idle iteration. */
void audio_engine_tick(void) {
    if (!g_audio_engine_ready) return;
    if (!audio_hda_pcm_is_open()) return;

    /* ONLY service a stream this engine actually owns.
     *
     * The HDA ring can also be opened straight from the ALSA layer
     * (snd_pcm.c calls audio_hda_pcm_open on HW_PARAMS), and then these
     * samples are not ours to write. Filling silence regardless kept the
     * ring permanently full of zeros: chrome's snd_pcm_writei got EAGAIN
     * forever, its audio clock froze (AudioContext currentTime stuck at
     * 0.47 while state stayed "running"), SyncReader timed out, and the
     * capture was 30 seconds of digital silence from a stream that was
     * genuinely running. Nothing looked broken anywhere -- which is what
     * made it expensive.
     *
     * /bin/linux-alsatest never hit this only because the boot harness
     * runs before idle_loop exists, so the tick was not yet alive. */
    bool mine = false;
    for (int i = 0; i < AUDIO_MAX_STREAMS; i++)
        if (g_audio_streams[i].state != AUDIO_STREAM_FREE) { mine = true; break; }
    if (!mine) return;

    if (audio_engine_pump() > 0) return;      /* real audio went out */

    /* Nothing to mix: keep the ring topped up with silence rather than
     * letting the DAC loop over stale samples. */
    long free_fr = audio_hda_pcm_free();
    if (free_fr <= 0) return;
    if (free_fr > AUDIO_MIX_BUF_SAMPLES) free_fr = AUDIO_MIX_BUF_SAMPLES;
    memset(g_audio_out_buf, 0, (size_t)free_fr * 2 * sizeof(int16_t));
    audio_hda_pcm_write(g_audio_out_buf, (size_t)free_fr);
}

void audio_engine_init(void) {
    memset(g_audio_streams, 0, sizeof(g_audio_streams));
    g_audio_engine_ready = true;
    kprintf("[audio_engine] initialized: %u Hz stereo 16-bit output "
            "(hda=%s)\n", AUDIO_DEFAULT_RATE,
            audio_hda_present() ? "present" : "absent");
}
