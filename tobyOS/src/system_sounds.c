/* system_sounds.c -- play short procedurally-generated audio cues.
 *
 * Uses the existing audio_mixer API to submit synthesized waveforms
 * for system events (notification ding, error buzz, click, etc.).
 */

#include <tobyos/types.h>
#include <tobyos/printk.h>
#include <tobyos/klibc.h>
#include <tobyos/system_sounds.h>

/* Forward-declared mixer API (from audio_mixer.c) */
int      mixer_open_stream(int pid, uint32_t sample_rate, uint8_t format);
void     mixer_close_stream(int handle);
uint32_t mixer_write(int handle, const void *data, uint32_t frames);

#define MIXER_FMT_S16   0
#define SAMPLE_RATE     48000

static int  g_sounds_enabled = 1;
static int  g_sound_stream = -1;

/* Fixed-point sine approximation (Q15). Input: phase 0..65535 maps to 0..2*pi. */
static int16_t fast_sin(uint32_t phase) {
    phase &= 0xFFFF;
    int neg = 0;
    if (phase >= 0x8000) { phase -= 0x8000; neg = 1; }
    if (phase >= 0x4000) phase = 0x8000 - phase;

    /* Quadratic approximation of sin in [0, pi/2]:
     * sin(x) ~ (4/pi)*x - (4/pi^2)*x^2 ~ x*(1 - x/pi_half)
     * Scaled to Q15: phase 0..0x4000 -> 0..32767 */
    int64_t x = (int64_t)phase;
    int64_t val = (x * (0x4000 - x) * 4) >> 14;
    if (val > 32767) val = 32767;
    return neg ? (int16_t)(-val) : (int16_t)val;
}

/* Generate a tone burst into a stereo S16 buffer.
 * Returns number of frames written. */
static uint32_t gen_tone(int16_t *buf, uint32_t max_frames,
                         uint32_t freq_hz, uint32_t duration_ms,
                         int square, int fade_out) {
    uint32_t frames = (SAMPLE_RATE * duration_ms) / 1000;
    if (frames > max_frames) frames = max_frames;

    uint32_t phase_inc = (uint32_t)(((uint64_t)freq_hz << 16) / SAMPLE_RATE);
    uint32_t phase = 0;

    for (uint32_t i = 0; i < frames; i++) {
        int16_t sample;
        if (square) {
            sample = (phase & 0x8000) ? 8000 : -8000;
        } else {
            sample = (int16_t)((int32_t)fast_sin(phase) * 8000 / 32767);
        }

        if (fade_out && frames > 0) {
            int32_t env = (int32_t)(frames - i) * 256 / (int32_t)frames;
            sample = (int16_t)((int32_t)sample * env / 256);
        }

        buf[i * 2]     = sample;
        buf[i * 2 + 1] = sample;
        phase += phase_inc;
    }
    return frames;
}

/* Generate a chord (multiple frequencies summed) */
static uint32_t gen_chord(int16_t *buf, uint32_t max_frames,
                          const uint32_t *freqs, int nfreqs,
                          uint32_t duration_ms, int fade_out) {
    uint32_t frames = (SAMPLE_RATE * duration_ms) / 1000;
    if (frames > max_frames) frames = max_frames;

    uint32_t phases[4] = {0};
    uint32_t incs[4];
    int n = nfreqs > 4 ? 4 : nfreqs;
    for (int f = 0; f < n; f++)
        incs[f] = (uint32_t)(((uint64_t)freqs[f] << 16) / SAMPLE_RATE);

    for (uint32_t i = 0; i < frames; i++) {
        int32_t acc = 0;
        for (int f = 0; f < n; f++) {
            acc += (int32_t)fast_sin(phases[f]) * 6000 / 32767;
            phases[f] += incs[f];
        }
        acc /= n;

        if (fade_out && frames > 0) {
            int32_t env = (int32_t)(frames - i) * 256 / (int32_t)frames;
            acc = acc * env / 256;
        }

        int16_t s = (int16_t)(acc > 32767 ? 32767 : (acc < -32768 ? -32768 : acc));
        buf[i * 2]     = s;
        buf[i * 2 + 1] = s;
    }
    return frames;
}

/* Max buffer: 500ms at 48kHz stereo */
#define MAX_SOUND_FRAMES  (SAMPLE_RATE / 2)
static int16_t g_sound_buf[MAX_SOUND_FRAMES * 2];

static void play_buf(uint32_t frames) {
    if (g_sound_stream < 0)
        g_sound_stream = mixer_open_stream(-1, SAMPLE_RATE, MIXER_FMT_S16);
    if (g_sound_stream < 0) return;
    mixer_write(g_sound_stream, g_sound_buf, frames);
}

void system_sound_play(enum sys_sound snd) {
    if (!g_sounds_enabled) return;
    if ((unsigned)snd >= SND_COUNT) return;

    uint32_t frames = 0;

    switch (snd) {
    case SND_NOTIFICATION: {
        /* 800 Hz sine, 100ms, fade out */
        frames = gen_tone(g_sound_buf, MAX_SOUND_FRAMES, 800, 100, 0, 1);
        break;
    }
    case SND_ERROR: {
        /* 200 Hz square wave, 200ms */
        frames = gen_tone(g_sound_buf, MAX_SOUND_FRAMES, 200, 200, 1, 0);
        break;
    }
    case SND_WARNING: {
        /* 400 Hz sine, 150ms, fade out */
        frames = gen_tone(g_sound_buf, MAX_SOUND_FRAMES, 400, 150, 0, 1);
        break;
    }
    case SND_CLICK: {
        /* 1200 Hz sine, 20ms */
        frames = gen_tone(g_sound_buf, MAX_SOUND_FRAMES, 1200, 20, 0, 0);
        break;
    }
    case SND_STARTUP: {
        /* C major chord: C4(261) + E4(329) + G4(392) Hz, 500ms, fade out */
        uint32_t freqs[3] = {261, 329, 392};
        frames = gen_chord(g_sound_buf, MAX_SOUND_FRAMES, freqs, 3, 500, 1);
        break;
    }
    case SND_SHUTDOWN: {
        /* G major chord descending: G3(196) + B3(247) + D4(294) Hz, 400ms, fade out */
        uint32_t freqs[3] = {196, 247, 294};
        frames = gen_chord(g_sound_buf, MAX_SOUND_FRAMES, freqs, 3, 400, 1);
        break;
    }
    case SND_CONNECT: {
        /* Rising two-tone: 600Hz 80ms then 900Hz 80ms */
        uint32_t f1 = gen_tone(g_sound_buf, MAX_SOUND_FRAMES, 600, 80, 0, 0);
        uint32_t f2 = gen_tone(g_sound_buf + f1 * 2, MAX_SOUND_FRAMES - f1, 900, 80, 0, 1);
        frames = f1 + f2;
        break;
    }
    case SND_DISCONNECT: {
        /* Falling two-tone: 900Hz 80ms then 600Hz 80ms */
        uint32_t f1 = gen_tone(g_sound_buf, MAX_SOUND_FRAMES, 900, 80, 0, 0);
        uint32_t f2 = gen_tone(g_sound_buf + f1 * 2, MAX_SOUND_FRAMES - f1, 600, 80, 0, 1);
        frames = f1 + f2;
        break;
    }
    default:
        break;
    }

    if (frames > 0)
        play_buf(frames);
}

void system_sound_set_enabled(int enabled) {
    g_sounds_enabled = !!enabled;
}

int system_sound_is_enabled(void) {
    return g_sounds_enabled;
}

void system_sounds_init(void) {
    g_sounds_enabled = 1;
    g_sound_stream = -1;
    kprintf("[syssnd] system sounds initialized\n");
}
