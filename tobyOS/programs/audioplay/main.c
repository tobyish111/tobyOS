/* programs/audioplay/main.c -- the PLAYBACK validator (audio slice 2).
 *
 * audiotest checks that the HDA controller and codec answer probes.
 * This checks the thing that actually matters: that bytes handed to
 * sys_audio_write() come out of the speakers.
 *
 * Until slice 2 that was impossible by construction -- audio_engine
 * mixed into a ring buffer whose only reader, audio_engine_get_dma_data(),
 * had zero callers in the entire tree, so userland audio physically could
 * not reach the DAC.
 *
 * It plays a 1000 Hz stereo square wave for ~600 ms. 1000 Hz is chosen so
 * that at 48 kHz one period is exactly 48 frames -- a whole number -- so
 * the emitted tone has no wrap discontinuity and logs/wavcheck.py can
 * measure it to a fraction of a percent. (The old 880 Hz tone test used
 * 1024 frames holding 18.96 periods, which reads ~2% low for that reason.)
 *
 * Verify with:
 *   qemu ... -audiodev wav,id=snd0,path=out.wav \
 *            -device intel-hda -device hda-output,audiodev=snd0
 *   python logs/wavcheck.py out.wav --expect-hz 1000
 *
 * Exit codes: 0 = tone submitted in full, 1 = open or write failed.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

/* The audio syscalls have no public libtoby header -- libtoby/src/audio.c
 * defines toby_audio_* but nothing declares them, so the native browser
 * rolls its own inline wrappers too (user_gui_browser/main.c:340). Match
 * that rather than reshape the libc surface from inside a test. */
#define SYS_AUDIO_OPEN   143
#define SYS_AUDIO_WRITE  144
#define SYS_AUDIO_CLOSE  145
#define SYS_AUDIO_VOLUME 146

static inline long sys_audio_open(uint32_t rate, int channels, int fmt) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r)
        : "0"((long)SYS_AUDIO_OPEN), "D"((long)rate), "S"((long)channels),
          "d"((long)fmt) : "rcx", "r11", "memory");
    return r;
}
static inline long sys_audio_write(int fd, const void *s, unsigned long n) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r)
        : "0"((long)SYS_AUDIO_WRITE), "D"((long)fd), "S"(s), "d"(n)
        : "rcx", "r11", "memory");
    return r;
}
static inline long sys_audio_close(int fd) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r)
        : "0"((long)SYS_AUDIO_CLOSE), "D"((long)fd) : "rcx", "r11", "memory");
    return r;
}
static inline long sys_audio_volume(int fd, int vol) {
    long r;
    __asm__ volatile ("syscall" : "=a"(r)
        : "0"((long)SYS_AUDIO_VOLUME), "D"((long)fd), "S"((long)vol)
        : "rcx", "r11", "memory");
    return r;
}

#define RATE       48000
#define TONE_HZ     1000
#define CHANNELS        2
#define AMPLITUDE   10000        /* well under full scale: no clipping   */
#define PERIOD_FR  (RATE / TONE_HZ)          /* exactly 48 frames        */
#define CHUNK_FR      480        /* 10 ms per write, 10 whole periods    */
#define TOTAL_MS      600

int main(void) {
    static short chunk[CHUNK_FR * CHANNELS];

    /* One chunk is a whole number of periods, so chunks can be repeated
     * back-to-back forever with no phase discontinuity at the seams. */
    for (int i = 0; i < CHUNK_FR; i++) {
        short v = ((i % PERIOD_FR) < (PERIOD_FR / 2))
                  ? (short)AMPLITUDE : (short)-AMPLITUDE;
        chunk[i * CHANNELS + 0] = v;
        chunk[i * CHANNELS + 1] = v;
    }

    int fd = (int)sys_audio_open(RATE, CHANNELS, 1 /* AUDIO_FMT_PCM16 */);
    if (fd < 0) {
        printf("FAIL: audioplay: sys_audio_open -> %d\n", fd);
        return 1;
    }
    printf("INFO: audioplay: stream %d open, %d Hz %d ch, tone %d Hz "
           "(%d frames/period)\n", fd, RATE, CHANNELS, TONE_HZ, PERIOD_FR);
    sys_audio_volume(fd, 100);

    const int chunks = (TOTAL_MS * RATE / 1000) / CHUNK_FR;
    const long chunk_bytes = (long)sizeof chunk;
    long submitted = 0;
    int  short_writes = 0, stalls = 0;

    for (int c = 0; c < chunks; c++) {
        long off = 0;
        /* The device ring holds ~85 ms; a short write means it is full and
         * we are ahead of the DAC, which is exactly the pacing signal.
         * Sleep a period's worth and try again rather than spinning. */
        while (off < chunk_bytes) {
            long w = sys_audio_write(fd, (const char *)chunk + off,
                                     (unsigned long)(chunk_bytes - off));
            if (w < 0) {
                printf("FAIL: audioplay: write -> %ld at chunk %d\n", w, c);
                sys_audio_close(fd);
                return 1;
            }
            if (w == 0) {
                stalls++;
                if (stalls > 10000) {
                    printf("FAIL: audioplay: ring never drained "
                           "(DMA not running?)\n");
                    sys_audio_close(fd);
                    return 1;
                }
                usleep(5000);
                continue;
            }
            if (w < chunk_bytes - off) short_writes++;
            off += w;
            submitted += w;
        }
    }

    /* DRAIN. close() does not drain -- it cannot, since a syscall must not
     * sit spinning while holding the BKL -- so the tail has to be pumped
     * from here. A zero-length write is the flush kick; sleeping alone is
     * not enough, because the kernel's periodic pump lives in pid 0's
     * idle_loop and this program is spawned by the boot harness, which
     * runs in kmain BEFORE idle_loop is ever entered. Worst case queued is
     * the per-stream ring (~340 ms) plus the device ring (~85 ms). */
    for (int i = 0; i < 60; i++) {
        sys_audio_write(fd, 0, 0);
        usleep(15000);
    }
    sys_audio_close(fd);

    long frames = submitted / (CHANNELS * 2);
    printf("PASS: audioplay: submitted %ld bytes = %ld frames = %ld ms "
           "@%d Hz (%d short writes, %d stalls)\n",
           submitted, frames, frames * 1000 / RATE, RATE,
           short_writes, stalls);
    return 0;
}
