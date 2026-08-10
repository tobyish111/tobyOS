/* programs/linux-alsatest/main.c
 *
 * Audio slice 4: play a tone through the REAL, UNMODIFIED alsa-lib.
 *
 * Slice 3 proved our kernel-side ALSA ABI against a hand-rolled test that
 * spoke the ioctls directly. That guards our own ideas about the ABI, not
 * the ABI itself -- a test and a kernel written by the same author agree
 * with each other by construction. This closes that gap: upstream
 * alsa-lib 1.2.8, byte-for-byte the library Debian ships, drives our
 * /dev/snd node with no knowledge that tobyOS is not Linux.
 *
 * It reaches the library by dlopen + dlsym rather than by linking, for one
 * reason: THAT IS EXACTLY WHAT CHROMIUM DOES. Chrome's
 * AlsaWrapper/media::AlsaPcmOutputStream dlopens libasound.so.2 and
 * resolves 37 snd_pcm_* symbols by name (verified by string-scanning the
 * chrome binary). Matching that means a pass here is direct evidence for
 * slice 5 rather than merely adjacent to it.
 *
 * Emits a 1000 Hz square wave for ~600 ms, matching /bin/audioplay and
 * /bin/linux-sndtest, so all three paths land as comparable bursts in the
 * same QEMU wav capture.
 *
 * exit: 0 = PASS, 1 = dlopen/dlsym failure, 2 = an ALSA call failed,
 *       3 = the stream never advanced.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>

#define RATE      48000
#define CHANS         2
#define TONE_HZ    1000
#define AMP       10000
#define PERIOD      480          /* 10 ms; a whole number of 1000 Hz periods */
#define CHUNKS       60          /* 600 ms total */

/* Opaque to us, exactly as they are to Chromium. */
typedef struct _snd_pcm snd_pcm_t;
typedef struct _snd_pcm_hw_params snd_pcm_hw_params_t;

/* The subset of the 37 symbols Chrome resolves that a playback path needs. */
static const char *(*p_strerror)(int);
static int   (*p_open)(snd_pcm_t **, const char *, int, int);
static int   (*p_close)(snd_pcm_t *);
static int   (*p_prepare)(snd_pcm_t *);
static int   (*p_drain)(snd_pcm_t *);
static long  (*p_writei)(snd_pcm_t *, const void *, unsigned long);
static long  (*p_avail_update)(snd_pcm_t *);
static int   (*p_recover)(snd_pcm_t *, int, int);
static int   (*p_set_params)(snd_pcm_t *, int, int, unsigned, unsigned,
                             int, unsigned);
static int   (*p_hw_params_malloc)(snd_pcm_hw_params_t **);
static void  (*p_hw_params_free)(snd_pcm_hw_params_t *);
static int   (*p_hw_params_any)(snd_pcm_t *, snd_pcm_hw_params_t *);
static int   (*p_hw_params_set_access)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
static int   (*p_hw_params_set_format)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
static int   (*p_hw_params_set_channels)(snd_pcm_t *, snd_pcm_hw_params_t *,
                                         unsigned);
static int   (*p_hw_params_set_rate_near)(snd_pcm_t *, snd_pcm_hw_params_t *,
                                          unsigned *, int *);
static int   (*p_hw_params)(snd_pcm_t *, snd_pcm_hw_params_t *);
static const char *(*p_name)(snd_pcm_t *);

/* alsa-lib enum values (stable ABI constants). */
#define SND_PCM_STREAM_PLAYBACK      0
#define SND_PCM_ACCESS_RW_INTERLEAVED 3
#define SND_PCM_FORMAT_S16_LE         2

static void *g_lib;
static int   g_missing;

static void *sym(const char *n) {
    void *s = dlsym(g_lib, n);
    if (!s) { printf("FAIL: dlsym(%s) -> NULL\n", n); g_missing++; }
    return s;
}

static short g_tone[PERIOD * CHANS];

int main(void) {
    printf("[LXALSA] start\n");

    /* Chromium's exact call: dlopen("libasound.so.2", RTLD_LAZY). */
    g_lib = dlopen("libasound.so.2", RTLD_LAZY);
    if (!g_lib) {
        printf("FAIL: dlopen(libasound.so.2): %s\n", dlerror());
        return 1;
    }
    printf("[LXALSA] dlopen(libasound.so.2) OK\n");

    p_strerror   = sym("snd_strerror");
    p_open       = sym("snd_pcm_open");
    p_close      = sym("snd_pcm_close");
    p_prepare    = sym("snd_pcm_prepare");
    p_drain      = sym("snd_pcm_drain");
    p_writei     = sym("snd_pcm_writei");
    p_avail_update = sym("snd_pcm_avail_update");
    p_recover    = sym("snd_pcm_recover");
    p_set_params = sym("snd_pcm_set_params");
    p_name       = sym("snd_pcm_name");
    p_hw_params_malloc      = sym("snd_pcm_hw_params_malloc");
    p_hw_params_free        = sym("snd_pcm_hw_params_free");
    p_hw_params_any         = sym("snd_pcm_hw_params_any");
    p_hw_params_set_access  = sym("snd_pcm_hw_params_set_access");
    p_hw_params_set_format  = sym("snd_pcm_hw_params_set_format");
    p_hw_params_set_channels= sym("snd_pcm_hw_params_set_channels");
    p_hw_params_set_rate_near = sym("snd_pcm_hw_params_set_rate_near");
    p_hw_params             = sym("snd_pcm_hw_params");
    if (g_missing) {
        printf("FAIL: %d symbol(s) missing\n", g_missing);
        return 1;
    }
    printf("[LXALSA] resolved 18 snd_pcm_* symbols\n");

    /* Try three device names, least to most machinery, and report each.
     * They isolate WHERE a failure lives, which one name cannot:
     *   hw:0,0      the kernel device alone -- no alsa-lib plugin at all
     *   plughw:0,0  the plug converter directly over it
     *   default     the full config parse: alsa.conf -> /etc/asound.conf
     *               -> plug -> hw:0,0
     * If hw works and plug does not, the gap is in what we report to the
     * converter, not in the device. */
    static const char *names[] = { "hw:0,0", "plughw:0,0", "default" };
    snd_pcm_t *pcm = NULL;
    const char *chosen = NULL;
    int rc = -1;
    /* Probe each name and CLOSE it again before trying the next: the
     * kernel exposes a single substream, so holding the first open makes
     * every later name fail with ENOENT and look like a config problem
     * when it is only "the device is busy". Reopen the winner afterwards. */
    for (int i = 0; i < 3; i++) {
        snd_pcm_t *t = NULL;
        int r = p_open(&t, names[i], SND_PCM_STREAM_PLAYBACK, 0);
        if (r < 0) {
            printf("[LXALSA] open(%-10s) FAIL: %s\n", names[i], p_strerror(r));
            continue;
        }
        printf("[LXALSA] open(%-10s) OK\n", names[i]);
        p_close(t);
        if (!chosen) chosen = names[i];
    }
    if (chosen) rc = p_open(&pcm, chosen, SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) { printf("FAIL: every device name failed\n"); return 2; }
    printf("[LXALSA] using \"%s\" -> %s\n", chosen,
           p_name ? p_name(pcm) : "?");

    /* The long-hand hw_params walk, which is what drives HW_REFINE
     * repeatedly -- the part of our kernel ABI with real logic in it. */
    snd_pcm_hw_params_t *hw = NULL;
    if (p_hw_params_malloc(&hw) < 0) { printf("FAIL: hw_params_malloc\n"); return 2; }
    if ((rc = p_hw_params_any(pcm, hw)) < 0) {
        printf("FAIL: hw_params_any: %s\n", p_strerror(rc)); return 2;
    }
    if ((rc = p_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        printf("FAIL: set_access: %s\n", p_strerror(rc)); return 2;
    }
    if ((rc = p_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0) {
        printf("FAIL: set_format: %s\n", p_strerror(rc)); return 2;
    }
    if ((rc = p_hw_params_set_channels(pcm, hw, CHANS)) < 0) {
        printf("FAIL: set_channels: %s\n", p_strerror(rc)); return 2;
    }
    unsigned rate = RATE; int dir = 0;
    if ((rc = p_hw_params_set_rate_near(pcm, hw, &rate, &dir)) < 0) {
        printf("FAIL: set_rate_near: %s\n", p_strerror(rc)); return 2;
    }
    if ((rc = p_hw_params(pcm, hw)) < 0) {
        printf("FAIL: hw_params commit: %s\n", p_strerror(rc)); return 2;
    }
    p_hw_params_free(hw);
    printf("[LXALSA] hw_params: rate=%u channels=%d S16_LE interleaved\n",
           rate, CHANS);

    if ((rc = p_prepare(pcm)) < 0) {
        printf("FAIL: prepare: %s\n", p_strerror(rc)); return 2;
    }

    for (int i = 0; i < PERIOD; i++) {
        short v = ((i % (RATE / TONE_HZ)) < (RATE / TONE_HZ / 2))
                  ? (short)AMP : (short)-AMP;
        g_tone[i * CHANS + 0] = v;
        g_tone[i * CHANS + 1] = v;
    }

    long total = 0;
    int  recovered = 0;
    for (int c = 0; c < CHUNKS; c++) {
        long done = 0;
        int guard = 0;
        while (done < PERIOD) {
            long w = p_writei(pcm, &g_tone[done * CHANS],
                              (unsigned long)(PERIOD - done));
            if (w == -11 /* -EAGAIN */) {
                /* NOT an error and NOT something to recover from: the ring
                 * is full and we are ahead of the DAC. tobyOS's PCM is
                 * effectively always non-blocking -- a syscall must not
                 * park while holding the BKL -- so this is the ordinary
                 * backpressure signal. Handing it to snd_pcm_recover
                 * (which only knows about EPIPE/ESTRPIPE) just returns it
                 * unchanged and looks like a hard failure. */
                if (++guard > 4000) { printf("FAIL: writei stuck\n"); return 3; }
                usleep(2000);
                continue;
            }
            if (w < 0) {
                /* Real faults -- underrun, suspend -- go to snd_pcm_recover,
                 * exactly as Chromium's AlsaPcmOutputStream does. */
                recovered++;
                if (p_recover(pcm, (int)w, 1) < 0) {
                    printf("FAIL: writei %ld unrecoverable: %s\n",
                           w, p_strerror((int)w));
                    return 2;
                }
                if (++guard > 4000) { printf("FAIL: writei stuck\n"); return 3; }
                usleep(2000);
                continue;
            }
            if (w == 0) {
                if (++guard > 4000) { printf("FAIL: no progress\n"); return 3; }
                usleep(2000);
                continue;
            }
            done  += w;
            total += w;
        }
    }
    printf("[LXALSA] wrote %ld frames (%ld ms), %d recover(s)\n",
           total, total * 1000 / RATE, recovered);

    long avail = p_avail_update(pcm);
    printf("[LXALSA] avail_update -> %ld\n", avail);

    p_drain(pcm);
    p_close(pcm);

    if (total != (long)PERIOD * CHUNKS) {
        printf("FAIL: expected %d frames, wrote %ld\n", PERIOD * CHUNKS, total);
        return 3;
    }
    printf("[LXALSA] VERDICT: PASS\n");
    return 0;
}
